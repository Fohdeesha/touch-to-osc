#include "input/reader.h"

#include "core/process.h"
#include "osc/osc.h"

#include <libevdev/libevdev.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <exception>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tto {

namespace {

void copy_text(char* dst, size_t cap, const std::string& s) { std::snprintf(dst, cap, "%s", s.c_str()); }

void drain_fd(int fd) {
    uint8_t buf[4096];
    while (read(fd, buf, sizeof buf) > 0) {
    }
}

const char* plural(int n) { return n == 1 ? "" : "s"; }

}

bool NodeClaims::claim(const std::string& sysname) {
    std::lock_guard<std::mutex> lock(m_);
    return held_.insert(sysname).second;
}

void NodeClaims::release(const std::string& sysname) {
    std::lock_guard<std::mutex> lock(m_);
    held_.erase(sysname);
}

bool NodeClaims::claimed(const std::string& sysname) {
    std::lock_guard<std::mutex> lock(m_);
    return held_.count(sysname) != 0;
}

DeviceReader::DeviceReader(DeviceConfig cfg, const std::string& prefix, DeviceShared& shared, int notify_fd, NodeClaims& claims, SysPaths paths)
    : cfg_(std::move(cfg)), prefix_(prefix), sh_(shared), notify_fd_(notify_fd), claims_(claims), paths_(std::move(paths)) {
    std::string base = prefix + "/" + cfg_.id + "/";
    addr_down_ = base + "down";
    addr_move_ = base + "move";
    addr_up_ = base + "up";
    addr_cancel_ = base + "cancel";
    enabled_ = cfg_.enabled;
    sh_.want_enabled.store(cfg_.enabled);
    sh_.silence_ms.store(cfg_.silence_ms);
    info_.enabled = enabled_;
}

DeviceReader::~DeviceReader() {
    join();
    close_device();
}

void DeviceReader::start() { thread_ = std::thread([this] { run(); }); }

void DeviceReader::join() {
    if (thread_.joinable()) thread_.join();
}

void DeviceReader::notify() {
    uint64_t one = 1;
    ssize_t ignored = write(notify_fd_, &one, sizeof one);
    (void)ignored;
}

void DeviceReader::event(EventKind kind, log::Level level, const char* fmt, ...) {
    ReaderEvent e;
    e.kind = kind;
    e.level = uint8_t(level);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);
    if (!sh_.events.push(e)) sh_.lost_events.fetch_add(1, std::memory_order_relaxed);
    notify();
}

void DeviceReader::publish_info() {
    if (!enabled_) {
        info_.state = DevState::disabled;
        copy_text(info_.reason, sizeof info_.reason, "turned off");
    } else {
        info_.state = base_state_;
        copy_text(info_.reason, sizeof info_.reason, base_reason_);
    }
    info_.enabled = enabled_;
    sh_.info.back() = info_;
    sh_.info.publish();
    notify();
}

void DeviceReader::set_state(DevState st, const std::string& reason) {
    if (st == base_state_ && reason == base_reason_) return;
    base_state_ = st;
    base_reason_ = reason;
    publish_info();
}

void DeviceReader::run() {
    set_thread_name(("touch-" + cfg_.id).c_str());
    sock_.reset(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    inotify_.reset(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (inotify_) inotify_add_watch(inotify_.get(), paths_.dev_input.c_str(), IN_CREATE | IN_ATTRIB | IN_MOVED_TO);
    publish_info();
    while (!sh_.stop.load()) {
        try {
            if (open_device()) {
                session();
                continue;
            }
            wait_for_change(2000);
        } catch (const std::exception& e) {
            recover(e.what());
        } catch (...) {
            recover("unknown error");
        }
    }
    info_.connected = false;
    info_.grabbed = false;
    publish_info();
}

void DeviceReader::recover(const char* what) {
    int n = 0;
    try {
        if (dev_) n = cancel("device lost");
    } catch (...) {
    }
    close_device();
    event(EventKind::device, log::Level::err, "%s: reading stopped on an internal error (%s); %d contact%s cancelled; trying again in a second", cfg_.id.c_str(), what, n,
          plural(n));
    try {
        set_state(DevState::read_error, std::string("internal error while reading (") + what + "); touch-to-osc tries again every second");
    } catch (...) {
    }
    wait_for_change(1000);
}

void DeviceReader::wait_for_change(int timeout_ms) {
    pollfd p[2] = {{inotify_ ? inotify_.get() : -1, POLLIN, 0}, {sh_.wake.get(), POLLIN, 0}};
    int r = poll(p, 2, timeout_ms);
    if (r <= 0) return;
    if (p[0].revents) drain_fd(p[0].fd);
    if (p[1].revents) {
        drain_fd(p[1].fd);
        if (sh_.cfg_gen.load() != cfg_gen_) {
            pull_config();
            set_state(DevState::restarting, "restarting after a settings change");
            publish_info();
        }
        apply_enabled();
    }
}

void DeviceReader::pull_config() {
    std::lock_guard<std::mutex> lock(sh_.cfg_mutex);
    cfg_ = sh_.next_cfg;
    cfg_gen_ = sh_.cfg_gen.load();
    info_.cfg_gen = cfg_gen_;
    warned_ambiguous_ = false;
}

bool DeviceReader::reconfigure() {
    DeviceConfig old = cfg_;
    pull_config();
    bool reopen = old.grab != cfg_.grab || old.has_usb != cfg_.has_usb || old.vendor != cfg_.vendor || old.product != cfg_.product || old.serial != cfg_.serial ||
                  old.port != cfg_.port;
    if (reopen) {
        int n = cancel("reconfigured");
        event(EventKind::device, log::Level::notice, "%s: restarting after a settings change; %d contact%s cancelled", cfg_.id.c_str(), n, plural(n));
        close_device();
        set_state(DevState::restarting, "restarting after a settings change");
        publish_info();
        return true;
    }
    bool moved = old.rotation != cfg_.rotation || old.mirror_x != cfg_.mirror_x || old.mirror_y != cfg_.mirror_y || old.calibration != cfg_.calibration;
    if (moved) {
        int n = cancel("reconfigured");
        event(EventKind::device, log::Level::notice, "%s: touch positions map differently after a settings change; %d contact%s cancelled", cfg_.id.c_str(), n,
              plural(n));
    }
    mapping_ = Mapping(cfg_, ax_, ay_);
    info_.aspect = mapping_.aspect();
    publish_info();
    return false;
}

bool DeviceReader::open_device() {
    auto nodes = scan_nodes(paths_);
    const NodeInfo* pick = nullptr;
    std::string why_not;
    int touchscreens = 0;
    bool any = false;
    for (const auto& n : nodes) {
        if (!matches(cfg_, n)) continue;
        any = true;
        std::string why;
        if (touch_kind(n, &why) == TouchKind::none) {
            if (why_not.empty()) why_not = "\"" + n.name + "\" (" + n.node + "), and " + why;
            continue;
        }
        ++touchscreens;
        if (!pick && !claims_.claimed(n.sysname)) pick = &n;
    }
    if (!any) {
        eacces_since_ = 0;
        set_state(DevState::not_connected,
                  "no input device matches " + cfg_.match_text() + ": plug the touchscreen in, or check the id with touch-to-osc --list");
        return false;
    }
    if (!pick) {
        if (touchscreens == 0)
            set_state(DevState::not_touchscreen, cfg_.match_text() + " is " + why_not + ": check usb and port in the config against touch-to-osc --list");
        else
            set_state(DevState::not_connected, "every touchscreen matching " + cfg_.match_text() + " is already read for another device in the config: set port to tell them apart");
        return false;
    }
    if (touchscreens > 1 && !warned_ambiguous_) {
        warned_ambiguous_ = true;
        event(EventKind::device, log::Level::warning, "%s: %d touchscreens match %s; reading %s on port %s. Set port in the config to choose", cfg_.id.c_str(),
              touchscreens, cfg_.match_text().c_str(), pick->node.c_str(), pick->usb_port.c_str());
    }
    int fd = open(pick->node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == EACCES || e == EPERM) {
            // a new node is root-only until udev applies its group a moment later: only a refusal that lasts is a fault
            Nanos now = monotonic_ns();
            if (!eacces_since_) eacces_since_ = now;
            if (now - eacces_since_ >= kSec)
                set_state(DevState::no_permission, "cannot open " + pick->node + ": permission denied; run touch-to-osc as root or as a member of the input group");
        } else if (e == ENOENT || e == ENODEV || e == ENXIO)
            set_state(DevState::not_connected, pick->node + " went away while opening it: check the touchscreen's USB cable");
        else
            set_state(DevState::read_error, "cannot open " + pick->node + ": " + errno_str(e) + "; unplug and replug the touchscreen");
        return false;
    }
    eacces_since_ = 0;
    // before anything is queued: changing the clock of a client with pending events makes the kernel flush them and report SYN_DROPPED
    int clk = CLOCK_MONOTONIC;
    clock_monotonic_ = ioctl(fd, EVIOCSCLOCKID, &clk) == 0;
    fd_.reset(fd);
    int rc = libevdev_new_from_fd(fd, &dev_);
    if (rc < 0) {
        dev_ = nullptr;
        fd_.reset();
        set_state(DevState::read_error, "cannot read " + pick->node + ": " + errno_str(-rc) + "; unplug and replug the touchscreen");
        return false;
    }
    if (!claims_.claim(pick->sysname)) {
        libevdev_free(dev_);
        dev_ = nullptr;
        fd_.reset();
        return false;
    }
    sysname_ = pick->sysname;
    node_ = pick->node;
    kind_ = touch_kind(*pick);
    copy_text(info_.node, sizeof info_.node, pick->node);
    copy_text(info_.name, sizeof info_.name, printable(pick->name));
    copy_text(info_.usb, sizeof info_.usb, usb_identity(*pick));
    copy_text(info_.port, sizeof info_.port, pick->usb_port.empty() ? printable(pick->phys) : pick->usb_port);
    copy_text(info_.usb_port, sizeof info_.usb_port, pick->usb_port);
    return true;
}

void DeviceReader::close_device() {
    if (dev_) {
        libevdev_free(dev_);
        dev_ = nullptr;
    }
    fd_.reset();
    if (!sysname_.empty()) claims_.release(sysname_);
    sysname_.clear();
    info_.connected = false;
    info_.grabbed = false;
}

void DeviceReader::read_raw() {
    bool touched = false;
    if (kind_ == TouchKind::multitouch) {
        for (int s = 0; s < tracker_.slots(); ++s) {
            RawSlot& r = tracker_.raw(s);
            r.tracking = libevdev_get_slot_value(dev_, unsigned(s), ABS_MT_TRACKING_ID);
            r.x = libevdev_get_slot_value(dev_, unsigned(s), ABS_MT_POSITION_X);
            r.y = libevdev_get_slot_value(dev_, unsigned(s), ABS_MT_POSITION_Y);
            r.palm = has_tool_ && libevdev_get_slot_value(dev_, unsigned(s), ABS_MT_TOOL_TYPE) == MT_TOOL_PALM;
            touched = touched || r.tracking != -1;
        }
    } else {
        bool down = libevdev_get_event_value(dev_, EV_KEY, BTN_TOUCH) != 0;
        if (down && !single_down_) single_seq_ = single_seq_ == INT32_MAX ? 0 : single_seq_ + 1;
        single_down_ = down;
        RawSlot& r = tracker_.raw(0);
        r.tracking = down ? single_seq_ : -1;
        r.x = libevdev_get_event_value(dev_, EV_ABS, ABS_X);
        r.y = libevdev_get_event_value(dev_, EV_ABS, ABS_Y);
        r.palm = false;
        touched = down;
    }
    raw_touched_ = touched;
}

void DeviceReader::publish_active() {
    ActiveSnap& s = sh_.active.back();
    s.frame = frame_;
    s.n = tracker_.active_ids(s.ids);
    sh_.active.publish();
    sh_.contacts.store(s.n, std::memory_order_relaxed);
    sh_.oldest_down_ns.store(tracker_.oldest_down(), std::memory_order_relaxed);
}

void DeviceReader::send(int n, Nanos frame_t) {
    frame_ = frame_ == INT32_MAX ? 1 : frame_ + 1;
    int32_t frame = frame_;
    Nanos mono = monotonic_ns();
    int64_t t_us = ((frame_t ? frame_t : mono) + (realtime_ns() - mono)) / kUs;
    osc::Writer w(buf_, sizeof buf_);
    w.begin_bundle();
    uint64_t downs = 0, cancels = 0;
    for (int i = 0; i < n; ++i) {
        const TouchEvent& e = evs_[i];
        if (e.phase == Phase::cancel) {
            w.begin_message(addr_cancel_.c_str(), "isih");
            w.i(e.contact);
            w.s(e.reason);
            w.i(frame);
            w.h(t_us);
            w.end_message();
            ++cancels;
            continue;
        }
        float x, y;
        mapping_.map(e.x, e.y, x, y);
        const std::string& a = e.phase == Phase::down ? addr_down_ : e.phase == Phase::move ? addr_move_ : addr_up_;
        w.begin_message(a.c_str(), "iffih");
        w.i(e.contact);
        w.f(x);
        w.f(y);
        w.i(frame);
        w.h(t_us);
        w.end_message();
        if (e.phase == Phase::down) ++downs;
    }
    if (downs) sh_.touches.fetch_add(downs, std::memory_order_relaxed);
    if (cancels) sh_.cancels.fetch_add(cancels, std::memory_order_relaxed);
    if (w.ok()) {
        const DestSet& d = sh_.dests.latest();
        for (uint32_t i = 0; i < d.n; ++i) {
            if (sendto(sock_.get(), w.data(), w.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&d.addr[i]), sizeof d.addr[i]) < 0)
                sh_.send_drops.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        sh_.send_drops.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame_t) {
        Nanos lat = monotonic_ns() - frame_t;
        if (lat >= 0) sh_.latency_us.push(uint32_t(std::min<Nanos>(lat / kUs, UINT32_MAX)));
    }
    publish_active();
}

int DeviceReader::cancel(const char* reason) {
    int n = tracker_.cancel_all(reason, evs_);
    if (n) send(n, 0);
    else publish_active();
    return n;
}

void DeviceReader::on_frame(const input_event& ev) {
    Nanos t = Nanos(ev.input_event_sec) * kSec + Nanos(ev.input_event_usec) * kUs;
    if (!clock_monotonic_) t += monotonic_ns() - realtime_ns();
    ++session_frames_;
    sh_.frames.fetch_add(1, std::memory_order_relaxed);
    if (raw_touched_ && prev_frame_ && t > prev_frame_) {
        sh_.touched_frames.fetch_add(1, std::memory_order_relaxed);
        sh_.touched_ns.fetch_add(t - prev_frame_, std::memory_order_relaxed);
    }
    prev_frame_ = t;
    last_frame_ = t;
    sh_.last_frame_ns.store(t, std::memory_order_relaxed);
    read_raw();
    if (!enabled_) return;
    int n = tracker_.end_frame(evs_, t);
    if (n) send(n, t);
}

void DeviceReader::on_resync() {
    read_raw();
    if (session_frames_ == 0) {
        tracker_.mute_down_slots();
        return;
    }
    sh_.resyncs.fetch_add(1, std::memory_order_relaxed);
    if (!enabled_) return;
    int n = cancel("resync");
    event(EventKind::resync, log::Level::warning,
          "%s: the kernel dropped touch events because they were not read in time; state read again, %d contact%s cancelled. If this repeats, the machine is overloaded",
          cfg_.id.c_str(), n, plural(n));
}

void DeviceReader::on_released() {
    if (!enabled_) return;
    int n = cancel("device lost");
    event(EventKind::device, log::Level::warning, "%s: the kernel released the touchscreen (it is going away or was reset); %d contact%s cancelled", cfg_.id.c_str(), n,
          plural(n));
}

int DeviceReader::drain() {
    for (;;) {
        input_event ev{};
        int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                // value 1 marks the kernel's own release of every key when the device goes away or is reset: not a lift
                if (ev.value == 1) on_released();
                else on_frame(ev);
            }
            continue;
        }
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            while (rc == LIBEVDEV_READ_STATUS_SYNC) rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_SYNC, &ev);
            on_resync();
            if (rc == -EAGAIN) continue;
            if (rc < 0) return rc;
            continue;
        }
        if (rc == -EAGAIN) return 0;
        return rc < 0 ? rc : -EIO;
    }
}

bool DeviceReader::silent(Nanos now) const {
    int32_t ms = sh_.silence_ms.load(std::memory_order_relaxed);
    return enabled_ && ms > 0 && tracker_.announced() > 0 && now - last_frame_ >= Nanos(ms) * kMs;
}

void DeviceReader::apply_enabled() {
    bool want = sh_.want_enabled.load();
    if (want == enabled_) return;
    if (!want) {
        int n = dev_ ? cancel("device disabled") : 0;
        enabled_ = false;
        event(EventKind::enabled, log::Level::notice, "%s turned off; %d contact%s cancelled", cfg_.id.c_str(), n, plural(n));
    } else {
        if (dev_) {
            read_raw();
            tracker_.mute_down_slots();
        }
        enabled_ = true;
        event(EventKind::enabled, log::Level::notice, "%s turned back on", cfg_.id.c_str());
    }
    publish_info();
}

bool DeviceReader::retry_grab() {
    if (libevdev_grab(dev_, LIBEVDEV_GRAB) != 0) return false;
    libevdev_grab(dev_, LIBEVDEV_UNGRAB);
    event(EventKind::grab, log::Level::notice, "%s: the other program let go of %s; reading it afresh", cfg_.id.c_str(), node_.c_str());
    return true;
}

bool DeviceReader::foreign_grab() {
    if (cfg_.grab) return false;
    if (libevdev_grab(dev_, LIBEVDEV_GRAB) != 0) return true;
    libevdev_grab(dev_, LIBEVDEV_UNGRAB);
    return false;
}

void DeviceReader::session() {
    libevdev_set_clock_id(dev_, CLOCK_MONOTONIC);
    session_frames_ = 0;
    bool foreign = false;
    info_.grabbed = false;
    if (libevdev_grab(dev_, LIBEVDEV_GRAB) == 0) {
        if (cfg_.grab) info_.grabbed = true;
        else libevdev_grab(dev_, LIBEVDEV_UNGRAB);
    } else {
        foreign = true;
    }
    Axis& ax = ax_;
    Axis& ay = ay_;
    auto axis = [&](unsigned code) {
        Axis a;
        if (const input_absinfo* i = libevdev_get_abs_info(dev_, code)) {
            a.min = i->minimum;
            a.max = i->maximum;
            a.resolution = i->resolution;
        }
        return a;
    };
    int slots = 1;
    if (kind_ == TouchKind::multitouch) {
        slots = std::clamp(libevdev_get_num_slots(dev_), 1, kMaxSlots);
        ax = axis(ABS_MT_POSITION_X);
        ay = axis(ABS_MT_POSITION_Y);
        has_tool_ = libevdev_has_event_code(dev_, EV_ABS, ABS_MT_TOOL_TYPE);
    } else {
        ax = axis(ABS_X);
        ay = axis(ABS_Y);
        has_tool_ = false;
    }
    mapping_ = Mapping(cfg_, ax, ay);
    tracker_.reset(slots);
    single_down_ = false;
    read_raw();
    tracker_.mute_down_slots();
    prev_frame_ = 0;
    last_frame_ = monotonic_ns();
    enabled_ = sh_.want_enabled.load();
    info_.connected = true;
    info_.slots = slots;
    info_.w_mm = mapping_.width_mm();
    info_.h_mm = mapping_.height_mm();
    info_.aspect = mapping_.aspect();
    publish_active();
    if (foreign)
        set_state(DevState::grab_refused, "another program holds " + node_ + " exclusively, so no touches reach touch-to-osc: stop that program (fuser -v " + node_ + " names it)");
    else
        set_state(DevState::ok, "");
    publish_info();
    event(EventKind::device, log::Level::notice, "%s connected: %s (usb %s) on port %s, %s%s", cfg_.id.c_str(), info_.name, info_.usb, info_.port, node_.c_str(),
          foreign ? ", but another program holds it" : info_.grabbed ? "" : ", not grabbed (grab = false)");

    for (;;) {
        int timeout = -1;
        Nanos now = monotonic_ns();
        int32_t silence = sh_.silence_ms.load(std::memory_order_relaxed);
        if (enabled_ && silence > 0 && tracker_.announced() > 0) {
            Nanos due = last_frame_ + Nanos(silence) * kMs;
            timeout = int(std::max<Nanos>(0, (due - now + kMs - 1) / kMs));
        }
        if (base_state_ == DevState::grab_refused) timeout = timeout < 0 ? 2000 : std::min(timeout, 2000);
        pollfd p[2] = {{fd_.get(), POLLIN, 0}, {sh_.wake.get(), POLLIN, 0}};
        int r = poll(p, 2, timeout);
        if (r < 0 && errno != EINTR) {
            int e = errno;
            int n = cancel("device lost");
            event(EventKind::device, log::Level::err, "%s lost: waiting for it failed (%s); %d contact%s cancelled", cfg_.id.c_str(), errno_str(e).c_str(), n, plural(n));
            close_device();
            set_state(DevState::read_error, "waiting for " + node_ + " failed: " + errno_str(e));
            wait_for_change(500);
            return;
        }
        if (r > 0 && p[1].revents) {
            drain_fd(p[1].fd);
            if (sh_.stop.load()) {
                const char* why = sh_.restarting.load() ? "restarting" : "stopping";
                int n = cancel(why);
                if (n) event(EventKind::device, log::Level::notice, "%s: %s; %d contact%s cancelled", cfg_.id.c_str(), why, n, plural(n));
                close_device();
                return;
            }
            if (sh_.cfg_gen.load() != cfg_gen_ && reconfigure()) return;
            apply_enabled();
        }
        int rc = 0;
        if (r == 0 || (r > 0 && p[0].revents)) rc = drain();
        if (rc < 0) {
            int n = cancel("device lost");
            if (rc == -ENODEV) {
                event(EventKind::device, log::Level::warning, "%s lost: unplugged or dropped off USB; %d contact%s cancelled", cfg_.id.c_str(), n, plural(n));
                close_device();
                set_state(DevState::not_connected, "the touchscreen dropped off USB: check its cable; it is read again as soon as it is back");
            } else {
                event(EventKind::device, log::Level::err, "%s lost: reading %s failed (%s); %d contact%s cancelled", cfg_.id.c_str(), node_.c_str(), errno_str(-rc).c_str(), n,
                      plural(n));
                close_device();
                set_state(DevState::read_error, "reading " + node_ + " failed: " + errno_str(-rc) + "; unplug and replug the touchscreen");
                wait_for_change(500);
            }
            return;
        }
        now = monotonic_ns();
        if (silent(now)) {
            Nanos quiet = now - last_frame_;
            int n = cancel("device silent");
            sh_.silences.fetch_add(1, std::memory_order_relaxed);
            event(EventKind::silence, log::Level::warning,
                  "%s sent nothing for %lld ms with contacts down; %d contact%s cancelled. If this repeats, check the touchscreen's USB cable", cfg_.id.c_str(),
                  (long long)(quiet / kMs), n, plural(n));
            if (foreign_grab())
                set_state(DevState::grab_refused,
                          "another program took " + node_ + " exclusively, so no touches reach touch-to-osc: stop that program (fuser -v " + node_ + " names it)");
        }
        if (r == 0 && base_state_ == DevState::grab_refused && retry_grab()) {
            close_device();
            return;
        }
    }
}

}

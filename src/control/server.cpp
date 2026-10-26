#include "control/server.h"

#include "config/atomic_file.h"
#include "core/log.h"
#include "core/names.h"
#include "core/process.h"

#include "tto_version.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <set>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace tto {

namespace {

std::string count_text(uint64_t n, const char* thing) { return std::to_string(n) + " " + thing + (n == 1 ? "" : "s"); }

const std::vector<std::string> kDeviceSettings = {"label",     "enabled",   "rotation",     "mirror",     "calibration",  "aspect",   "silence_ms",
                                                  "grab",      "match_usb", "match_serial", "match_port", "usb_recovery", "usb_reset"};
const std::vector<std::string> kDeviceReported = {"connected", "state",   "reason",   "usb_watched_port", "usb_port_state", "usb_resets",   "name",
                                                  "usb",       "port",    "slots",    "size_mm",          "grabbed",        "contacts",     "oldest_s",
                                                  "last_event_s", "rate_hz", "latency_us", "counts",       "active"};
const std::vector<std::string> kCommands = {"ping",          "status/send",       "restart",          "subscribe",
                                            "unsubscribe",   "request",           "log_level",        "allowed_ips/add",
                                            "allowed_ips/remove", "saved_destinations/add", "saved_destinations/remove", "settings/forget"};
const std::vector<std::string> kReported = {"osc_api",   "version", "run",      "health",       "reasons",     "host",     "config",
                                            "settings_file", "overrides", "devices", "subscribers", "destinations", "uptime_s", "alive"};

std::string not_settable(const std::string& name) { return name + " is reported by touch-to-osc and cannot be set"; }

std::string ids_text(const Config& c) {
    std::string s;
    for (const auto& d : c.devices) s += (s.empty() ? "" : ", ") + d.id;
    return s;
}

std::string env_text(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

constexpr Nanos kTick = 100 * kMs;
constexpr Nanos kFullEvery = 10 * kSec;
constexpr Nanos kStatusReplyEvery = 200 * kMs;
constexpr Nanos kLatencyWindow = 10 * kSec;
constexpr size_t kMaxLatencySamples = 4096;
constexpr Nanos kHealthWindow = 60 * kSec;
constexpr Nanos kReportEvery = 10 * kSec;
constexpr Nanos kSaveSettle = 300 * kMs;
constexpr Nanos kSaveEvery = kSec;
constexpr Nanos kUsbStopWait = 2 * kSec;
constexpr Nanos kUsbLongWait = 30 * kSec;
constexpr Nanos kWriteStopWait = 3 * kSec;
constexpr Nanos kCheckWait = 2 * kSec;
constexpr double kEventBurst = 10;
constexpr double kEventsPerSecond = 1;
constexpr double kRefusalBurst = 20;
constexpr double kRefusalsPerSecond = 20;
constexpr size_t kMaxRefusedHosts = 1024;
constexpr Nanos kRefusedMemory = 10 * kSec;
constexpr size_t kBundleBytes = 1200;

std::optional<int64_t> integer(const osc::Arg& a) {
    if (auto i = std::get_if<int32_t>(&a)) return *i;
    if (auto h = std::get_if<int64_t>(&a)) return *h;
    if (auto f = std::get_if<float>(&a)) {
        if (std::isfinite(*f) && std::floor(*f) == *f && std::abs(*f) < 1e9f) return int64_t(*f);
    }
    if (auto d = std::get_if<double>(&a)) {
        if (std::isfinite(*d) && std::floor(*d) == *d && std::abs(*d) < 9e15) return int64_t(*d);
    }
    return std::nullopt;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t slash = s.find('/', pos);
        out.push_back(s.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return out;
}

void write_one(int fd) {
    uint64_t one = 1;
    ssize_t ignored = write(fd, &one, sizeof one);
    (void)ignored;
}

void drain(int fd) {
    uint64_t v;
    while (read(fd, &v, sizeof v) > 0) {
    }
}

bool to_sockaddr(const Peer& p, sockaddr_in& a) {
    a = sockaddr_in{};
    a.sin_family = AF_INET;
    a.sin_port = htons(p.port);
    return inet_pton(AF_INET, p.ip.c_str(), &a.sin_addr) == 1;
}

float round1(double v) { return float(std::round(v * 10.0) / 10.0); }

std::optional<double> number(const osc::Arg& a) {
    if (auto i = std::get_if<int32_t>(&a)) return double(*i);
    if (auto h = std::get_if<int64_t>(&a)) return double(*h);
    if (auto f = std::get_if<float>(&a)) {
        if (std::isfinite(*f)) return double(*f);
    }
    if (auto d = std::get_if<double>(&a)) {
        if (std::isfinite(*d)) return *d;
    }
    return std::nullopt;
}

const std::string* one_string(const std::vector<osc::Arg>& args) {
    if (args.size() != 1) return nullptr;
    return std::get_if<std::string>(&args[0]);
}

std::string num_text(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

std::string cal_text(const std::vector<double>& c) {
    if (c.empty()) return "none";
    std::string s;
    for (double v : c) s += (s.empty() ? "" : " ") + num_text(v);
    return s;
}

bool reopens(const DeviceConfig& a, const DeviceConfig& b) {
    return a.grab != b.grab || a.has_usb != b.has_usb || a.vendor != b.vendor || a.product != b.product || a.serial != b.serial || a.port != b.port;
}

bool remaps(const DeviceConfig& a, const DeviceConfig& b) {
    return a.rotation != b.rotation || a.mirror_x != b.mirror_x || a.mirror_y != b.mirror_y || a.calibration != b.calibration || a.aspect != b.aspect;
}

bool take_token(double& tokens, Nanos& last, Nanos now, double burst, double per_second) {
    if (last) tokens = std::min(burst, tokens + double(now - last) / 1e9 * per_second);
    else tokens = burst;
    last = now;
    if (tokens < 1.0) return false;
    tokens -= 1.0;
    return true;
}

int32_t next_counter(int32_t v) { return v == INT32_MAX ? 1 : v + 1; }

// receivers whose OSC library takes only i, f and s get 64-bit numbers as i
std::vector<osc::Message> plain_copy(std::vector<osc::Message> msgs) {
    for (auto& m : msgs) {
        for (auto& a : m.args) {
            if (auto h = std::get_if<int64_t>(&a)) a = int32_t(std::clamp<int64_t>(*h, INT32_MIN, INT32_MAX));
        }
    }
    return msgs;
}

}

Server::Server(Config base, SettingsLoad settings, Launch launch, SysPaths paths)
    : base_(std::move(base)),
      over_(std::move(settings.overrides)),
      over_values_(std::move(settings.config_values)),
      cfg_(apply_overrides(base_, over_)),
      settings_notes_(std::move(settings.notes)),
      settings_load_problem_(std::move(settings.problem)),
      settings_file_(settings.file_exists && !over_.empty() ? base_.paths.settings : std::string()),
      config_path_(std::move(launch.config_path)),
      config_text_(std::move(launch.config_text)),
      config_problem_(std::move(launch.config_problem)),
      exe_(std::move(launch.exe)),
      state_path_(base_.paths.state),
      paths_(std::move(paths)),
      prefix_(cfg_.osc.prefix),
      usb_root_(env_text("TTO_USB_ROOT")),
      usb_worker_(usb_root_.empty() ? UsbPaths() : usb_paths_under(usb_root_)) {
    char host[256] = {};
    if (gethostname(host, sizeof host - 1) == 0) host_ = host;
    if ((!over_.empty() && !settings.file_exists) || settings.rewrite) settings_dirty_ = true;
    usb_worker_.set_marker(usb_reset_marker(state_path_));
    keep_restart_reply();
}

void Server::keep_restart_reply() {
    std::string leases = env_text(kKeptLeases);
    unsetenv(kKeptLeases);
    for (size_t at = 0; at < leases.size() && carried_leases_.size() < kMaxLeases;) {
        size_t end = leases.find(';', at);
        std::string one = leases.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? leases.size() : end + 1;
        char lip[INET_ADDRSTRLEN] = {};
        unsigned lport = 0;
        int secs = 0;
        if (std::sscanf(one.c_str(), "%15[0-9.]:%u:%d", lip, &lport, &secs) == 3 && lport > 0 && lport < 65536 && secs >= 1 && secs <= 60)
            carried_leases_.push_back({Peer{lip, uint16_t(lport)}, secs});
    }
    std::string v = env_text(kRestartedBy);
    unsetenv(kRestartedBy);
    char ip[INET_ADDRSTRLEN] = {};
    unsigned port = 0;
    long seq = 0;
    int used = 0;
    if (v.empty() || std::sscanf(v.c_str(), "%15[0-9.]:%u:%ld:%n", ip, &port, &seq, &used) != 3 || port == 0 || port > 65535) return;
    auto unhex = [&](size_t from, size_t to) {
        std::string out;
        for (size_t i = from; i + 1 < to; i += 2) out += char(std::strtoul(v.substr(i, 2).c_str(), nullptr, 16));
        return out;
    };
    size_t colon = v.find(':', size_t(used));
    std::string command = unhex(size_t(used), colon == std::string::npos ? v.size() : colon);
    std::string note = colon == std::string::npos ? std::string() : unhex(colon + 1, v.size());
    if (!check_text(note, 320).empty()) note.clear();
    kept_.keep(Peer{ip, uint16_t(port)}, int32_t(seq), std::vector<uint8_t>(command.begin(), command.end()), true, note, monotonic_ns());
}

void Server::restore_leases() {
    Nanos now = monotonic_ns();
    for (const auto& [peer, secs] : carried_leases_) {
        bool allowed = cfg_.osc.open_to_any || std::find(cfg_.osc.allowed_ips.begin(), cfg_.osc.allowed_ips.end(), peer.ip) != cfg_.osc.allowed_ips.end();
        if (!allowed) {
            TTO_NOTICE("subscriber %s was not kept over the restart: it is no longer in osc.allowed_ips", peer.text().c_str());
            continue;
        }
        if (leases_.subscribe(peer, secs, now) == Leases::Outcome::added)
            TTO_NOTICE("subscriber %s kept over the restart (%d s of its lease left)", peer.text().c_str(), secs);
    }
    carried_leases_.clear();
    // the first tick sends them the full status and an alive with the new run at once
    if (!leases_.all().empty()) next_second_ = now;
}

Server::~Server() {
    stop_screens();
    for (auto& sh : shared_) {
        sh->stop.store(true);
        if (sh->wake) write_one(sh->wake.get());
    }
    for (auto& r : readers_) r->join();
}

void Server::stop_screens() {
    if (!screens_thread_.joinable()) return;
    write_one(screens_stop_.get());
    screens_thread_.join();
}

// the touchscreens no device reads, found off the control thread: reading the input devices reads USB attributes,
// which wait for the USB device lock; a screen counts only when it is unread in two scans half a second apart
void Server::watch_screens() {
    set_thread_name("screens");
    UniqueFd watch(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (watch) inotify_add_watch(watch.get(), paths_.dev_input.c_str(), IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO);
    auto unread = [&] {
        std::set<std::string> out;
        for (const auto& n : scan_nodes(paths_)) {
            if (touch_kind(n) == TouchKind::none || claims_.claimed(n.sysname)) continue;
            std::string port = n.usb_port.empty() ? printable(n.phys) : n.usb_port;
            out.insert((port.empty() ? std::string("-") : port) + " " + usb_text(n.vendor, n.product) + " " + printable(n.name));
        }
        return out;
    };
    int wait_ms = 1000;
    std::vector<std::string> last;
    bool published = false;
    for (;;) {
        pollfd p[2] = {{screens_stop_.get(), POLLIN, 0}, {watch ? watch.get() : -1, POLLIN, 0}};
        int r = poll(p, 2, wait_ms);
        if (r > 0 && p[0].revents) return;
        if (r > 0 && p[1].revents) drain(watch.get());
        wait_ms = 5000;
        auto first = unread();
        pollfd s{screens_stop_.get(), POLLIN, 0};
        if (poll(&s, 1, 500) > 0) return;
        auto second = unread();
        std::vector<std::string> both;
        std::set_intersection(first.begin(), first.end(), second.begin(), second.end(), std::back_inserter(both));
        if (published && both == last) continue;
        last = both;
        published = true;
        {
            std::lock_guard<std::mutex> lock(screens_m_);
            screens_found_ = both;
        }
        screens_gen_.fetch_add(1);
        write_one(notify_fd_.get());
    }
}

void Server::collect_screens() {
    uint32_t gen = screens_gen_.load();
    if (gen == screens_seen_) return;
    screens_seen_ = gen;
    std::vector<std::string> now;
    {
        std::lock_guard<std::mutex> lock(screens_m_);
        now = screens_found_;
    }
    if (!screens_first_) {
        for (const auto& s : now) {
            if (std::find(unused_screens_.begin(), unused_screens_.end(), s) != unused_screens_.end()) continue;
            emit("screens", log::Level::notice, "screens",
                 "a touchscreen that no device reads was plugged in (port, usb id, name: " + s + "): to read it, send " + addr("devices/add") +
                     " with an id, its usb id and its port, or add a [[device]] block (sudo touch-to-osc --list prints one)");
        }
        for (const auto& s : unused_screens_) {
            if (std::find(now.begin(), now.end(), s) == now.end()) emit("screens", log::Level::notice, "screens", "a touchscreen that no device read went away: " + s);
        }
    }
    screens_first_ = false;
    unused_screens_ = std::move(now);
    status_dirty_ = true;
}

bool Server::open_sockets() {
    send_fd_.reset(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    UniqueFd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd || !send_fd_) {
        TTO_CRIT("cannot create a UDP socket: %s", errno_str().c_str());
        return false;
    }
    sockaddr_in a{};
    to_sockaddr(Peer{cfg_.osc.listen.host, uint16_t(cfg_.osc.listen.port)}, a);
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        int e = errno;
        std::string why = e == EADDRINUSE ? "another program listens there, most likely touch-to-osc itself (the service: systemctl stop touch-to-osc first)"
                          : e == EADDRNOTAVAIL ? "that address is not one of this machine's"
                                               : "the port cannot be used";
        TTO_CRIT("cannot listen on UDP %s:%d (%s): %s; or change osc.listen in %s", cfg_.osc.listen.host.c_str(), cfg_.osc.listen.port, errno_str(e).c_str(),
                 why.c_str(), config_path_.c_str());
        return false;
    }
    osc_fd_ = std::move(fd);
    return true;
}

void Server::banner() {
    TTO_NOTICE("touch-to-osc %s (osc_api %d), run %d, config %s", TTO_VERSION, kOscApi, run_, config_path_.c_str());
    std::string from = cfg_.osc.open_to_any ? std::string("ANY HOST") : std::string();
    for (const auto& ip : cfg_.osc.open_to_any ? std::vector<std::string>() : cfg_.osc.allowed_ips) from += (from.empty() ? "" : ", ") + ip;
    TTO_NOTICE("OSC on UDP %s:%d, prefix %s, commands and subscriptions from %s", cfg_.osc.listen.host.c_str(), cfg_.osc.listen.port, prefix_.c_str(), from.c_str());
    for (const auto& d : cfg_.devices) {
        std::string mirror = d.mirror_text();
        TTO_NOTICE("device %s (%s): %s, rotation %d%s%s, silence %s%s", d.id.c_str(), d.label.c_str(), d.match_text().c_str(), d.rotation,
                   mirror.empty() ? "" : ", mirror ", mirror.c_str(), d.silence_ms ? (std::to_string(d.silence_ms) + " ms").c_str() : "off",
                   d.enabled ? "" : ", turned off in the config");
    }
    if (!usb_root_.empty())
        TTO_WARN("USB recovery looks for USB under %s, not the machine's own (TTO_USB_ROOT is set): that is for tests only, unset it", usb_root_.c_str());
    if (cfg_.destinations.empty()) TTO_NOTICE("no saved destinations: everything goes to subscribers");
    for (const auto& d : cfg_.destinations)
        TTO_NOTICE("destination %s:%d gets %s", d.address.host.c_str(), d.address.port,
                   d.touches && d.status ? "touches and status" : d.touches ? "touches (and the alive)" : "status (and the alive)");
    if (base_.paths.settings.empty()) {
        TTO_NOTICE("settings changed over OSC are kept until a restart (paths.settings is empty)");
    } else if (over_.empty()) {
        TTO_NOTICE("no saved settings; changes made over OSC are saved in %s", base_.paths.settings.c_str());
    } else {
        std::string names;
        for (const auto& n : override_names(over_)) names += (names.empty() ? "" : ", ") + n;
        TTO_NOTICE("saved settings from %s win over the config: %s", base_.paths.settings.c_str(), names.c_str());
    }
    for (const auto& n : settings_notes_) TTO_WARN("%s", n.c_str());
    if (!settings_load_problem_.empty()) TTO_WARN("%s", settings_load_problem_.c_str());
    if (!config_problem_.empty()) TTO_WARN("%s", config_problem_.c_str());
    for (const auto& w : overlap_warnings(cfg_)) TTO_WARN("%s", w.c_str());
}

int Server::run() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    signal_fd_.reset(signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK));
    while (run_ == 0) {
        if (getrandom(&run_, sizeof run_, 0) != sizeof run_) run_ = int32_t(uint32_t(realtime_ns()) | 1u);
    }
    started_ = monotonic_ns();
    log::Level level = log::Level::info;
    log::parse_level(cfg_.log.level.c_str(), level);
    log::set_min_level(level);
    if (!open_sockets()) return 3;
    notify_fd_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    screens_stop_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    timer_fd_.reset(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!notify_fd_ || !timer_fd_ || !signal_fd_ || !screens_stop_) {
        TTO_CRIT("cannot create the event handles: %s", errno_str().c_str());
        return 4;
    }
    itimerspec its{};
    its.it_interval.tv_nsec = long(kTick);
    its.it_value.tv_nsec = long(kTick);
    timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
    banner();
    claims_.set_devices(cfg_.devices);
    for (const auto& d : cfg_.devices) {
        auto sh = std::make_unique<DeviceShared>();
        sh->wake.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (!sh->wake) {
            TTO_CRIT("cannot create the event handles: %s", errno_str().c_str());
            return 4;
        }
        readers_.push_back(std::make_unique<DeviceReader>(d, readers_.size(), prefix_, *sh, notify_fd_.get(), claims_, paths_));
        shared_.push_back(std::move(sh));
        stats_.emplace_back();
        cfg_gen_.push_back(0);
        restart_gen_.push_back(0);
        usb_rec_.emplace_back();
        usb_last_port_.emplace_back();
        usb_state_.push_back(PortState::unknown);
        usb_watching_.push_back(false);
        usb_why_.emplace_back();
        usb_resets_.push_back(0);
        calibrate_until_.push_back(0);
    }
    std::string note;
    state_ = load_state(state_path_, &note);
    if (!note.empty()) TTO_WARN("%s", note.c_str());
    bool state_changed = false;
    for (auto it = state_.usb_ports.begin(); it != state_.usb_ports.end();) {
        auto d = std::find_if(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& c) { return c.id == it->first; });
        if (d == cfg_.devices.end() || !parse_usb_port(it->second)) {
            it = state_.usb_ports.erase(it);
            state_changed = true;
            continue;
        }
        usb_last_port_[size_t(d - cfg_.devices.begin())] = it->second;
        ++it;
    }
    repair_usb_hub();
    rx_.resize(65536);
    next_second_ = monotonic_ns() + kSec;
    restore_leases();
    publish_dests();
    for (auto& r : readers_) r->start();
    sd_notify_msg("READY=1");
    screens_thread_ = std::thread([this] { watch_screens(); });
    if (config_problem_.empty() && !config_text_.empty() && (state_.config_path != config_path_ || state_.config_text != config_text_)) {
        state_.config_path = config_path_;
        state_.config_text = config_text_;
        state_.config_saved = local_time_text();
        state_changed = true;
    }
    if (state_changed) save_state();

    for (;;) {
        pollfd p[4] = {{osc_fd_.get(), POLLIN, 0}, {signal_fd_.get(), POLLIN, 0}, {notify_fd_.get(), POLLIN, 0}, {timer_fd_.get(), POLLIN, 0}};
        if (poll(p, 4, -1) < 0) {
            if (errno == EINTR) continue;
            TTO_CRIT("waiting for events failed: %s", errno_str().c_str());
            break;
        }
        if (p[1].revents) {
            signalfd_siginfo si{};
            ssize_t ignored = read(signal_fd_.get(), &si, sizeof si);
            (void)ignored;
            TTO_NOTICE("stopping on %s", si.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM");
            break;
        }
        if (p[2].revents) {
            drain(notify_fd_.get());
            drain_reader_events();
            collect_screens();
            send_status_changes(false);
        }
        if (p[3].revents) {
            drain(timer_fd_.get());
            on_tick();
        }
        if (p[0].revents) on_osc();
        if (restart_requested_) break;
    }

    sd_notify_msg(restart_requested_ ? "RELOADING=1" : "STOPPING=1");
    stop_screens();
    for (auto& sh : shared_) {
        sh->restarting.store(restart_requested_);
        sh->stop.store(true);
        write_one(sh->wake.get());
    }
    for (auto& r : readers_) r->join();
    drain_reader_events();
    if (settings_dirty_) save_settings();
    send_bye();
    const char* doing = restart_requested_ ? "restarting" : "stopping";
    // a reset cut short between its unbind and its bind leaves every device on that hub off, so one in flight is
    // waited for; a restart also waits for a port check, since exec cannot end a thread the kernel holds
    bool long_wait = usb_worker_.resetting() || restart_requested_;
    if (!wait_usb_worker(long_wait ? kUsbLongWait : kUsbStopWait, !restart_requested_))
        TTO_WARN("a USB %s is still waiting in the kernel; %s without waiting for it", usb_worker_.resetting() ? "reset" : "port check", doing);
    collect_usb(monotonic_ns(), false);
    if (!restart_requested_ && !writer_.idle()) sd_notify_msg("EXTEND_TIMEOUT_USEC=5000000");
    if (!writer_.wait_idle(kWriteStopWait)) TTO_WARN("a file is still being written (the disk is slow or stuck); %s without waiting for it", doing);
    collect_writes(monotonic_ns());
    if (restart_requested_ && restart_asked_) {
        char head[64];
        std::snprintf(head, sizeof head, "%s:%u:%d:", restart_asked_->peer.ip.c_str(), unsigned(restart_asked_->peer.port), int(restart_asked_->seq));
        std::string v = head;
        char hex[3];
        for (uint8_t b : restart_asked_->command) {
            std::snprintf(hex, sizeof hex, "%02x", unsigned(b));
            v += hex;
        }
        v += ":";
        for (unsigned char b : restart_asked_->note) {
            std::snprintf(hex, sizeof hex, "%02x", unsigned(b));
            v += hex;
        }
        setenv(kRestartedBy, v.c_str(), 1);
    }
    if (restart_requested_) {
        std::string kept;
        Nanos now = monotonic_ns();
        for (const auto& l : leases_.all()) {
            if (l.until <= now) continue;
            long long left = std::min<long long>(60, (l.until - now + kSec - 1) / kSec);
            kept += (kept.empty() ? "" : ";") + l.peer.text() + ":" + std::to_string(left);
        }
        if (!kept.empty()) setenv(kKeptLeases, kept.c_str(), 1);
    }
    TTO_NOTICE("%s", restart_requested_ ? "restarting" : "stopped");
    return restart_requested_ ? kRestartExit : 0;
}

Status Server::restart_check() {
    const std::string keeps = "touch-to-osc keeps running: ";
    auto fresh = load_config(config_path_);
    if (!fresh) return err(keeps + "at a restart its config file would be refused (" + fresh.error() + ")");
    HostPort to = apply_overrides(*fresh, over_).osc.listen;
    if (Status st = test_bind(to); !st) return err(keeps + "at a restart it would " + st.error() + ": change osc.listen in " + config_path_);
    if (exe_.empty()) return {};
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return err(keeps + "the installed program cannot be checked (" + errno_str() + ")");
    UniqueFd rd(fds[0]), wr(fds[1]);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, wr.get(), 1);
    posix_spawn_file_actions_adddup2(&fa, wr.get(), 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    std::string arg = "--check-config=" + config_path_;
    char* argv[] = {const_cast<char*>(exe_.c_str()), const_cast<char*>(arg.c_str()), nullptr};
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe_.c_str(), &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    wr.reset();
    if (rc != 0) return err(keeps + "the program it would restart, " + exe_ + ", cannot be run (" + errno_str(rc) + ")");
    fcntl(rd.get(), F_SETFL, O_NONBLOCK);
    std::string out;
    auto take = [&] {
        char buf[512];
        ssize_t n;
        while ((n = read(rd.get(), buf, sizeof buf)) > 0) {
            if (out.size() < 4096) out.append(buf, size_t(n));
        }
    };
    Nanos until = monotonic_ns() + kCheckWait;
    int status = 0;
    for (;;) {
        take();
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        if (monotonic_ns() >= until) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return err(keeps + "the installed program did not finish checking " + config_path_ + " in time");
        }
        usleep(5000);
    }
    take();
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 0 || code == kCheckSettingsRefused) return {};
    std::string first = out.substr(0, out.find('\n'));
    for (char& c : first) {
        if (c < 0x20 || c >= 0x7f) c = '?';
    }
    return err(keeps + "the installed version (" + exe_ + ") would not start with " + config_path_ + (first.empty() ? std::string() : ": " + first));
}

Status Server::test_bind(const HostPort& to) const {
    if (to.host == cfg_.osc.listen.host && to.port == cfg_.osc.listen.port) return {};
    UniqueFd probe(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    sockaddr_in a{};
    to_sockaddr(Peer{to.host, uint16_t(to.port)}, a);
    if (!probe || bind(probe.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        return err("listen on UDP " + to.text() + ", which cannot be used (" + errno_str() + ")");
    return {};
}

Status Server::apply_by_restart(Overrides next, const Peer& peer, const std::string& what, const std::string& then) {
    if (base_.paths.settings.empty())
        return err("changes made over OSC are not saved here (paths.settings is empty), so the restart that applies this would lose it: change it in " + config_path_);
    if (usb_worker_.resetting() || usb_asked_) return err("a USB reset is running: try again in a moment");
    drop_unchanged(next, base_);
    if (settings_text(next, Overrides(), "") == settings_text(over_, Overrides(), "")) return {};
    Config eff = apply_overrides(base_, next);
    if (eff.devices.empty()) return err("that would leave no device");
    std::set<std::string> ids;
    for (const auto& d : eff.devices) {
        if (!d.has_usb && d.port.empty()) return err("device " + d.id + " would match nothing: it needs a usb id or a port");
        if (!ids.insert(d.id).second) return err("two devices would be called " + d.id);
    }
    if (Status st = test_bind(eff.osc.listen); !st) return err("touch-to-osc could not " + st.error());
    if (Status st = restart_check(); !st) return st;
    auto file = load_config(config_path_);
    note_config_values(over_, next, file ? *file : base_, over_values_);
    over_ = std::move(next);
    if (!settings_dirty_) dirty_since_ = monotonic_ns();
    settings_dirty_ = true;
    restart_requested_ = true;
    restart_asked_ = asking_;
    reply_note_ = "restarting to apply it" + then;
    emit("settings", log::Level::notice, "settings", what + ", from " + peer.ip + "; touch-to-osc restarts to apply it");
    emit("restart", log::Level::notice, "restart", "restarting to apply " + what + ", as asked by " + peer.ip + ": every contact is cancelled" + then);
    return {};
}

void Server::send_description(const Peer& to) {
    auto items = describe_items();
    std::vector<osc::Message> group;
    size_t size = 16;
    for (const auto& item : items) {
        osc::Message m{addr("describe/item"), {item}};
        size_t n = osc::encode(m).size() + 4;
        if (!group.empty() && size + n > kBundleBytes) {
            send_bytes(to, osc::encode_bundle(group));
            group.clear();
            size = 16;
        }
        group.push_back(std::move(m));
        size += n;
    }
    group.push_back({addr("describe/done"), {int32_t(items.size())}});
    send_bytes(to, osc::encode_bundle(group));
}

bool Server::wait_usb_worker(Nanos max, bool stopping) {
    Nanos until = monotonic_ns() + max;
    for (;;) {
        Nanos left = until - monotonic_ns();
        if (usb_worker_.wait(std::max<Nanos>(0, std::min<Nanos>(kSec, left)))) return true;
        if (monotonic_ns() >= until) return false;
        if (sd_watchdog_enabled()) sd_notify_msg("WATCHDOG=1");
        if (stopping) sd_notify_msg("EXTEND_TIMEOUT_USEC=5000000");
    }
}

void Server::on_tick() {
    Nanos now = monotonic_ns();
    drain_reader_events();
    drain_latency(now);
    collect_usb(now, true);
    collect_writes(now);
    expire_modes(now);
    if (settings_dirty_ && now - dirty_since_ >= kSaveSettle && now - last_save_ >= kSaveEvery) save_settings();
    if (now >= next_second_) {
        next_second_ = now - next_second_ > 2 * kSec ? now + kSec : next_second_ + kSec;
        every_second(now);
    } else if (status_dirty_) {
        send_status_changes(false);
    }
}

void Server::every_second(Nanos now) {
    expire_leases(now);
    prune(now);
    sample(now);
    watch_usb(now);
    bool full = now - full_at_ >= kFullEvery;
    if (full) full_at_ = now;
    send_status_changes(full);
    alive_ = next_counter(alive_);
    send_alive(now);
    report_destinations(now);
    report_suppressed(now);
    if (sd_watchdog_enabled()) sd_notify_msg("WATCHDOG=1");
}

void Server::expire_leases(Nanos now) {
    auto ended = leases_.expire(now);
    if (ended.empty()) return;
    publish_dests();
    status_dirty_ = true;
    for (const auto& p : ended) emit("subscriber", log::Level::notice, "subscriber", p.text() + " ended (its lease ran out)");
}

void Server::prune(Nanos now) {
    for (auto it = refused_at_.begin(); it != refused_at_.end();) {
        if (now - it->second > kRefusedMemory) it = refused_at_.erase(it);
        else ++it;
    }
    auto tg = targets();
    for (auto it = dest_health_.begin(); it != dest_health_.end();) {
        bool current = std::any_of(tg.begin(), tg.end(), [&](const Target& t) { return t.peer.text() == it->first; });
        if (current) ++it;
        else it = dest_health_.erase(it);
    }
}

void Server::emit(const std::string& key, log::Level level, const std::string& kind, const std::string& text, const std::string& device) {
    Limit& l = limits_[key];
    l.kind = kind;
    l.level = level;
    l.device = device;
    if (!take_token(l.tokens, l.last, monotonic_ns(), kEventBurst, kEventsPerSecond)) {
        ++l.suppressed;
        return;
    }
    log::write(level, "%s: %s", kind.c_str(), text.c_str());
    send_event(kind, text, level, device);
}

void Server::report_suppressed(Nanos now) {
    for (auto& [key, l] : limits_) {
        if (!l.suppressed || !take_token(l.tokens, l.last, now, kEventBurst, kEventsPerSecond)) continue;
        std::string text = count_text(l.suppressed, ("more \"" + l.kind + "\" event").c_str()) + " (" + key + ") came too fast to report one by one";
        l.suppressed = 0;
        TTO_WARN("%s", text.c_str());
        send_event(l.kind, text, l.level, l.device);
    }
    if (refusals_dropped_) {
        TTO_WARN("%llu refusals to hosts outside osc.allowed_ips were not answered (too many)", (unsigned long long)refusals_dropped_);
        refusals_dropped_ = 0;
    }
}

void Server::drain_reader_events() {
    for (size_t i = 0; i < shared_.size(); ++i) {
        ReaderEvent e;
        bool any = false;
        ModeTouch t;
        while (shared_[i]->mode_touches.pop(t)) mode_touch(i, t);
        while (shared_[i]->events.pop(e)) {
            any = true;
            emit(cfg_.devices[i].id + "/" + event_kind_name(e.kind), log::Level(e.level), event_kind_name(e.kind), e.text, cfg_.devices[i].id);
        }
        if (any) status_dirty_ = true;
        uint64_t lost = shared_[i]->lost_events.load(std::memory_order_relaxed);
        if (lost != stats_[i].lost_events_seen) {
            emit(cfg_.devices[i].id + "/lost", log::Level::warning, "device",
                 cfg_.devices[i].id + ": " + count_text(lost - stats_[i].lost_events_seen, "status event") +
                     " about it were dropped because they came too fast (touches were not affected)",
                 cfg_.devices[i].id);
            stats_[i].lost_events_seen = lost;
        }
    }
}

void Server::drain_latency(Nanos now) {
    for (size_t i = 0; i < shared_.size(); ++i) {
        uint32_t v;
        auto& lat = stats_[i].latency;
        while (shared_[i]->latency_us.pop(v)) lat.emplace_back(now, v);
        while (!lat.empty() && (now - lat.front().first > kLatencyWindow || lat.size() > kMaxLatencySamples)) lat.pop_front();
    }
}

void Server::sample(Nanos now) {
    for (size_t i = 0; i < shared_.size(); ++i) {
        DeviceShared& sh = *shared_[i];
        DevStats& st = stats_[i];
        uint64_t tf = sh.touched_frames.load(std::memory_order_relaxed);
        int64_t tn = sh.touched_ns.load(std::memory_order_relaxed);
        if (tf > st.prev_touched_frames && tn > st.prev_touched_ns)
            st.rate = round1(double(tf - st.prev_touched_frames) * 1e9 / double(tn - st.prev_touched_ns));
        st.prev_touched_frames = tf;
        st.prev_touched_ns = tn;
        st.samples.push_back(Sample{now, sh.resyncs.load(), sh.silences.load(), sh.send_drops.load(), sh.losses.load()});
        while (st.samples.size() > 2 && now - st.samples.front().at > kHealthWindow) st.samples.pop_front();
    }
}

std::vector<Server::Target> Server::targets() const {
    std::vector<Target> out;
    auto add = [&](const Peer& p, bool touches, bool status, bool plain) {
        for (auto& t : out) {
            if (t.peer == p) {
                t.touches = t.touches || touches;
                t.status = t.status || status;
                t.plain = t.plain || plain;
                return;
            }
        }
        out.push_back(Target{p, touches, status, plain});
    };
    for (const auto& d : cfg_.destinations) add(Peer{d.address.host, uint16_t(d.address.port)}, d.touches, d.status, d.plain);
    for (const auto& l : leases_.all()) add(l.peer, l.touches, l.status, l.plain);
    return out;
}

void Server::publish_dests() {
    auto tg = targets();
    for (auto& sh : shared_) {
        DestSet& d = sh->dests.back();
        d.n = 0;
        for (const auto& t : tg) {
            if (!t.touches || d.n >= kMaxSendTargets) continue;
            if (to_sockaddr(t.peer, d.addr[d.n])) d.plain[d.n++] = t.plain;
        }
        sh->dests.publish();
    }
}

bool Server::send_bytes(const Peer& to, const std::vector<uint8_t>& bytes) {
    sockaddr_in a;
    if (!to_sockaddr(to, a)) return false;
    return sendto(send_fd_.get(), bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&a), sizeof a) >= 0;
}

void Server::send_to(const Peer& to, const std::vector<osc::Message>& msgs) {
    for (const auto& m : msgs) send_bytes(to, osc::encode(m));
}

void Server::send_target(const Target& t, const std::vector<uint8_t>& bytes) {
    bool ok = send_bytes(t.peer, bytes);
    int e = errno;
    DestHealth& h = dest_health_[t.peer.text()];
    if (ok) {
        if (h.failing) {
            h.failing = false;
            TTO_NOTICE("sending to %s works again", t.peer.text().c_str());
        }
        return;
    }
    ++h.failures;
    h.error = errno_str(e);
    h.failing = true;
}

void Server::report_destinations(Nanos now) {
    for (auto& [key, h] : dest_health_) {
        if (!h.failing || h.failures == h.reported || now - h.reported_at < kReportEvery) continue;
        std::string text = key + " failing: " + h.error + " (" + std::to_string(h.failures - h.reported) + " sends)";
        h.reported = h.failures;
        h.reported_at = now;
        TTO_WARN("sending to %s", text.c_str());
        send_event("send", text, log::Level::warning, std::string());
    }
}

bool Server::health_part(const std::string& address) const {
    if (address.size() <= prefix_.size() + 1 || address.compare(0, prefix_.size() + 1, prefix_ + "/") != 0) return false;
    std::string rest = address.substr(prefix_.size() + 1);
    if (rest == "health" || rest == "reasons") return true;
    auto ends = [&](const char* tail) {
        size_t n = std::strlen(tail);
        return rest.size() > n && rest.compare(rest.size() - n, n, tail) == 0;
    };
    return rest.rfind("device/", 0) == 0 && std::count(rest.begin(), rest.end(), '/') == 2 && (ends("/health") || ends("/reasons"));
}

// every status bundle starts with its number; the health and every device's health come in one bundle
std::vector<std::vector<uint8_t>> Server::pack_status(const std::vector<osc::Message>& msgs, bool numbered) {
    std::vector<std::vector<uint8_t>> out;
    std::vector<osc::Message> group;
    const size_t head = 16 + 4 + osc::encode({addr("status_gen"), {int32_t(0)}}).size();
    size_t size = head;
    auto flush = [&] {
        if (group.empty()) return;
        if (numbered) status_gen_ = next_counter(status_gen_);
        group.insert(group.begin(), osc::Message{addr("status_gen"), {status_gen_}});
        out.push_back(osc::encode_bundle(group));
        group.clear();
        size = head;
    };
    for (size_t i = 0; i < msgs.size();) {
        size_t j = i + 1;
        if (health_part(msgs[i].address)) {
            while (j < msgs.size() && health_part(msgs[j].address)) ++j;
        }
        size_t need = 0;
        for (size_t k = i; k < j; ++k) need += osc::encode(msgs[k]).size() + 4;
        if (!group.empty() && size + need > kBundleBytes) flush();
        for (size_t k = i; k < j; ++k) group.push_back(msgs[k]);
        size += need;
        i = j;
    }
    flush();
    return out;
}

bool Server::is_plain(const Peer& p) const {
    for (const auto& t : targets()) {
        if (t.peer == p) return t.plain;
    }
    return false;
}

void Server::send_full_status(const Peer& to, bool plain, bool with_counters) {
    send_status_changes(false);
    auto msgs = status_messages();
    for (const auto& m : msgs) last_status_[m.address] = osc::encode(m);
    if (with_counters) {
        for (auto& c : counter_messages(monotonic_ns(), plain)) msgs.push_back(std::move(c));
    }
    for (const auto& b : pack_status(msgs, false)) send_bytes(to, b);
}

void Server::send_status_changes(bool full) {
    status_dirty_ = false;
    std::vector<Target> tg;
    for (const auto& t : targets()) {
        if (t.status) tg.push_back(t);
    }
    if (tg.empty()) {
        last_status_.clear();
        return;
    }
    auto all = status_messages();
    std::vector<bool> send(all.size(), full);
    for (size_t i = 0; i < all.size(); ++i) {
        auto it = last_status_.find(all[i].address);
        if (it == last_status_.end() || it->second != osc::encode(all[i])) send[i] = true;
    }
    bool health_changed = false;
    for (size_t i = 0; i < all.size(); ++i) health_changed = health_changed || (send[i] && health_part(all[i].address));
    for (size_t i = 0; i < all.size() && health_changed; ++i) {
        if (health_part(all[i].address)) send[i] = true;
    }
    std::vector<osc::Message> changed;
    for (size_t i = 0; i < all.size(); ++i) {
        if (!send[i]) continue;
        last_status_[all[i].address] = osc::encode(all[i]);
        changed.push_back(std::move(all[i]));
    }
    if (changed.empty()) return;
    auto bundles = pack_status(changed, true);
    for (const auto& t : tg) {
        for (const auto& b : bundles) send_target(t, b);
    }
}

void Server::send_alive(Nanos now) {
    auto tg = targets();
    if (tg.empty()) return;
    Health h = health(now);
    int32_t connected = 0, contacts = 0;
    std::vector<osc::Message> base;
    base.push_back({addr("alive"), {}});
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DevInfo& info = shared_[i]->info.latest();
        connected += info.connected ? 1 : 0;
        const ActiveSnap& a = shared_[i]->active.latest();
        contacts += a.n;
        osc::Message m{addr("device/" + cfg_.devices[i].id + "/active"), {a.frame}};
        for (int k = 0; k < a.n; ++k) m.args.emplace_back(a.ids[k]);
        base.push_back(std::move(m));
    }
    base[0].args = {alive_, run_, std::string(TTO_VERSION), h.level, connected, int32_t(cfg_.devices.size()), contacts, status_gen_};
    auto bare = osc::encode_bundle(base);
    std::vector<uint8_t> with_counters[2];
    for (const auto& t : tg) {
        if (t.status) {
            std::vector<uint8_t>& b = with_counters[t.plain ? 1 : 0];
            if (b.empty()) {
                auto all = base;
                for (auto& m : counter_messages(now, t.plain)) all.push_back(std::move(m));
                b = osc::encode_bundle(all);
            }
            send_target(t, b);
        } else {
            send_target(t, bare);
        }
    }
}

void Server::send_event(const std::string& kind, const std::string& text, log::Level level, const std::string& device) {
    const char* severity = level <= log::Level::err ? "error" : level == log::Level::warning ? "warning" : "notice";
    auto bytes = osc::encode({addr("event"), {kind, text, std::string(severity), device}});
    for (const auto& t : targets()) {
        if (t.status) send_target(t, bytes);
    }
}

void Server::send_bye() {
    auto bytes = osc::encode({addr("bye"), {run_}});
    for (const auto& t : targets()) send_target(t, bytes);
}

Server::Health Server::health(Nanos now) {
    // each device's faults and warnings are its own health; the top level adds what is about no device
    std::vector<std::string> faults, warnings, device_faults, device_warnings;
    Health out;
    for (size_t i = 0; i < shared_.size(); ++i) {
        std::vector<std::string> f, w;
        const DeviceConfig& d = cfg_.devices[i];
        const DevInfo& info = shared_[i]->info.latest();
        if (info.cfg_gen < restart_gen_[i] || info.state == DevState::restarting) {
            w.push_back(d.label + " is restarting after a settings change");
        } else if (!info.enabled) {
            const DeviceConfig* in_file = file_device(d.id);
            if (!in_file || in_file->enabled) w.push_back(d.label + " is turned off by a saved setting");
        } else {
            switch (info.state) {
            case DevState::ok: break;
            case DevState::starting: w.push_back(d.label + " is starting"); break;
            default:
                f.push_back(d.label + ": " + (!info.connected && usb_watching_[i] && usb_state_[i] == PortState::stuck ? usb_reason(i) : std::string(info.reason)));
                break;
            }
        }
        if (info.connected && info.matching > 1 && d.port.empty())
            w.push_back(d.label + " matches " + std::to_string(info.matching) + " touchscreens (" + d.match_text() + ") and reads the one on port " +
                        std::string(info.port) + ", which can change at a restart: set match_port to choose");
        const auto& s = stats_[i].samples;
        if (s.size() >= 2) {
            uint64_t resyncs = s.back().resyncs - s.front().resyncs;
            uint64_t drops = s.back().drops - s.front().drops;
            if (resyncs) w.push_back(d.label + ": the kernel dropped touch events " + count_text(resyncs, "time") + " in the last minute; the machine may be overloaded");
            if (drops)
                w.push_back(d.label + ": " + count_text(drops, "touch message") +
                            " could not be sent in the last minute: the network or a receiver cannot keep up, or a receiver cannot be reached (" + prefix_ +
                            "/destinations lists them)");
        }
        if (!usb_problem_.empty() && usb_problem_dev_ == i) f.push_back(usb_problem_);
        if (calibrate_until_[i] > now)
            w.push_back(d.label + " is being calibrated: its touches are paused (" + addr("device/" + d.id + "/calibrate") + " 0 ends it)");
        device_faults.insert(device_faults.end(), f.begin(), f.end());
        device_warnings.insert(device_warnings.end(), w.begin(), w.end());
        std::vector<std::string> mine = f;
        mine.insert(mine.end(), w.begin(), w.end());
        out.devices.push_back({!f.empty() ? "fault" : !w.empty() ? "warning" : "ok", std::move(mine)});
    }
    for (const auto& [key, h] : dest_health_) {
        if (h.failing) warnings.push_back("sending to " + key + " fails: " + h.error + "; check that it can be reached from this machine");
    }
    if (!usb_problem_.empty() && usb_problem_dev_ >= shared_.size()) faults.push_back(usb_problem_);
    if (identify_until_ > now) warnings.push_back("touches are paused while screens are identified (" + addr("identify") + " 0 ends it)");
    if (!config_problem_.empty()) warnings.push_back(config_problem_);
    if (!settings_load_problem_.empty()) warnings.push_back(settings_load_problem_);
    if (!settings_problem_.empty()) warnings.push_back(settings_problem_);
    if (!state_problem_.empty()) warnings.push_back(state_problem_);
    bool any_fault = !faults.empty() || !device_faults.empty();
    bool any_warning = !warnings.empty() || !device_warnings.empty();
    out.level = any_fault ? "fault" : any_warning ? "warning" : "ok";
    out.reasons = device_faults;
    out.reasons.insert(out.reasons.end(), faults.begin(), faults.end());
    out.reasons.insert(out.reasons.end(), device_warnings.begin(), device_warnings.end());
    out.reasons.insert(out.reasons.end(), warnings.begin(), warnings.end());
    return out;
}

std::optional<UsbPortRef> Server::usb_port_of(size_t index) const {
    if (!usb_last_port_[index].empty()) return parse_usb_port(usb_last_port_[index]);
    return parse_usb_port(cfg_.devices[index].port);
}

bool Server::usb_watchable(size_t index, const DevInfo& info) const {
    return cfg_.devices[index].usb_recovery && !info.connected && usb_port_of(index).has_value();
}

void Server::watch_usb(Nanos now) {
    std::vector<std::pair<size_t, UsbPortRef>> ports;
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DeviceConfig& d = cfg_.devices[i];
        const DevInfo& info = shared_[i]->info.latest();
        if (info.connected) {
            if (info.usb_port[0] && usb_last_port_[i] != info.usb_port) {
                usb_last_port_[i] = info.usb_port;
                remember_port(i);
            }
            if (usb_rec_[i].attempts() > 0)
                emit(d.id + "/usb", log::Level::notice, "usb", d.id + " is back after " + count_text(unsigned(usb_rec_[i].attempts()), "USB reset"), d.id);
            usb_rec_[i].connected();
        }
        std::optional<UsbPortRef> ref;
        if (usb_watchable(i, info)) ref = usb_port_of(i);
        if (!ref) {
            if (usb_watching_[i]) status_dirty_ = true;
            usb_watching_[i] = false;
            continue;
        }
        ports.emplace_back(i, *ref);
    }
    if (usb_worker_.busy()) {
        if (!usb_worker_.resetting() && now - usb_worker_.busy_since() >= kSlowPortCheck)
            for (const auto& [i, ref] : ports)
                if (std::find(usb_checking_.begin(), usb_checking_.end(), i) != usb_checking_.end()) note_port(i, ref.path, PortState::setting_up, std::string(), now);
        return;
    }
    if (ports.empty()) return;
    usb_checking_.clear();
    for (const auto& p : ports) usb_checking_.push_back(p.first);
    usb_worker_.check(std::move(ports), now);
}

void Server::note_port(size_t index, const std::string& port, PortState state, const std::string& why, Nanos now) {
    auto ref = usb_port_of(index);
    if (!usb_watchable(index, shared_[index]->info.latest()) || !ref || ref->path != port) return;
    const DeviceConfig& d = cfg_.devices[index];
    if (!usb_watching_[index] || state != usb_state_[index]) status_dirty_ = true;
    if (state == PortState::unknown && why != usb_why_[index])
        emit(d.id + "/usb", log::Level::warning, "usb", d.id + ": cannot tell whether anything is plugged into USB port " + port + " (" + why + ")", d.id);
    usb_why_[index] = state == PortState::unknown ? why : std::string();
    usb_state_[index] = state;
    usb_watching_[index] = true;
    if (usb_rec_[index].observe(now, state) && !usb_due_) usb_due_ = index;
}

void Server::collect_usb(Nanos now, bool start_next) {
    UsbWorker::Done done;
    if (usb_worker_.finished(done)) {
        if (done.reset) {
            finish_usb_reset(now, done);
        } else {
            if (!done.result) TTO_ERR("checking USB ports failed: %s", done.result.error().c_str());
            for (const UsbReading& r : done.readings) note_port(r.device, r.port, r.state, r.why, now);
        }
    }
    std::optional<size_t> due = usb_due_;
    usb_due_.reset();
    if (!start_next || usb_worker_.busy()) return;
    if (usb_asked_) {
        UsbAsk ask = std::move(*usb_asked_);
        usb_asked_.reset();
        start_usb_reset(ask.device, ask.ref, ask.by);
    } else if (due) {
        auto ref = usb_port_of(*due);
        if (ref && usb_watchable(*due, shared_[*due]->info.latest())) start_usb_reset(*due, *ref, std::string());
    }
}

void Server::start_usb_reset(size_t index, const UsbPortRef& ref, const std::string& asked_by) {
    const std::string& id = cfg_.devices[index].id;
    usb_worker_dev_ = index;
    usb_worker_manual_ = !asked_by.empty();
    usb_worker_target_ = usb_target_text(ref);
    usb_worker_hub_ = ref.hub_interface;
    if (usb_worker_manual_)
        emit(id + "/usb", log::Level::warning, "usb",
             id + ": resetting " + usb_worker_target_ + " as asked by " + asked_by + "; every device on it drops for a moment", id);
    else
        emit(id + "/usb", log::Level::warning, "usb",
             id + ": something is plugged into USB port " + ref.path + " but the kernel has not set it up; resetting " + usb_worker_target_ + " (attempt " +
                 std::to_string(usb_rec_[index].attempts() + 1) + "); every device on it drops for a moment",
             id);
    usb_worker_.reset(ref, monotonic_ns());
    status_dirty_ = true;
}

void Server::finish_usb_reset(Nanos now, const UsbWorker::Done& done) {
    size_t i = usb_worker_dev_;
    const std::string& id = cfg_.devices[i].id;
    ++usb_resets_[i];
    if (!usb_worker_manual_) {
        std::vector<std::string> watched(usb_rec_.size());
        for (size_t j = 0; j < watched.size(); ++j) {
            auto ref = usb_port_of(j);
            if (usb_watching_[j] && ref) watched[j] = ref->hub_interface;
        }
        reset_done_on_hub(usb_rec_, watched, i, usb_worker_hub_, now);
    }
    std::string also = done.dropped.empty() ? "nothing else was on it" : "it also dropped ";
    for (size_t k = 0; k < done.dropped.size(); ++k) also += (k ? ", " : "") + done.dropped[k];
    if (done.result) {
        usb_problem_.clear();
        usb_problem_dev_ = SIZE_MAX;
        emit(id + "/usb", log::Level::notice, "usb",
             id + ": " + usb_worker_target_ + " reset in " + std::to_string(done.took / kMs) + " ms (" + also + "); waiting for the touchscreen to come back", id);
    } else {
        usb_problem_ = done.result.error();
        usb_problem_dev_ = i;
        emit(id + "/usb", log::Level::err, "usb", id + ": " + done.result.error(), id);
    }
    status_dirty_ = true;
}

std::string Server::usb_reason(size_t index) const {
    auto ref = usb_port_of(index);
    std::string port = ref ? ref->path : std::string("?");
    std::string tries = count_text(usb_resets_[index], "reset") + " so far";
    return "plugged into USB port " + port + " but the kernel has not set it up; touch-to-osc resets the USB connection by itself (" + tries +
           "). If this keeps happening, check the cable and its joints";
}

std::string Server::disabled_reason(size_t index) const {
    std::string on = "send " + prefix_ + "/device/" + cfg_.devices[index].id + "/enabled 1 to turn it back on";
    const DeviceConfig* f = file_device(cfg_.devices[index].id);
    if (f && !f->enabled) return "turned off in the config file (enabled = false): set it to true there, or " + on;
    return "turned off by a saved setting (changed over OSC, for example from an app): turn it on there, or " + on;
}

std::vector<osc::Message> Server::status_messages() {
    std::vector<osc::Message> out;
    auto add = [&](const std::string& rest, std::vector<osc::Arg> args) { out.push_back({addr(rest), std::move(args)}); };
    Health h = health(monotonic_ns());
    add("osc_api", {kOscApi});
    add("version", {std::string(TTO_VERSION)});
    add("run", {run_});
    add("health", {h.level});
    std::vector<osc::Arg> reasons(h.reasons.begin(), h.reasons.end());
    add("reasons", reasons);
    for (size_t i = 0; i < h.devices.size() && i < cfg_.devices.size(); ++i) {
        add("device/" + cfg_.devices[i].id + "/health", {h.devices[i].first});
        add("device/" + cfg_.devices[i].id + "/reasons", std::vector<osc::Arg>(h.devices[i].second.begin(), h.devices[i].second.end()));
    }
    add("host", {host_});
    add("config", {config_path_});
    add("settings_file", {settings_file_});
    add("listen", {cfg_.osc.listen.text()});
    add("prefix", {prefix_});
    add("log_level", {cfg_.log.level});
    std::vector<osc::Arg> allowed;
    if (cfg_.osc.open_to_any) allowed.emplace_back(std::string("any"));
    for (const auto& ip : cfg_.osc.allowed_ips) allowed.emplace_back(ip);
    add("allowed_ips", allowed);
    std::vector<osc::Arg> saved;
    for (const auto& d : cfg_.destinations) saved.emplace_back(destination_text(d));
    add("saved_destinations", saved);
    std::vector<osc::Arg> overrides;
    for (const auto& n : override_names(over_)) overrides.emplace_back(n);
    add("overrides", overrides);
    std::vector<osc::Arg> ids;
    for (const auto& d : cfg_.devices) ids.emplace_back(d.id);
    add("devices", ids);
    add("unused_screens", std::vector<osc::Arg>(unused_screens_.begin(), unused_screens_.end()));
    Nanos now_s = monotonic_ns();
    add("identify_s", {int32_t(identify_until_ > now_s ? (identify_until_ - now_s + kSec - 1) / kSec : 0)});
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DeviceConfig& d = cfg_.devices[i];
        const DevInfo& info = shared_[i]->info.latest();
        bool restarting = info.cfg_gen < restart_gen_[i];
        std::string b = "device/" + d.id + "/";
        add(b + "label", {d.label});
        add(b + "enabled", {int32_t(info.enabled ? 1 : 0)});
        add(b + "connected", {int32_t(info.connected ? 1 : 0)});
        bool stuck = !info.connected && usb_watching_[i] && usb_state_[i] == PortState::stuck;
        ReadMode mode = ReadMode(shared_[i]->mode.load());
        bool paused = info.state == DevState::ok && info.enabled && mode != ReadMode::touches;
        add(b + "state", {std::string(restarting ? "restarting" : paused ? (mode == ReadMode::identify ? "identifying" : "calibrating") : state_name(info.state))});
        add(b + "calibrate_s", {int32_t(calibrate_until_[i] > now_s ? (calibrate_until_[i] - now_s + kSec - 1) / kSec : 0)});
        add(b + "reason", {restarting ? std::string("restarting after a settings change")
                           : !info.enabled ? disabled_reason(i)
                           : stuck         ? usb_reason(i)
                                           : std::string(info.reason)});
        auto usb_ref = usb_port_of(i);
        add(b + "usb_recovery", {int32_t(d.usb_recovery ? 1 : 0)});
        add(b + "usb_watched_port", {usb_ref ? usb_ref->path : std::string()});
        add(b + "usb_port_state", {std::string(usb_watching_[i] ? port_state_text(usb_state_[i]) : "")});
        add(b + "usb_resets", {int32_t(std::min<uint64_t>(usb_resets_[i], INT32_MAX))});
        add(b + "match_usb", {d.usb_setting()});
        add(b + "match_serial", {d.serial});
        add(b + "match_port", {d.port});
        std::vector<osc::Arg> cal;
        for (double v : d.calibration) cal.emplace_back(float(v));
        add(b + "calibration", cal);
        add(b + "grab", {int32_t(d.grab ? 1 : 0)});
        add(b + "name", {std::string(info.connected ? info.name : "")});
        add(b + "usb", {std::string(info.connected ? info.usb : "")});
        add(b + "port", {std::string(info.connected ? info.port : "")});
        add(b + "slots", {int32_t(info.connected ? info.slots : 0)});
        add(b + "size_mm", {info.connected ? info.w_mm : 0.0f, info.connected ? info.h_mm : 0.0f});
        add(b + "rotation", {int32_t(d.rotation)});
        add(b + "mirror", {d.mirror_text()});
        add(b + "aspect", {d.aspect > 0 ? float(d.aspect) : info.aspect});
        add(b + "silence_ms", {int32_t(d.silence_ms)});
        add(b + "grabbed", {int32_t(info.connected && info.grabbed ? 1 : 0)});
    }
    return out;
}

std::vector<osc::Message> Server::counter_messages(Nanos now, bool plain) {
    std::vector<osc::Message> out;
    auto add = [&](const std::string& rest, std::vector<osc::Arg> args) { out.push_back({addr(rest), std::move(args)}); };
    add("uptime_s", {int32_t(std::min<Nanos>((now - started_) / kSec, INT32_MAX))});
    for (size_t i = 0; i < shared_.size(); ++i) {
        DeviceShared& sh = *shared_[i];
        std::string b = "device/" + cfg_.devices[i].id + "/";
        add(b + "contacts", {int32_t(sh.contacts.load())});
        Nanos oldest = sh.oldest_down_ns.load();
        add(b + "oldest_s", {oldest ? round1(double(now - oldest) / 1e9) : -1.0f});
        Nanos last = sh.last_frame_ns.load();
        add(b + "last_event_s", {last ? round1(double(now - last) / 1e9) : -1.0f});
        Nanos loss = sh.last_loss_ns.load(), back = sh.last_back_ns.load();
        add(b + "last_loss_s", {loss ? round1(double(now - loss) / 1e9) : -1.0f});
        add(b + "last_gone_s", {loss && back > loss ? round1(double(back - loss) / 1e9) : -1.0f});
        add(b + "rate_hz", {stats_[i].rate});
        std::vector<uint32_t> lat;
        for (const auto& [at, v] : stats_[i].latency) lat.push_back(v);
        int32_t p50 = 0, p99 = 0;
        if (!lat.empty()) {
            std::sort(lat.begin(), lat.end());
            p50 = int32_t(lat[(lat.size() - 1) / 2]);
            p99 = int32_t(lat[std::min(lat.size() - 1, size_t(std::ceil(0.99 * double(lat.size()))) - 1)]);
        }
        add(b + "latency_us", {p50, p99});
        add(b + "counts", {int64_t(sh.frames.load()), int64_t(sh.touches.load()), int64_t(sh.resyncs.load()), int64_t(sh.send_drops.load()),
                           int64_t(sh.cancels.load()), int64_t(sh.losses.load())});
    }
    std::vector<osc::Arg> subs;
    for (const auto& l : leases_.all()) subs.emplace_back(l.peer.text() + " " + std::to_string(std::max<Nanos>(0, (l.until - now + kSec - 1) / kSec)));
    add("subscribers", subs);
    std::vector<osc::Arg> dests;
    for (const auto& t : targets()) {
        auto it = dest_health_.find(t.peer.text());
        dests.emplace_back(t.peer.text() + (it != dest_health_.end() && it->second.failing ? " failing" : " ok"));
    }
    add("destinations", dests);
    return plain ? plain_copy(std::move(out)) : out;
}

void Server::on_osc() {
    for (int n = 0; n < 64; ++n) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        ssize_t got = recvfrom(osc_fd_.get(), rx_.data(), rx_.size(), MSG_TRUNC, reinterpret_cast<sockaddr*>(&from), &len);
        if (got < 0) break;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        Peer peer{ip, ntohs(from.sin_port)};
        if (size_t(got) > rx_.size()) {
            TTO_LOG_EVERY(30 * kSec, log::Level::warning, "an OSC packet of %zd bytes from %s was dropped: larger than %zu", got, ip, rx_.size());
            continue;
        }
        bool allowed = cfg_.osc.open_to_any || std::find(cfg_.osc.allowed_ips.begin(), cfg_.osc.allowed_ips.end(), peer.ip) != cfg_.osc.allowed_ips.end();
        osc::DecodeIssue issue;
        auto msgs = osc::decode_packet(rx_.data(), size_t(got), &issue);
        if (!msgs) {
            if (issue.address.empty()) TTO_LOG_EVERY(30 * kSec, log::Level::warning, "a malformed OSC packet from %s was dropped", ip);
            else refuse(peer, issue.address, allowed ? issue.what : outsider_reason(peer), !allowed);
            continue;
        }
        if (!allowed) {
            const osc::Message* first = nullptr;
            for (const auto& m : *msgs) {
                if (m.address == addr("hello")) hello(m, peer, false);
                else if (!first) first = &m;
            }
            if (first) {
                uint16_t named = 0;
                if (first->address == addr("subscribe") && !first->args.empty()) {
                    int64_t port = integer(first->args[0]).value_or(0);
                    if (port > 0 && port < 65536) named = uint16_t(port);
                }
                refuse(peer, first->address, outsider_reason(peer), true, named);
            }
            continue;
        }
        for (const auto& m : *msgs) command(m, peer);
    }
    if (status_dirty_) send_status_changes(false);
}

std::string Server::outsider_reason(const Peer& peer) const {
    bool removed = std::find(over_.allowed_removed.begin(), over_.allowed_removed.end(), peer.ip) != over_.allowed_removed.end();
    if (removed)
        return peer.ip + " may not send commands here: it was taken off the allowed hosts over OSC (a saved setting); send " + prefix_ + "/allowed_ips/add " +
               peer.ip + " from an allowed host, or " + prefix_ + "/settings/forget allowed_ips";
    return peer.ip + " may not send commands here: add it to osc.allowed_ips in " + config_path_ + " and restart touch-to-osc, or send " + prefix_ +
           "/allowed_ips/add " + peer.ip + " from an allowed host";
}

void Server::hello(const osc::Message& m, const Peer& peer, bool allowed) {
    Peer to = peer;
    if (m.args.size() == 1) {
        auto port = integer(m.args[0]);
        if (port && *port > 0 && *port < 65536) to.port = uint16_t(*port);
    }
    std::vector<osc::Message> here{{addr("here"), {host_, std::string(TTO_VERSION), kOscApi, int32_t(cfg_.osc.listen.port), int32_t(cfg_.devices.size()),
                                                   int32_t(allowed ? 1 : 0)}}};
    if (allowed) {
        reply_to(to, here);
        return;
    }
    if (!take_token(refusal_tokens_, refusal_last_, monotonic_ns(), kRefusalBurst, kRefusalsPerSecond)) {
        ++refusals_dropped_;
        return;
    }
    send_to(to, here);
}

// a device as the config file has it, or as it was added over OSC; the running list may differ from both
const DeviceConfig* Server::file_device(const std::string& id) const {
    for (const auto& d : base_.devices) {
        if (d.id == id) return &d;
    }
    for (const auto& d : over_.devices_added) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

std::string Server::calibrating_id() const {
    Nanos now = monotonic_ns();
    for (size_t i = 0; i < calibrate_until_.size(); ++i) {
        if (calibrate_until_[i] > now) return cfg_.devices[i].id;
    }
    return {};
}

void Server::update_modes() {
    Nanos now = monotonic_ns();
    for (size_t i = 0; i < shared_.size(); ++i) {
        ReadMode want = calibrate_until_[i] > now ? ReadMode::calibrate : identify_until_ > now ? ReadMode::identify : ReadMode::touches;
        if (shared_[i]->mode.load() == uint8_t(want)) continue;
        shared_[i]->mode.store(uint8_t(want));
        write_one(shared_[i]->wake.get());
        status_dirty_ = true;
    }
}

void Server::expire_modes(Nanos now) {
    bool changed = false;
    if (identify_until_ && now >= identify_until_) {
        identify_until_ = 0;
        changed = true;
        emit("identify", log::Level::notice, "identify", "identifying screens ended: its time ran out; touches go out again");
    }
    for (size_t i = 0; i < calibrate_until_.size(); ++i) {
        if (!calibrate_until_[i] || now < calibrate_until_[i]) continue;
        calibrate_until_[i] = 0;
        changed = true;
        const DeviceConfig& d = cfg_.devices[i];
        emit(d.id + "/calibrate", log::Level::notice, "calibrate", d.label + ": guided calibration ended: its time ran out; touches go out again", d.id);
    }
    if (changed) update_modes();
}

void Server::mode_touch(size_t index, const ModeTouch& t) {
    const DeviceConfig& d = cfg_.devices[index];
    Nanos now = monotonic_ns();
    osc::Message msg;
    if (calibrate_until_[index] > now) {
        msg = {addr("device/" + d.id + "/calibration/touch"), {t.contact, std::string(t.down ? "down" : "up"), t.u, t.v}};
    } else if (identify_until_ > now && t.down) {
        const DevInfo& info = shared_[index]->info.latest();
        msg = {addr("identified"), {d.id, d.label, std::string(info.port), t.x, t.y}};
        char at[48];
        std::snprintf(at, sizeof at, "%.2f, %.2f", double(t.x), double(t.y));
        emit(d.id + "/identify", log::Level::notice, "identify",
             d.label + (info.port[0] ? " (port " + std::string(info.port) + ")" : std::string()) + " was touched at " + at, d.id);
    } else {
        return;
    }
    auto bytes = osc::encode(msg);
    for (const auto& tg : targets()) {
        if (tg.status) send_target(tg, bytes);
    }
}

Status Server::calibration_fit(size_t index, const std::vector<osc::Arg>& args, const Peer& peer) {
    // copied: applying the fit replaces cfg_
    const DeviceConfig d = cfg_.devices[index];
    if (args.empty() || args.size() % 4 != 0)
        return err("calibration/fit takes 3 to 16 points, four numbers each: x y where the target was shown, u v where it was touched");
    std::vector<CalPoint> pts;
    for (size_t k = 0; k + 3 < args.size(); k += 4) {
        auto x = number(args[k]), y = number(args[k + 1]), u = number(args[k + 2]), v = number(args[k + 3]);
        if (!x || !y || !u || !v) return err("calibration/fit takes numbers only");
        if (*x < 0 || *x > 1 || *y < 0 || *y > 1) return err("calibration/fit: x and y are where a target was shown, 0..1 of the display");
        if (*u < -1 || *u > 2 || *v < -1 || *v > 2) return err("calibration/fit: u and v are where a target was touched, as calibration/touch reports it");
        pts.push_back({*x, *y, *u, *v});
    }
    auto fit = fit_calibration(pts);
    if (!fit) return err(fit.error());
    char pct[16];
    std::snprintf(pct, sizeof pct, "%.1f%%", fit->worst * 100);
    if (fit->worst > 0.05)
        return err(std::string("the touches do not line up with the targets: worst miss ") + pct + " of the screen (point " + std::to_string(fit->worst_at + 1) +
                   "); touch each target as it is shown and fit again");
    if (std::string p = check_calibration(fit->c); !p.empty()) return err("the fit gives a calibration that " + p + "; check the points");
    const DevInfo& info = shared_[index]->info.latest();
    bool turned = d.rotation == 90 || d.rotation == 270;
    double w = turned ? info.h_mm : info.w_mm, h = turned ? info.w_mm : info.h_mm;
    double worst_mm = -1;
    if (w > 0 && h > 0) {
        worst_mm = 0;
        for (const auto& q : pts) {
            double fx = fit->c[0] * q.u + fit->c[1] * q.v + fit->c[2], fy = fit->c[3] * q.u + fit->c[4] * q.v + fit->c[5];
            worst_mm = std::max(worst_mm, std::hypot((fx - q.x) * w, (fy - q.y) * h));
        }
    }
    std::vector<osc::Arg> c(fit->c.begin(), fit->c.end());
    if (Status st = set_device(index, "calibration", c, peer); !st) return st;
    char note[96];
    if (worst_mm >= 0) std::snprintf(note, sizeof note, "fitted from %zu points: worst miss %s of the screen (%.1f mm)", pts.size(), pct, worst_mm);
    else std::snprintf(note, sizeof note, "fitted from %zu points: worst miss %s of the screen", pts.size(), pct);
    reply_note_ = note;
    reply_to(peer, {{addr("device/" + d.id + "/calibration/fitted"), {float(fit->worst), float(worst_mm)}}});
    emit(d.id + "/calibrate", log::Level::notice, "calibrate", d.label + ": calibration " + note + ", as asked by " + peer.ip, d.id);
    return {};
}

void Server::reply_to(const Peer& to, const std::vector<osc::Message>& msgs) {
    send_to(to, msgs);
    std::vector<uint16_t> done{to.port};
    for (const auto& l : leases_.all()) {
        if (l.peer.ip != to.ip || std::find(done.begin(), done.end(), l.peer.port) != done.end()) continue;
        done.push_back(l.peer.port);
        send_to(l.peer, msgs);
    }
}

void Server::refuse(const Peer& peer, const std::string& address, const std::string& why, bool limited, uint16_t also_port) {
    if (limited) {
        Nanos now = monotonic_ns();
        auto it = refused_at_.find(peer.ip);
        if (it != refused_at_.end() && now - it->second < 5 * kSec) return;
        if (refused_at_.size() >= kMaxRefusedHosts || !take_token(refusal_tokens_, refusal_last_, now, kRefusalBurst, kRefusalsPerSecond)) {
            ++refusals_dropped_;
            return;
        }
        refused_at_[peer.ip] = now;
    }
    TTO_LOG_EVERY(5 * kSec, log::Level::warning, "OSC %s from %s refused: %s", address.c_str(), peer.ip.c_str(), why.c_str());
    std::vector<osc::Message> e{{addr("error"), {address, why}}};
    if (limited) send_to(peer, e);
    else reply_to(peer, e);
    if (also_port && also_port != peer.port) send_to(Peer{peer.ip, also_port}, e);
}

bool Server::command(const osc::Message& m, const Peer& peer) {
    TTO_DEBUG("OSC %s from %s:%u", m.address.c_str(), peer.ip.c_str(), unsigned(peer.port));
    if (m.address == addr("request")) return request(m, peer);
    Status st = apply(m, peer);
    if (!st) {
        refuse(peer, m.address, st.error(), false);
        return false;
    }
    return true;
}

bool Server::request(const osc::Message& m, const Peer& peer) {
    auto seq = m.args.empty() ? std::nullopt : integer(m.args[0]);
    if (!seq || *seq < INT32_MIN || *seq > INT32_MAX || m.args.size() < 2 || !std::holds_alternative<std::string>(m.args[1])) {
        refuse(peer, m.address, "a request needs a seq number, an address, then the command's arguments", false);
        return false;
    }
    int32_t s = int32_t(*seq);
    osc::Message inner{std::get<std::string>(m.args[1]), std::vector<osc::Arg>(m.args.begin() + 2, m.args.end())};
    auto answer = [&](bool ok, const std::string& note) {
        reply_to(peer, {{addr("reply"), {s, std::string(ok ? "ok" : "refused"), note}}});
    };
    Nanos now = monotonic_ns();
    auto bytes = osc::encode(inner);
    const KeptReplies::Kept* k = nullptr;
    switch (kept_.find(peer, s, bytes, now, &k)) {
    case KeptReplies::Lookup::repeat: answer(k->ok, k->note); return k->ok;
    case KeptReplies::Lookup::conflict:
        answer(false, "seq " + std::to_string(s) + " was used for another command in the last " + std::to_string(kKeepReplies / kSec) + " s: use a new seq for each command");
        return false;
    case KeptReplies::Lookup::none: break;
    }
    Status st;
    if (!osc::valid_address(inner.address)) st = err("a request's address must be an OSC address: a slash, then printable characters");
    else if (inner.address == addr("request")) st = err("a request cannot carry another request");
    else {
        asking_ = Asked{peer, s, bytes, std::string()};
        reply_note_.clear();
        st = apply(inner, peer);
        asking_.reset();
    }
    if (!st) TTO_LOG_EVERY(5 * kSec, log::Level::warning, "OSC %s from %s refused: %s", osc::valid_address(inner.address) ? inner.address.c_str() : "(not an address)",
                           peer.ip.c_str(), st.error().c_str());
    std::string note = st.ok() ? reply_note_ : st.error();
    reply_note_.clear();
    if (restart_asked_ && restart_asked_->seq == s && restart_asked_->peer == peer) restart_asked_->note = note;
    kept_.keep(peer, s, bytes, st.ok(), note, now);
    answer(st.ok(), note);
    return st.ok();
}

Status Server::apply(const osc::Message& m, const Peer& peer) {
    if (restart_requested_) return err("touch-to-osc is restarting: send it again in a moment");
    if (m.address.size() <= prefix_.size() + 1 || m.address.compare(0, prefix_.size() + 1, prefix_ + "/") != 0)
        return err("unknown address: every address here starts with " + prefix_ + "/");
    auto parts = split(m.address.substr(prefix_.size() + 1));
    auto reply_peer = [&]() {
        Peer to = peer;
        if (m.args.size() == 1) {
            auto port = integer(m.args[0]);
            if (port && *port > 0 && *port < 65536) to.port = uint16_t(*port);
        }
        return to;
    };
    if (parts.size() == 1 && parts[0] == "describe") {
        if (m.args.size() > 1) return err("describe takes no arguments, or the port to answer to");
        Nanos now = monotonic_ns();
        if (describe_at_ && now - describe_at_ < 500 * kMs) return err("at most 2 descriptions a second");
        describe_at_ = now;
        send_description(reply_peer());
        return {};
    }
    if (parts.size() == 2 && parts[0] == "devices" && parts[1] == "add") {
        const char* usage = "devices/add needs an id, a usb id (vvvv:pppp) and a USB port: \"screen2\" \"0712:0009\" \"3-4\" (one of the two may be \"\")";
        if (m.args.size() != 3) return err(usage);
        const std::string* id = std::get_if<std::string>(&m.args[0]);
        const std::string* usb = std::get_if<std::string>(&m.args[1]);
        const std::string* port = std::get_if<std::string>(&m.args[2]);
        if (!id || !usb || !port) return err(usage);
        if (std::string p = check_device_id(*id); !p.empty()) return err("the id " + p);
        if (std::any_of(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& d) { return d.id == *id; })) return err("there is a device " + *id + " already");
        if (cfg_.devices.size() >= kMaxDevices) return err("at most " + std::to_string(kMaxDevices) + " devices");
        DeviceConfig d;
        d.id = *id;
        d.label = *id;
        if (!usb->empty()) {
            if (std::string p = check_usb(*usb, &d.vendor, &d.product); !p.empty()) return err("match_usb " + p);
            d.has_usb = true;
        }
        if (std::string p = check_text(*port, 64); !p.empty()) return err("match_port " + p);
        d.port = *port;
        if (!d.has_usb && d.port.empty()) return err("device " + d.id + " would match nothing: give a usb id or a port");
        Overrides next = over_;
        auto gone = std::find(next.devices_removed.begin(), next.devices_removed.end(), d.id);
        if (gone != next.devices_removed.end()) {
            next.devices_removed.erase(gone);
            DeviceOverrides& x = next.devices[d.id];
            x.match_usb = d.usb_setting();
            x.match_port = d.port;
        } else {
            next.devices_added.push_back(d);
        }
        return apply_by_restart(std::move(next), peer, "device " + d.id + " added (" + d.match_text() + ")", "; then touch-to-osc reads " + d.id + " too");
    }
    if (parts.size() == 2 && parts[0] == "devices" && parts[1] == "remove") {
        const std::string* id = m.args.size() == 1 ? std::get_if<std::string>(&m.args[0]) : nullptr;
        if (!id) return err("devices/remove needs the id of a device");
        if (std::none_of(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& d) { return d.id == *id; }))
            return err(check_text(*id, 32).empty() ? "no device " + *id + " (devices: " + ids_text(cfg_) + ")" : std::string("no device by that name"));
        if (cfg_.devices.size() == 1) return err(*id + " is the only device: touch-to-osc needs at least one");
        Overrides next = over_;
        auto added = std::find_if(next.devices_added.begin(), next.devices_added.end(), [&](const DeviceConfig& d) { return d.id == *id; });
        if (added != next.devices_added.end()) next.devices_added.erase(added);
        else next.devices_removed.push_back(*id);
        return apply_by_restart(std::move(next), peer, "device " + *id + " removed", "; then touch-to-osc no longer reads " + *id);
    }
    if (parts.size() == 1 && (parts[0] == "listen" || parts[0] == "prefix")) {
        const std::string* v = m.args.size() == 1 ? std::get_if<std::string>(&m.args[0]) : nullptr;
        Overrides next = over_;
        if (parts[0] == "listen") {
            HostPort hp;
            if (!v) return err("listen needs \"ip:port\", like \"0.0.0.0:9480\"");
            if (std::string p = check_listen(*v, hp); !p.empty()) return err("listen " + p);
            next.listen = hp;
            return apply_by_restart(std::move(next), peer, "listen " + cfg_.osc.listen.text() + " -> " + hp.text(),
                                    "; then touch-to-osc listens on " + hp.text() + ": send commands there");
        }
        if (!v) return err("prefix needs one string, like \"/touch\"");
        if (std::string p = check_prefix(*v); !p.empty()) return err("prefix " + p);
        next.prefix = *v;
        return apply_by_restart(std::move(next), peer, "prefix " + prefix_ + " -> " + *v, "; then every address starts with " + *v);
    }
    if (parts.size() == 1 && parts[0] == "identify") {
        auto s = m.args.size() == 1 ? integer(m.args[0]) : std::nullopt;
        if (!s || *s < 0 || *s > 60) return err("identify needs a number of seconds: 1-60 to identify the screens (touches pause meanwhile), 0 to end it");
        std::string busy = calibrating_id();
        if (*s > 0 && !busy.empty()) return err(busy + " is being calibrated: end that first (" + addr("device/" + busy + "/calibrate") + " 0)");
        bool was = identify_until_ != 0;
        identify_until_ = *s ? monotonic_ns() + *s * kSec : 0;
        update_modes();
        status_dirty_ = true;
        if (*s && !was)
            emit("identify", log::Level::notice, "identify",
                 "identifying screens for " + std::to_string(*s) + " s, as asked by " + peer.ip + ": touches are paused; each screen touched is reported");
        else if (!*s && was)
            emit("identify", log::Level::notice, "identify", "identifying screens ended, as asked by " + peer.ip + "; touches go out again");
        return {};
    }
    if (parts.size() == 1 && parts[0] == "hello") {
        if (m.args.size() > 1) return err("hello takes no arguments, or the port to answer to");
        hello(m, peer, true);
        return {};
    }
    if (parts.size() == 1 && parts[0] == "ping") {
        if (m.args.size() > 1) return err("ping takes no arguments, or the port to answer to");
        reply_to(reply_peer(), {{addr("pong"), {run_}}});
        return {};
    }
    if (parts.size() == 2 && parts[0] == "status" && parts[1] == "send") {
        Nanos now = monotonic_ns();
        if (status_reply_at_ && now - status_reply_at_ < kStatusReplyEvery) return err("at most 5 status requests a second");
        status_reply_at_ = now;
        Peer to = reply_peer();
        send_full_status(to, is_plain(to), true);
        return {};
    }
    if (parts.size() == 1 && parts[0] == "restart") {
        if (!m.args.empty()) return err("restart takes no arguments");
        if (usb_worker_.resetting() || usb_asked_) return err("a USB reset is running; touch-to-osc keeps running: try the restart again in a moment");
        if (Status st = restart_check(); !st) return st;
        restart_requested_ = true;
        restart_asked_ = asking_;
        emit("restart", log::Level::notice, "restart",
             "restarting, as asked by " + peer.ip + ": every contact is cancelled, then touch-to-osc starts again from " + config_path_ + " and its saved settings");
        return {};
    }
    if (parts.size() == 1 && parts[0] == "subscribe") return subscribe(m, peer);
    if (parts.size() == 1 && parts[0] == "unsubscribe") return unsubscribe(m, peer);
    if (parts.size() == 4 && parts[0] == "device" && parts[2] == "calibration" && parts[3] == "fit") {
        auto it = std::find_if(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& d) { return d.id == parts[1]; });
        if (it == cfg_.devices.end()) return err("no device " + parts[1] + " (devices: " + ids_text(cfg_) + ")");
        return calibration_fit(size_t(it - cfg_.devices.begin()), m.args, peer);
    }
    if (parts.size() == 3 && parts[0] == "device") {
        auto it = std::find_if(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& d) { return d.id == parts[1]; });
        if (it == cfg_.devices.end()) return err("no device " + parts[1] + " (devices: " + ids_text(cfg_) + ")");
        size_t index = size_t(it - cfg_.devices.begin());
        if (parts[2] == "calibrate") {
            auto s = m.args.size() == 1 ? integer(m.args[0]) : std::nullopt;
            if (!s || *s < 0 || *s > 60) return err("calibrate needs a number of seconds: 1-60 to calibrate the screen (its touches pause meanwhile), 0 to end it");
            if (*s > 0 && identify_until_) return err("screens are being identified: end that first (" + addr("identify") + " 0)");
            std::string busy = calibrating_id();
            if (*s > 0 && !busy.empty() && busy != it->id) return err(busy + " is being calibrated: end that first (" + addr("device/" + busy + "/calibrate") + " 0)");
            bool was = calibrate_until_[index] != 0;
            calibrate_until_[index] = *s ? monotonic_ns() + *s * kSec : 0;
            update_modes();
            status_dirty_ = true;
            if (*s && !was)
                emit(it->id + "/calibrate", log::Level::notice, "calibrate",
                     it->label + ": guided calibration for " + std::to_string(*s) + " s, as asked by " + peer.ip + ": its touches are paused; each touch is reported",
                     it->id);
            else if (!*s && was)
                emit(it->id + "/calibrate", log::Level::notice, "calibrate", it->label + ": guided calibration ended, as asked by " + peer.ip, it->id);
            return {};
        }
        if (parts[2] == "usb_reset") {
            if (!m.args.empty()) return err("usb_reset takes no arguments");
            auto ref = usb_port_of(index);
            if (!ref) return err(it->id + " has not been seen on a USB port since touch-to-osc started and no port is set for it, so there is nothing to reset: plug the screen in again, or set match_port to the USB port it is plugged into, such as 3-3");
            collect_usb(monotonic_ns(), false);
            if (usb_worker_.resetting() || usb_asked_) return err("a USB reset is already running; try again in a moment");
            if (usb_worker_.busy()) {
                usb_asked_ = UsbAsk{index, *ref, peer.ip};
                return {};
            }
            start_usb_reset(index, *ref, peer.ip);
            return {};
        }
        return set_device(index, parts[2], m.args, peer);
    }
    return set_global(parts, m.args, peer);
}

Status Server::set_device(size_t index, const std::string& key, const std::vector<osc::Arg>& args, const Peer& peer) {
    const DeviceConfig& cur = cfg_.devices[index];
    Overrides next = over_;
    DeviceOverrides& x = next.devices[cur.id];
    std::string what = cur.id + " " + key + " ";
    auto text = [&](size_t max_len, std::string& out) -> Status {
        const std::string* s = one_string(args);
        if (!s) return err(key + " needs one string");
        std::string problem = check_text(*s, max_len);
        if (!problem.empty()) return err(key + " " + problem);
        out = *s;
        return {};
    };
    auto flag = [&](bool& out) -> Status {
        auto v = args.size() == 1 ? integer(args[0]) : std::nullopt;
        if (!v || (*v != 0 && *v != 1)) return err(key + " needs 0 or 1");
        out = *v == 1;
        return {};
    };
    auto whole = [&](long long& out) -> Status {
        auto v = args.size() == 1 ? integer(args[0]) : std::nullopt;
        if (!v) return err(key + " needs one whole number");
        out = *v;
        return {};
    };
    std::string s;
    bool b = false;
    long long n = 0;
    if (key == "label") {
        if (Status st = text(64, s); !st) return st;
        x.label = s;
        what += quoted(cur.label) + " -> " + quoted(s.empty() ? cur.id : s);
    } else if (key == "enabled") {
        if (Status st = flag(b); !st) return st;
        x.enabled = b;
        what = cur.id + (b ? " turned on" : " turned off");
    } else if (key == "rotation") {
        if (Status st = whole(n); !st) return st;
        if (std::string p = check_rotation(n); !p.empty()) return err("rotation " + p);
        x.rotation = int(n);
        what += std::to_string(cur.rotation) + " -> " + std::to_string(n);
    } else if (key == "mirror") {
        if (Status st = text(4, s); !st) return st;
        if (std::string p = check_mirror(s); !p.empty()) return err("mirror " + p);
        x.mirror = s;
        what += quoted(cur.mirror_text()) + " -> " + quoted(s);
    } else if (key == "calibration") {
        std::vector<double> c;
        for (const auto& a : args) {
            auto v = number(a);
            if (!v) return err("calibration needs six numbers, or none to remove it");
            c.push_back(*v);
        }
        if (std::string p = check_calibration(c); !p.empty()) return err("calibration " + p);
        x.calibration = c;
        what += cal_text(cur.calibration) + " -> " + cal_text(c);
    } else if (key == "aspect") {
        auto v = args.size() == 1 ? number(args[0]) : std::nullopt;
        if (!v) return err("aspect needs one number");
        if (std::string p = check_aspect(*v); !p.empty()) return err("aspect " + p);
        x.aspect = *v;
        what += num_text(cur.aspect) + " -> " + num_text(*v);
    } else if (key == "silence_ms") {
        if (Status st = whole(n); !st) return st;
        if (std::string p = check_silence(n); !p.empty()) return err("silence_ms " + p);
        x.silence_ms = int(n);
        what += std::to_string(cur.silence_ms) + " -> " + std::to_string(n);
    } else if (key == "grab") {
        if (Status st = flag(b); !st) return st;
        x.grab = b;
        what += std::string(cur.grab ? "1" : "0") + " -> " + (b ? "1" : "0");
    } else if (key == "match_usb") {
        if (Status st = text(16, s); !st) return st;
        if (std::string p = check_usb(s, nullptr, nullptr); !p.empty()) return err("match_usb " + p);
        x.match_usb = s;
        what += quoted(cur.usb_setting()) + " -> " + quoted(s);
    } else if (key == "match_serial") {
        if (Status st = text(128, s); !st) return st;
        x.match_serial = s;
        what += quoted(cur.serial) + " -> " + quoted(s);
    } else if (key == "match_port") {
        if (Status st = text(64, s); !st) return st;
        x.match_port = s;
        what += quoted(cur.port) + " -> " + quoted(s);
    } else if (key == "usb_recovery") {
        if (Status st = flag(b); !st) return st;
        x.usb_recovery = b;
        what += std::string(cur.usb_recovery ? "1" : "0") + " -> " + (b ? "1" : "0");
    } else if (std::find(kDeviceReported.begin(), kDeviceReported.end(), key) != kDeviceReported.end()) {
        bool where = key == "usb" || key == "port" || key == "name";
        return err(not_settable(key) + (where ? ": it is the screen read now; to choose which screen is read, set match_usb, match_serial or match_port" : ""));
    } else {
        std::string near = closest_name(key, kDeviceSettings);
        return err("unknown address" + (near.empty() ? std::string() : " (did you mean " + addr("device/" + cur.id + "/" + near) + "?)"));
    }
    return commit(std::move(next), peer, what, cur.id);
}

Status Server::set_global(const std::vector<std::string>& parts, const std::vector<osc::Arg>& args, const Peer& peer) {
    Overrides next = over_;
    const std::string* s = one_string(args);
    if (parts.size() == 1 && parts[0] == "log_level") {
        if (!s) return err("log_level needs one string");
        if (std::string p = check_log_level(*s); !p.empty()) return err("log_level " + p);
        next.log_level = *s;
        return commit(std::move(next), peer, "log level " + cfg_.log.level + " -> " + *s);
    }
    if (parts.size() == 2 && parts[0] == "allowed_ips" && (parts[1] == "add" || parts[1] == "remove")) {
        if (!s) return err("allowed_ips/" + parts[1] + " needs one IPv4 address");
        if (std::string p = check_ipv4(*s); !p.empty()) return err(p);
        const auto& now = cfg_.osc.allowed_ips;
        bool listed = std::find(now.begin(), now.end(), *s) != now.end();
        bool in_file = std::find(base_.osc.allowed_ips.begin(), base_.osc.allowed_ips.end(), *s) != base_.osc.allowed_ips.end();
        auto drop = [&](std::vector<std::string>& v) { v.erase(std::remove(v.begin(), v.end(), *s), v.end()); };
        if (parts[1] == "add") {
            if (listed) return err(*s + " is already allowed");
            if (now.size() >= kMaxAllowedIps) return err("at most " + std::to_string(kMaxAllowedIps) + " allowed hosts");
            drop(next.allowed_removed);
            if (!in_file) next.allowed_added.push_back(*s);
        } else {
            if (!listed) return err(*s + " is not in allowed_ips");
            drop(next.allowed_added);
            if (in_file) next.allowed_removed.push_back(*s);
        }
        return commit(std::move(next), peer, "allowed host " + *s + (parts[1] == "add" ? " added" : " removed"));
    }
    if (parts.size() == 2 && parts[0] == "saved_destinations" && (parts[1] == "add" || parts[1] == "remove")) {
        const std::string* address = args.empty() ? nullptr : std::get_if<std::string>(&args[0]);
        if (!address) return err("saved_destinations/" + parts[1] + " needs \"IP:PORT\"" + (parts[1] == "add" ? " and optionally touches and status (0 or 1)" : ""));
        DestinationConfig d;
        if (std::string p = check_destination(*address, d.address); !p.empty()) return err("the destination " + p);
        const std::string key = d.address.text();
        auto same_address = [&](const DestinationConfig& x) { return x.address.text() == key; };
        bool listed = std::any_of(cfg_.destinations.begin(), cfg_.destinations.end(), same_address);
        bool in_file = std::any_of(base_.destinations.begin(), base_.destinations.end(), same_address);
        if (parts[1] == "add") {
            if (args.size() != 1 && args.size() != 3 && args.size() != 4)
                return err("saved_destinations/add needs \"IP:PORT\", or \"IP:PORT\" touches status [plain] (each 0 or 1)");
            if (args.size() >= 3) {
                auto t = integer(args[1]), st = integer(args[2]);
                auto pl = args.size() == 4 ? integer(args[3]) : std::optional<int64_t>(0);
                if (!t || !st || !pl || (*t != 0 && *t != 1) || (*st != 0 && *st != 1) || (*pl != 0 && *pl != 1))
                    return err("touches, status and plain must each be 0 or 1");
                d.touches = *t == 1;
                d.status = *st == 1;
                d.plain = *pl == 1;
            }
            if (!d.touches && !d.status) return err("a destination needs touches or status, or both");
            if (listed) return err(key + " is already a saved destination");
            if (cfg_.destinations.size() >= kMaxDestinations) return err("at most " + std::to_string(kMaxDestinations) + " saved destinations");
            next.destinations_added.push_back(d);
        } else {
            if (args.size() != 1) return err("saved_destinations/remove needs \"IP:PORT\"");
            if (!listed) return err(key + " is not a saved destination");
            next.destinations_added.erase(std::remove_if(next.destinations_added.begin(), next.destinations_added.end(), same_address), next.destinations_added.end());
            if (in_file) next.destinations_removed.push_back(key);
        }
        return commit(std::move(next), peer, "saved destination " + (parts[1] == "add" ? destination_text(d) + " added" : key + " removed"));
    }
    if (parts.size() == 2 && parts[0] == "settings" && parts[1] == "forget") {
        if (args.empty()) {
            if (over_.empty()) return {};
            return commit(Overrides{}, peer, "every saved setting forgotten: back to the config file");
        }
        if (!s) return err("settings/forget takes the name of one setting (as /touch/overrides lists them), or nothing to forget them all");
        if (!forget_override(next, *s)) return err(check_text(*s, 128).empty() ? "nothing is saved for " + *s : "nothing is saved under that name");
        return commit(std::move(next), peer, *s + " forgotten: back to the config file");
    }
    std::string name;
    for (const auto& p : parts) name += (name.empty() ? "" : "/") + p;
    if (name == "allowed_ips" || name == "saved_destinations")
        return err(name + " is changed with " + addr(name + "/add") + " and " + addr(name + "/remove"));
    if (std::find(kReported.begin(), kReported.end(), name) != kReported.end()) return err(not_settable(name));
    std::string near = closest_name(name, kCommands);
    return err("unknown address" + (near.empty() ? std::string() : " (did you mean " + addr(near) + "?)"));
}

Status Server::commit(Overrides next, const Peer& peer, const std::string& what, std::string device) {
    drop_unchanged(next, base_);
    if (settings_text(next, Overrides(), "") == settings_text(over_, Overrides(), "")) return {};
    // the listen address, the prefix and the device list are only ever changed by starting again
    auto listen_text = [](const Overrides& o) { return o.listen ? o.listen->text() : std::string(); };
    auto added_text = [](const Overrides& o) {
        std::string s;
        for (const auto& d : o.devices_added) s += d.id + "=" + d.match_text() + ";";
        return s;
    };
    if (listen_text(next) != listen_text(over_) || next.prefix != over_.prefix || added_text(next) != added_text(over_) ||
        next.devices_removed != over_.devices_removed)
        return apply_by_restart(std::move(next), peer, what, std::string());
    Config eff = apply_overrides(base_, next);
    for (const auto& d : eff.devices) {
        if (!d.has_usb && d.port.empty()) return err("device " + d.id + " would match nothing: it needs a usb id or a port");
    }
    if (!eff.osc.open_to_any) {
        if (eff.osc.allowed_ips.empty()) return err("this would lock out every host: at least one must stay allowed");
        if (std::find(eff.osc.allowed_ips.begin(), eff.osc.allowed_ips.end(), peer.ip) == eff.osc.allowed_ips.end())
            return err("this would lock out " + peer.ip + ", the host sending this command");
    }
    Config old = cfg_;
    auto file = load_config(config_path_);
    note_config_values(over_, next, file ? *file : base_, over_values_);
    over_ = std::move(next);
    cfg_ = std::move(eff);
    apply_effects(old);
    if (!settings_dirty_) dirty_since_ = monotonic_ns();
    settings_dirty_ = true;
    status_dirty_ = true;
    emit("settings", log::Level::notice, "settings", what + ", from " + peer.ip, device);
    return {};
}

void Server::apply_effects(const Config& old) {
    if (old.log.level != cfg_.log.level) {
        log::Level l = log::Level::info;
        log::parse_level(cfg_.log.level.c_str(), l);
        log::set_min_level(l);
    }
    if (!(old.destinations == cfg_.destinations)) {
        publish_dests();
        prune(monotonic_ns());
    }
    if (old.devices.size() != shared_.size() || cfg_.devices.size() != shared_.size()) {
        TTO_ERR("internal error: the device list changed without a restart; the change waits for the next restart");
        return;
    }
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DeviceConfig& a = old.devices[i];
        const DeviceConfig& b = cfg_.devices[i];
        DeviceShared& sh = *shared_[i];
        bool wake = false;
        if (a.enabled != b.enabled) {
            sh.want_enabled.store(b.enabled);
            wake = true;
        }
        if (a.silence_ms != b.silence_ms) {
            sh.silence_ms.store(b.silence_ms);
            wake = true;
        }
        bool reopen = reopens(a, b);
        if (reopen && (a.has_usb != b.has_usb || a.vendor != b.vendor || a.product != b.product || a.serial != b.serial || a.port != b.port)) {
            usb_last_port_[i].clear();
            remember_port(i);
            usb_rec_[i].connected();
            usb_watching_[i] = false;
        }
        if (reopen) claims_.set_device(i, b);
        if (reopen || remaps(a, b)) {
            std::lock_guard<std::mutex> lock(sh.cfg_mutex);
            sh.next_cfg = b;
            cfg_gen_[i] = sh.cfg_gen.load() + 1;
            sh.cfg_gen.store(cfg_gen_[i]);
            if (reopen) restart_gen_[i] = cfg_gen_[i];
            wake = true;
        }
        if (wake) write_one(sh.wake.get());
    }
}

void Server::save_settings() {
    const std::string& path = base_.paths.settings;
    settings_dirty_ = false;
    last_save_ = monotonic_ns();
    if (path.empty()) return;
    if (over_.empty()) writer_.remove(path);
    else writer_.write(path, settings_text(over_, over_values_, config_path_));
}

void Server::save_state() {
    if (!state_path_.empty()) writer_.write(state_path_, state_text(state_));
}

void Server::collect_writes(Nanos now) {
    // the service's unit lets it write only in its state folder
    auto hint = [](const Status& st) {
        return st.error().find(std::strerror(EROFS)) == std::string::npos
                   ? std::string()
                   : "; the service can only write under /var/lib/touch-to-osc: keep paths.settings and paths.state there";
    };
    for (auto& o : writer_.outcomes()) {
        if (o.path == base_.paths.settings) {
            if (o.result) {
                settings_problem_.clear();
                settings_file_ = o.removed ? std::string() : o.path;
            } else {
                settings_problem_ = o.removed ? "removing the settings file " + o.path + " failed (" + o.result.error() + "): its old settings come back at a restart"
                                              : "saving the settings to " + o.path + " failed (" + o.result.error() +
                                                    "): changes made over OSC are lost at a restart; trying again every 10 s";
                settings_problem_ += hint(o.result);
                emit("settings-save", log::Level::err, "settings", settings_problem_);
                if (!settings_dirty_) dirty_since_ = now;
                settings_dirty_ = true;
                last_save_ = now + 9 * kSec;
            }
            status_dirty_ = true;
        } else if (o.path == state_path_) {
            std::string was = state_problem_;
            state_problem_ = o.result ? std::string()
                                      : "keeping what touch-to-osc learns between runs in " + o.path + " failed (" + o.result.error() +
                                            "): the USB port each screen was last on and the copy of the config that last worked are not kept" + hint(o.result);
            if (!state_problem_.empty() && was.empty()) emit("state-save", log::Level::warning, "settings", state_problem_);
            if (state_problem_ != was) status_dirty_ = true;
        }
    }
}

void Server::remember_port(size_t index) {
    const std::string& id = cfg_.devices[index].id;
    auto it = state_.usb_ports.find(id);
    std::string was = it == state_.usb_ports.end() ? std::string() : it->second;
    if (was == usb_last_port_[index]) return;
    if (auto ref = parse_usb_port(usb_last_port_[index]))
        emit(id + "/usb-port", log::Level::notice, "usb",
             id + " is on USB port " + ref->path + "; a USB reset for it resets " + usb_target_text(*ref) + " (sudo touch-to-osc --list names what else is on it)", id);
    if (usb_last_port_[index].empty()) state_.usb_ports.erase(id);
    else state_.usb_ports[id] = usb_last_port_[index];
    save_state();
}

void Server::repair_usb_hub() {
    std::string marker = usb_reset_marker(state_path_);
    if (marker.empty()) return;
    std::string done;
    Status st = bind_hub_left_unbound(marker, usb_worker_.paths(), &done);
    if (!st) {
        usb_problem_ = st.error();
        TTO_ERR("%s", usb_problem_.c_str());
    } else if (!done.empty()) {
        TTO_WARN("%s", done.c_str());
    }
}

Status Server::subscribe(const osc::Message& m, const Peer& peer) {
    const char* usage = "subscribe needs a port (1-65535), optionally a lease of 1-60 seconds, and optionally what to get: all, touches or status, "
                        "with plain for no 64-bit arguments (\"touches plain\")";
    auto port = m.args.empty() ? std::nullopt : integer(m.args[0]);
    if (!port || *port < 1 || *port > 65535) return err(usage);
    auto refused = [&](const std::string& why) {
        if (*port != peer.port) send_to(Peer{peer.ip, uint16_t(*port)}, {{addr("error"), {m.address, why}}});
        return err(why);
    };
    if (m.args.size() > 3) return refused(usage);
    int64_t secs = 10;
    size_t next = 1;
    if (m.args.size() > next && !std::holds_alternative<std::string>(m.args[next])) {
        auto s = integer(m.args[next]);
        if (!s || *s < 1 || *s > 60) return refused(usage);
        secs = *s;
        ++next;
    }
    bool touches = true, status = true, plain = false;
    std::string what = "all";
    if (m.args.size() > next) {
        const std::string* w = std::get_if<std::string>(&m.args[next]);
        if (!w || m.args.size() > next + 1) return refused(usage);
        int kinds = 0;
        std::string word;
        std::istringstream words(*w);
        while (words >> word) {
            if (word == "plain") plain = true;
            else if (word == "all" || word == "touches" || word == "status") {
                ++kinds;
                what = word;
                touches = word != "status";
                status = word != "touches";
            } else {
                return refused("subscribe: " + (check_text(word, 32).empty() ? "\"" + word + "\"" : std::string("that word")) + " is not one of all, touches, status, plain");
            }
        }
        if (kinds > 1) return refused("subscribe takes one of all, touches and status");
    }
    if (plain) what += " plain";
    Nanos now = monotonic_ns();
    expire_leases(now);
    Peer to{peer.ip, uint16_t(*port)};
    std::vector<osc::Message> ack{{addr("subscribed"), {int32_t(*port), int32_t(secs), what}}};
    send_status_changes(false);
    const Lease* before = nullptr;
    for (const auto& l : leases_.all()) {
        if (l.peer == to) before = &l;
    }
    bool had_status = before && before->status;
    bool changed = before && (before->touches != touches || before->status != status || before->plain != plain);
    switch (leases_.subscribe(to, int(secs), now, touches, status, plain)) {
    case Leases::Outcome::renewed:
        TTO_DEBUG("subscriber %s renewed (lease %lld s)", to.text().c_str(), (long long)secs);
        send_to(to, ack);
        if (changed) {
            publish_dests();
            if (status && !had_status) send_full_status(to, plain, false);
        }
        return {};
    case Leases::Outcome::full:
        return refused("at most " + std::to_string(kMaxLeases) + " subscribers at once (" + prefix_ +
                       "/subscribers lists them): one has to unsubscribe or let its lease run out");
    case Leases::Outcome::added: break;
    }
    publish_dests();
    send_to(to, ack);
    if (status) send_full_status(to, plain, false);
    emit("subscriber", log::Level::notice, "subscriber", to.text() + " added (lease " + std::to_string(secs) + " s" + (what == "all" ? "" : ", " + what) + ")");
    return {};
}

Status Server::unsubscribe(const osc::Message& m, const Peer& peer) {
    auto port = m.args.size() == 1 ? integer(m.args[0]) : std::nullopt;
    if (!port || *port < 1 || *port > 65535) return err("unsubscribe needs the port it subscribed with");
    Peer to{peer.ip, uint16_t(*port)};
    if (leases_.unsubscribe(to)) {
        publish_dests();
        status_dirty_ = true;
        emit("subscriber", log::Level::notice, "subscriber", to.text() + " ended (it unsubscribed)");
    }
    send_to(to, {{addr("unsubscribed"), {int32_t(*port)}}});
    return {};
}

}

#include "control/server.h"

#include "config/atomic_file.h"
#include "core/log.h"
#include "core/process.h"

#include "tto_version.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace tto {

namespace {

constexpr Nanos kTick = 100 * kMs;
constexpr Nanos kFullEvery = 10 * kSec;
constexpr Nanos kStatusReplyEvery = 200 * kMs;
constexpr Nanos kLatencyWindow = 10 * kSec;
constexpr size_t kMaxLatencySamples = 4096;
constexpr Nanos kHealthWindow = 60 * kSec;
constexpr Nanos kReportEvery = 10 * kSec;
constexpr Nanos kSaveSettle = 300 * kMs;
constexpr Nanos kSaveEvery = kSec;
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

std::vector<std::vector<uint8_t>> pack(const std::vector<osc::Message>& msgs) {
    std::vector<std::vector<uint8_t>> out;
    std::vector<osc::Message> group;
    size_t size = 16;
    for (const auto& m : msgs) {
        size_t n = osc::encode(m).size() + 4;
        if (!group.empty() && size + n > kBundleBytes) {
            out.push_back(osc::encode_bundle(group));
            group.clear();
            size = 16;
        }
        group.push_back(m);
        size += n;
    }
    if (!group.empty()) out.push_back(osc::encode_bundle(group));
    return out;
}

}

Server::Server(Config base, SettingsLoad settings, std::string config_path, SysPaths paths)
    : base_(std::move(base)),
      over_(std::move(settings.overrides)),
      cfg_(apply_overrides(base_, over_)),
      settings_notes_(std::move(settings.notes)),
      settings_load_problem_(std::move(settings.problem)),
      settings_file_(settings.file_exists && !over_.empty() ? base_.paths.settings : std::string()),
      config_path_(std::move(config_path)),
      paths_(std::move(paths)),
      prefix_(base_.osc.prefix) {
    char host[256] = {};
    if (gethostname(host, sizeof host - 1) == 0) host_ = host;
    if (!over_.empty() && !settings.file_exists) settings_dirty_ = true;
}

Server::~Server() {
    for (auto& sh : shared_) {
        sh->stop.store(true);
        if (sh->wake) write_one(sh->wake.get());
    }
    for (auto& r : readers_) r->join();
}

bool Server::open_sockets() {
    send_fd_.reset(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    UniqueFd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd || !send_fd_) {
        TTO_CRIT("cannot create a UDP socket: %s", errno_str().c_str());
        return false;
    }
    int one = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    to_sockaddr(Peer{cfg_.osc.listen.host, uint16_t(cfg_.osc.listen.port)}, a);
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        TTO_CRIT("cannot listen on UDP %s:%d (%s): another program uses the port, or the address is not this machine's; change osc.listen in %s",
                 cfg_.osc.listen.host.c_str(), cfg_.osc.listen.port, errno_str().c_str(), config_path_.c_str());
        return false;
    }
    osc_fd_ = std::move(fd);
    return true;
}

void Server::banner() {
    TTO_NOTICE("touch-to-osc %s (osc_api %d), run %d, config %s", TTO_VERSION, kOscApi, run_, config_path_.c_str());
    std::string from = cfg_.osc.open_to_any ? std::string("ANY HOST") : std::to_string(cfg_.osc.allowed_ips.size()) + " allowed host(s)";
    TTO_NOTICE("OSC on UDP %s:%d, prefix %s, commands and subscriptions from %s", cfg_.osc.listen.host.c_str(), cfg_.osc.listen.port, prefix_.c_str(), from.c_str());
    for (const auto& d : cfg_.devices) {
        std::string mirror = d.mirror_text();
        TTO_NOTICE("device %s (%s): %s, rotation %d%s%s, silence %s%s", d.id.c_str(), d.label.c_str(), d.match_text().c_str(), d.rotation,
                   mirror.empty() ? "" : ", mirror ", mirror.c_str(), d.silence_ms ? (std::to_string(d.silence_ms) + " ms").c_str() : "off",
                   d.enabled ? "" : ", turned off in the config");
    }
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
    timer_fd_.reset(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!notify_fd_ || !timer_fd_ || !signal_fd_) {
        TTO_CRIT("cannot create the event handles: %s", errno_str().c_str());
        return 4;
    }
    itimerspec its{};
    its.it_interval.tv_nsec = long(kTick);
    its.it_value.tv_nsec = long(kTick);
    timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
    banner();
    for (const auto& d : cfg_.devices) {
        auto sh = std::make_unique<DeviceShared>();
        sh->wake.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (!sh->wake) {
            TTO_CRIT("cannot create the event handles: %s", errno_str().c_str());
            return 4;
        }
        readers_.push_back(std::make_unique<DeviceReader>(d, prefix_, *sh, notify_fd_.get(), claims_, paths_));
        shared_.push_back(std::move(sh));
        stats_.emplace_back();
        cfg_gen_.push_back(0);
        restart_gen_.push_back(0);
    }
    rx_.resize(65536);
    next_second_ = monotonic_ns() + kSec;
    publish_dests();
    for (auto& r : readers_) r->start();
    sd_notify_msg("READY=1");

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
            send_status_changes(false);
        }
        if (p[3].revents) {
            drain(timer_fd_.get());
            on_tick();
        }
        if (p[0].revents) on_osc();
    }

    sd_notify_msg("STOPPING=1");
    for (auto& sh : shared_) {
        sh->stop.store(true);
        write_one(sh->wake.get());
    }
    for (auto& r : readers_) r->join();
    drain_reader_events();
    if (settings_dirty_) save_settings();
    send_bye();
    TTO_NOTICE("stopped");
    return 0;
}

void Server::on_tick() {
    Nanos now = monotonic_ns();
    drain_reader_events();
    drain_latency(now);
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

void Server::emit(const std::string& key, log::Level level, const std::string& kind, const std::string& text) {
    Limit& l = limits_[key];
    l.kind = kind;
    if (!take_token(l.tokens, l.last, monotonic_ns(), kEventBurst, kEventsPerSecond)) {
        ++l.suppressed;
        return;
    }
    log::write(level, "%s: %s", kind.c_str(), text.c_str());
    send_event(kind, text);
}

void Server::report_suppressed(Nanos now) {
    for (auto& [key, l] : limits_) {
        if (!l.suppressed || !take_token(l.tokens, l.last, now, kEventBurst, kEventsPerSecond)) continue;
        std::string text = std::to_string(l.suppressed) + " more \"" + l.kind + "\" events (" + key + ") came too fast to report one by one";
        l.suppressed = 0;
        TTO_WARN("%s", text.c_str());
        send_event(l.kind, text);
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
        while (shared_[i]->events.pop(e)) {
            any = true;
            emit(cfg_.devices[i].id + "/" + event_kind_name(e.kind), log::Level(e.level), event_kind_name(e.kind), e.text);
        }
        if (any) status_dirty_ = true;
        uint64_t lost = shared_[i]->lost_events.load(std::memory_order_relaxed);
        if (lost != stats_[i].lost_events_seen) {
            emit(cfg_.devices[i].id + "/lost", log::Level::warning, "device",
                 cfg_.devices[i].id + ": " + std::to_string(lost - stats_[i].lost_events_seen) + " device messages were dropped because they came too fast");
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
        st.samples.push_back(Sample{now, sh.resyncs.load(), sh.silences.load(), sh.send_drops.load()});
        while (st.samples.size() > 2 && now - st.samples.front().at > kHealthWindow) st.samples.pop_front();
    }
}

std::vector<Server::Target> Server::targets() const {
    std::vector<Target> out;
    auto add = [&](const Peer& p, bool touches, bool status) {
        for (auto& t : out) {
            if (t.peer == p) {
                t.touches = t.touches || touches;
                t.status = t.status || status;
                return;
            }
        }
        out.push_back(Target{p, touches, status});
    };
    for (const auto& d : cfg_.destinations) add(Peer{d.address.host, uint16_t(d.address.port)}, d.touches, d.status);
    for (const auto& l : leases_.all()) add(l.peer, true, true);
    return out;
}

void Server::publish_dests() {
    auto tg = targets();
    for (auto& sh : shared_) {
        DestSet& d = sh->dests.back();
        d.n = 0;
        for (const auto& t : tg) {
            if (!t.touches || d.n >= kMaxSendTargets) continue;
            if (to_sockaddr(t.peer, d.addr[d.n])) ++d.n;
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
        send_event("send", text);
    }
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
    std::vector<osc::Message> changed;
    for (auto& m : status_messages()) {
        auto bytes = osc::encode(m);
        auto it = last_status_.find(m.address);
        if (full || it == last_status_.end() || it->second != bytes) {
            last_status_[m.address] = std::move(bytes);
            changed.push_back(std::move(m));
        }
    }
    if (changed.empty()) return;
    auto bundles = pack(changed);
    for (const auto& t : tg) {
        for (const auto& b : bundles) send_target(t, b);
    }
}

void Server::send_bundled(const Peer& to, const std::vector<osc::Message>& msgs) {
    for (const auto& b : pack(msgs)) send_bytes(to, b);
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
    base[0].args = {alive_, run_, std::string(TTO_VERSION), h.level, connected, int32_t(cfg_.devices.size()), contacts};
    auto plain = osc::encode_bundle(base);
    std::vector<uint8_t> with_counters;
    for (const auto& t : tg) {
        if (t.status) {
            if (with_counters.empty()) {
                auto all = base;
                for (auto& m : counter_messages(now)) all.push_back(std::move(m));
                with_counters = osc::encode_bundle(all);
            }
            send_target(t, with_counters);
        } else {
            send_target(t, plain);
        }
    }
}

void Server::send_event(const std::string& kind, const std::string& text) {
    auto bytes = osc::encode({addr("event"), {kind, text}});
    for (const auto& t : targets()) {
        if (t.status) send_target(t, bytes);
    }
}

void Server::send_bye() {
    auto bytes = osc::encode({addr("bye"), {run_}});
    for (const auto& t : targets()) send_target(t, bytes);
}

Server::Health Server::health(Nanos now) {
    std::vector<std::string> faults, warnings;
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DeviceConfig& d = cfg_.devices[i];
        const DevInfo& info = shared_[i]->info.latest();
        if (info.cfg_gen < restart_gen_[i] || info.state == DevState::restarting) {
            warnings.push_back(d.label + " is restarting after a settings change");
        } else if (!info.enabled) {
            warnings.push_back(d.label + " is turned off" + (base_.devices[i].enabled ? " (a saved setting)" : " in the config file"));
        } else {
            switch (info.state) {
            case DevState::ok: break;
            case DevState::starting: warnings.push_back(d.label + " is starting"); break;
            default: faults.push_back(d.label + ": " + info.reason); break;
            }
        }
        const auto& s = stats_[i].samples;
        if (s.size() >= 2) {
            uint64_t resyncs = s.back().resyncs - s.front().resyncs;
            uint64_t silences = s.back().silences - s.front().silences;
            uint64_t drops = s.back().drops - s.front().drops;
            if (resyncs) warnings.push_back(d.label + ": the kernel dropped touch events " + std::to_string(resyncs) + " time(s) in the last minute; the machine may be overloaded");
            if (silences) warnings.push_back(d.label + " went silent with fingers down " + std::to_string(silences) + " time(s) in the last minute: check its USB cable");
            if (drops) warnings.push_back(d.label + ": " + std::to_string(drops) + " touch message(s) could not be sent in the last minute");
        }
    }
    for (const auto& [key, h] : dest_health_) {
        if (h.failing) warnings.push_back("sending to " + key + " fails: " + h.error);
    }
    if (!settings_load_problem_.empty()) warnings.push_back(settings_load_problem_);
    if (!settings_problem_.empty()) warnings.push_back(settings_problem_);
    (void)now;
    Health out;
    out.level = !faults.empty() ? "fault" : !warnings.empty() ? "warning" : "ok";
    out.reasons = faults;
    out.reasons.insert(out.reasons.end(), warnings.begin(), warnings.end());
    return out;
}

std::string Server::disabled_reason(size_t index) const {
    std::string on = "send " + prefix_ + "/device/" + cfg_.devices[index].id + "/enabled 1 to turn it back on";
    if (!base_.devices[index].enabled) return "turned off in the config file (enabled = false): set it to true there, or " + on;
    return "turned off by a saved setting: " + on;
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
    add("host", {host_});
    add("config", {config_path_});
    add("settings_file", {settings_file_});
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
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DeviceConfig& d = cfg_.devices[i];
        const DevInfo& info = shared_[i]->info.latest();
        bool restarting = info.cfg_gen < restart_gen_[i];
        std::string b = "device/" + d.id + "/";
        add(b + "label", {d.label});
        add(b + "enabled", {int32_t(info.enabled ? 1 : 0)});
        add(b + "connected", {int32_t(info.connected ? 1 : 0)});
        add(b + "state", {std::string(restarting ? "restarting" : state_name(info.state))});
        add(b + "reason", {restarting ? std::string("restarting after a settings change") : !info.enabled ? disabled_reason(i) : std::string(info.reason)});
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

std::vector<osc::Message> Server::counter_messages(Nanos now) {
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
                           int64_t(sh.cancels.load())});
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
    return out;
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
        auto msgs = osc::decode_packet(rx_.data(), size_t(got));
        if (!msgs) {
            TTO_LOG_EVERY(30 * kSec, log::Level::warning, "a malformed OSC packet from %s was dropped", ip);
            continue;
        }
        bool allowed = cfg_.osc.open_to_any || std::find(cfg_.osc.allowed_ips.begin(), cfg_.osc.allowed_ips.end(), peer.ip) != cfg_.osc.allowed_ips.end();
        if (!allowed) {
            if (!msgs->empty()) refuse(peer, msgs->front().address, peer.ip + " may not send commands here: add it to osc.allowed_ips in " + config_path_, true);
            continue;
        }
        for (const auto& m : *msgs) command(m, peer);
    }
    if (status_dirty_) send_status_changes(false);
}

void Server::refuse(const Peer& peer, const std::string& address, const std::string& why, bool limited) {
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
    send_to(peer, {{addr("error"), {address, why}}});
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
        send_to(peer, {{addr("reply"), {s, std::string(ok ? "ok" : "refused"), note}}});
    };
    Nanos now = monotonic_ns();
    auto bytes = osc::encode(inner);
    const KeptReplies::Kept* k = nullptr;
    switch (kept_.find(peer, s, bytes, now, &k)) {
    case KeptReplies::Lookup::repeat: answer(k->ok, k->note); return k->ok;
    case KeptReplies::Lookup::conflict: answer(false, "seq used for another command"); return false;
    case KeptReplies::Lookup::none: break;
    }
    Status st = inner.address == addr("request") ? Status(err("a request cannot carry another request")) : apply(inner, peer);
    if (!st) TTO_LOG_EVERY(5 * kSec, log::Level::warning, "OSC %s from %s refused: %s", inner.address.c_str(), peer.ip.c_str(), st.error().c_str());
    kept_.keep(peer, s, bytes, st.ok(), st.ok() ? std::string() : st.error(), now);
    answer(st.ok(), st.ok() ? std::string() : st.error());
    return st.ok();
}

Status Server::apply(const osc::Message& m, const Peer& peer) {
    if (m.address.size() <= prefix_.size() + 1 || m.address.compare(0, prefix_.size() + 1, prefix_ + "/") != 0) return err("unknown address");
    auto parts = split(m.address.substr(prefix_.size() + 1));
    auto reply_peer = [&]() {
        Peer to = peer;
        if (m.args.size() == 1) {
            auto port = integer(m.args[0]);
            if (port && *port > 0 && *port < 65536) to.port = uint16_t(*port);
        }
        return to;
    };
    if (parts.size() == 1 && parts[0] == "ping") {
        if (m.args.size() > 1) return err("ping takes no arguments, or the port to answer to");
        send_to(reply_peer(), {{addr("pong"), {run_}}});
        return {};
    }
    if (parts.size() == 2 && parts[0] == "status" && parts[1] == "send") {
        Nanos now = monotonic_ns();
        if (status_reply_at_ && now - status_reply_at_ < kStatusReplyEvery) return err("at most 5 status requests a second");
        status_reply_at_ = now;
        auto msgs = status_messages();
        for (auto& c : counter_messages(now)) msgs.push_back(std::move(c));
        send_bundled(reply_peer(), msgs);
        return {};
    }
    if (parts.size() == 1 && parts[0] == "subscribe") return subscribe(m, peer);
    if (parts.size() == 1 && parts[0] == "unsubscribe") return unsubscribe(m, peer);
    if (parts.size() == 3 && parts[0] == "device") {
        auto it = std::find_if(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& d) { return d.id == parts[1]; });
        if (it == cfg_.devices.end()) return err("no device " + parts[1]);
        return set_device(size_t(it - cfg_.devices.begin()), parts[2], m.args, peer);
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
    } else {
        return err("unknown address");
    }
    return commit(std::move(next), peer, what);
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
        std::vector<std::string> list = cfg_.osc.allowed_ips;
        auto it = std::find(list.begin(), list.end(), *s);
        if (parts[1] == "add") {
            if (it != list.end()) return err(*s + " is already allowed");
            if (list.size() >= kMaxAllowedIps) return err("at most " + std::to_string(kMaxAllowedIps) + " allowed hosts");
            list.push_back(*s);
        } else {
            if (it == list.end()) return err(*s + " is not in allowed_ips");
            list.erase(it);
        }
        next.allowed_ips = list;
        return commit(std::move(next), peer, "allowed host " + *s + (parts[1] == "add" ? " added" : " removed"));
    }
    if (parts.size() == 2 && parts[0] == "saved_destinations" && (parts[1] == "add" || parts[1] == "remove")) {
        const std::string* address = args.empty() ? nullptr : std::get_if<std::string>(&args[0]);
        if (!address) return err("saved_destinations/" + parts[1] + " needs \"IP:PORT\"" + (parts[1] == "add" ? " and optionally touches and status (0 or 1)" : ""));
        DestinationConfig d;
        if (std::string p = check_destination(*address, d.address); !p.empty()) return err("the destination " + p);
        std::vector<DestinationConfig> list = cfg_.destinations;
        auto it = std::find_if(list.begin(), list.end(), [&](const DestinationConfig& x) { return x.address.text() == d.address.text(); });
        if (parts[1] == "add") {
            if (args.size() != 1 && args.size() != 3) return err("saved_destinations/add needs \"IP:PORT\", or \"IP:PORT\" touches status (each 0 or 1)");
            if (args.size() == 3) {
                auto t = integer(args[1]), st = integer(args[2]);
                if (!t || !st || (*t != 0 && *t != 1) || (*st != 0 && *st != 1)) return err("touches and status must each be 0 or 1");
                d.touches = *t == 1;
                d.status = *st == 1;
            }
            if (!d.touches && !d.status) return err("a destination needs touches or status, or both");
            if (it != list.end()) return err(d.address.text() + " is already a saved destination");
            if (list.size() >= kMaxDestinations) return err("at most " + std::to_string(kMaxDestinations) + " saved destinations");
            list.push_back(d);
        } else {
            if (args.size() != 1) return err("saved_destinations/remove needs \"IP:PORT\"");
            if (it == list.end()) return err(d.address.text() + " is not a saved destination");
            list.erase(it);
        }
        next.saved_destinations = list;
        return commit(std::move(next), peer, "saved destination " + (parts[1] == "add" ? destination_text(d) + " added" : d.address.text() + " removed"));
    }
    if (parts.size() == 2 && parts[0] == "settings" && parts[1] == "forget") {
        if (args.empty()) {
            if (over_.empty()) return {};
            return commit(Overrides{}, peer, "every saved setting forgotten: back to the config file");
        }
        if (!s) return err("settings/forget takes the name of one setting (as /touch/overrides lists them), or nothing to forget them all");
        if (!forget_override(next, *s)) return err("nothing is saved for " + *s);
        return commit(std::move(next), peer, *s + " forgotten: back to the config file");
    }
    return err("unknown address");
}

Status Server::commit(Overrides next, const Peer& peer, const std::string& what) {
    drop_unchanged(next, base_);
    if (settings_text(next, "") == settings_text(over_, "")) return {};
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
    over_ = std::move(next);
    cfg_ = std::move(eff);
    apply_effects(old);
    if (!settings_dirty_) dirty_since_ = monotonic_ns();
    settings_dirty_ = true;
    status_dirty_ = true;
    emit("settings", log::Level::notice, "settings", what + ", from " + peer.ip);
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
    Nanos now = monotonic_ns();
    settings_dirty_ = false;
    status_dirty_ = true;
    last_save_ = now;
    if (path.empty()) return;
    if (over_.empty()) {
        if (unlink(path.c_str()) == 0 || errno == ENOENT) {
            settings_file_.clear();
            settings_problem_.clear();
        } else {
            settings_problem_ = "removing the settings file " + path + " failed (" + errno_str() + "): its old settings come back at a restart";
            emit("settings-save", log::Level::err, "settings", settings_problem_);
        }
        return;
    }
    Status st = write_file_atomic(path, settings_text(over_, config_path_));
    if (!st) {
        settings_problem_ = "saving the settings to " + path + " failed (" + st.error() + "): changes made over OSC are lost at a restart; trying again every 10 s";
        emit("settings-save", log::Level::err, "settings", settings_problem_);
        settings_dirty_ = true;
        last_save_ = now + 9 * kSec;
        return;
    }
    settings_problem_.clear();
    settings_file_ = path;
}

Status Server::subscribe(const osc::Message& m, const Peer& peer) {
    const char* usage = "subscribe needs a port (1-65535) and optionally a lease of 1-60 seconds";
    if (m.args.empty() || m.args.size() > 2) return err(usage);
    auto port = integer(m.args[0]);
    if (!port || *port < 1 || *port > 65535) return err(usage);
    int64_t secs = 10;
    if (m.args.size() == 2) {
        auto s = integer(m.args[1]);
        if (!s || *s < 1 || *s > 60) return err(usage);
        secs = *s;
    }
    Nanos now = monotonic_ns();
    expire_leases(now);
    Peer to{peer.ip, uint16_t(*port)};
    std::vector<osc::Message> ack{{addr("subscribed"), {int32_t(*port), int32_t(secs)}}};
    send_status_changes(false);
    switch (leases_.subscribe(to, int(secs), now)) {
    case Leases::Outcome::renewed:
        TTO_DEBUG("subscriber %s renewed (lease %lld s)", to.text().c_str(), (long long)secs);
        send_to(to, ack);
        return {};
    case Leases::Outcome::full: return err("at most " + std::to_string(kMaxLeases) + " subscribers; this one is refused");
    case Leases::Outcome::added: break;
    }
    publish_dests();
    auto full = status_messages();
    for (const auto& msg : full) last_status_[msg.address] = osc::encode(msg);
    send_to(to, ack);
    send_bundled(to, full);
    emit("subscriber", log::Level::notice, "subscriber", to.text() + " added (lease " + std::to_string(secs) + " s)");
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

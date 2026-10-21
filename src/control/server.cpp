#include "control/server.h"

#include "core/log.h"
#include "core/process.h"

#include "tto_version.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <csignal>
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
constexpr Nanos kHealthWindow = 60 * kSec;
constexpr Nanos kReportEvery = 10 * kSec;

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

}

Server::Server(Config cfg, std::string config_path, SysPaths paths)
    : cfg_(std::move(cfg)), config_path_(std::move(config_path)), paths_(std::move(paths)), prefix_(cfg_.osc.prefix) {}

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
    }
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
    send_bye();
    TTO_NOTICE("stopped");
    return 0;
}

void Server::on_tick() {
    Nanos now = monotonic_ns();
    drain_reader_events();
    drain_latency(now);
    if (++ticks_ % 10 == 0) every_second(now);
    else send_status_changes(false);
}

void Server::every_second(Nanos now) {
    for (const auto& p : leases_.expire(now)) {
        publish_dests();
        TTO_NOTICE("subscriber %s ended (its lease ran out)", p.text().c_str());
        send_event("subscriber", p.text() + " ended");
    }
    sample(now);
    bool full = now - full_at_ >= kFullEvery;
    if (full) full_at_ = now;
    send_status_changes(full);
    send_alive(now);
    report_destinations(now);
    if (sd_watchdog_enabled()) sd_notify_msg("WATCHDOG=1");
}

void Server::drain_reader_events() {
    for (size_t i = 0; i < shared_.size(); ++i) {
        ReaderEvent e;
        while (shared_[i]->events.pop(e)) {
            log::write(log::Level(e.level), "%s", e.text);
            send_event(event_kind_name(e.kind), e.text);
        }
        uint64_t lost = shared_[i]->lost_events.load(std::memory_order_relaxed);
        if (lost != stats_[i].lost_events_seen) {
            TTO_WARN("%s: %llu device messages were dropped because they came too fast", cfg_.devices[i].id.c_str(),
                     (unsigned long long)(lost - stats_[i].lost_events_seen));
            stats_[i].lost_events_seen = lost;
        }
    }
}

void Server::drain_latency(Nanos now) {
    for (size_t i = 0; i < shared_.size(); ++i) {
        uint32_t v;
        auto& lat = stats_[i].latency;
        while (shared_[i]->latency_us.pop(v)) lat.emplace_back(now, v);
        while (!lat.empty() && now - lat.front().first > kLatencyWindow) lat.pop_front();
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
    std::vector<Target> tg;
    for (const auto& t : targets()) {
        if (t.status) tg.push_back(t);
    }
    if (tg.empty()) {
        last_status_.clear();
        return;
    }
    std::vector<std::vector<uint8_t>> send;
    for (const auto& m : status_messages()) {
        auto bytes = osc::encode(m);
        auto it = last_status_.find(m.address);
        if (full || it == last_status_.end() || it->second != bytes) {
            last_status_[m.address] = bytes;
            send.push_back(std::move(bytes));
        }
    }
    for (const auto& t : tg) {
        for (const auto& b : send) send_target(t, b);
    }
}

void Server::send_alive(Nanos now) {
    ++alive_;
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
        if (!info.enabled) {
            warnings.push_back(d.label + " is turned off by a command");
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
    (void)now;
    Health out;
    out.level = !faults.empty() ? "fault" : !warnings.empty() ? "warning" : "ok";
    out.reasons = faults;
    out.reasons.insert(out.reasons.end(), warnings.begin(), warnings.end());
    return out;
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
    std::vector<osc::Arg> ids;
    for (const auto& d : cfg_.devices) ids.emplace_back(d.id);
    add("devices", ids);
    for (size_t i = 0; i < shared_.size(); ++i) {
        const DeviceConfig& d = cfg_.devices[i];
        const DevInfo& info = shared_[i]->info.latest();
        std::string b = "device/" + d.id + "/";
        add(b + "label", {d.label});
        add(b + "enabled", {int32_t(info.enabled ? 1 : 0)});
        add(b + "connected", {int32_t(info.connected ? 1 : 0)});
        add(b + "state", {std::string(state_name(info.state))});
        add(b + "reason", {std::string(info.reason)});
        add(b + "name", {std::string(info.connected ? info.name : "")});
        add(b + "usb", {std::string(info.connected ? info.usb : "")});
        add(b + "port", {std::string(info.connected ? info.port : "")});
        add(b + "slots", {int32_t(info.connected ? info.slots : 0)});
        add(b + "size_mm", {info.connected ? info.w_mm : 0.0f, info.connected ? info.h_mm : 0.0f});
        add(b + "rotation", {int32_t(d.rotation)});
        add(b + "mirror", {d.mirror_text()});
        add(b + "aspect", {info.aspect > 0 ? info.aspect : float(d.aspect)});
        add(b + "silence_ms", {int32_t(d.silence_ms)});
        add(b + "grabbed", {int32_t(info.connected && info.grabbed ? 1 : 0)});
    }
    return out;
}

std::vector<osc::Message> Server::counter_messages(Nanos now) {
    std::vector<osc::Message> out;
    auto add = [&](const std::string& rest, std::vector<osc::Arg> args) { out.push_back({addr(rest), std::move(args)}); };
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
        uint8_t buf[8192];
        sockaddr_in from{};
        socklen_t len = sizeof from;
        ssize_t got = recvfrom(osc_fd_.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (got < 0) return;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        Peer peer{ip, ntohs(from.sin_port)};
        auto msgs = osc::decode_packet(buf, size_t(got));
        if (!msgs) {
            TTO_LOG_EVERY(30 * kSec, log::Level::warning, "a malformed OSC packet from %s was dropped", ip);
            continue;
        }
        bool allowed = cfg_.osc.open_to_any || std::find(cfg_.osc.allowed_ips.begin(), cfg_.osc.allowed_ips.end(), peer.ip) != cfg_.osc.allowed_ips.end();
        for (const auto& m : *msgs) {
            if (!allowed) {
                refuse(peer, m.address, peer.ip + " may not send commands here: add it to osc.allowed_ips in " + config_path_, true);
                continue;
            }
            command(m, peer);
        }
    }
}

void Server::refuse(const Peer& peer, const std::string& address, const std::string& why, bool limited) {
    if (limited) {
        Nanos now = monotonic_ns();
        Nanos& at = refused_at_[peer.ip];
        if (at && now - at < 5 * kSec) return;
        at = now;
    }
    TTO_LOG_EVERY(5 * kSec, log::Level::warning, "OSC %s from %s refused: %s", address.c_str(), peer.ip.c_str(), why.c_str());
    send_to(peer, {{addr("error"), {address, why}}});
}

bool Server::command(const osc::Message& m, const Peer& peer) {
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
        send_to(reply_peer(), msgs);
        return {};
    }
    if (parts.size() == 1 && parts[0] == "subscribe") return subscribe(m, peer);
    if (parts.size() == 1 && parts[0] == "unsubscribe") return unsubscribe(m, peer);
    if (parts.size() == 3 && parts[0] == "device" && parts[2] == "enabled") {
        auto it = std::find_if(cfg_.devices.begin(), cfg_.devices.end(), [&](const DeviceConfig& d) { return d.id == parts[1]; });
        if (it == cfg_.devices.end()) return err("no device " + parts[1]);
        auto v = m.args.size() == 1 ? integer(m.args[0]) : std::nullopt;
        if (!v || (*v != 0 && *v != 1)) return err("enabled needs 0 or 1");
        DeviceShared& sh = *shared_[size_t(it - cfg_.devices.begin())];
        bool want = *v == 1;
        if (sh.want_enabled.exchange(want) != want) {
            TTO_NOTICE("device %s %s over OSC from %s", it->id.c_str(), want ? "turned on" : "turned off", peer.ip.c_str());
            write_one(sh.wake.get());
        }
        return {};
    }
    return err("unknown address");
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
    for (const auto& p : leases_.expire(now)) {
        TTO_NOTICE("subscriber %s ended (its lease ran out)", p.text().c_str());
        send_event("subscriber", p.text() + " ended");
    }
    Peer to{peer.ip, uint16_t(*port)};
    std::vector<osc::Message> ack{{addr("subscribed"), {int32_t(*port), int32_t(secs)}}};
    send_status_changes(false);
    switch (leases_.subscribe(to, int(secs), now)) {
    case Leases::Outcome::renewed: send_to(to, ack); return {};
    case Leases::Outcome::full: return err("at most " + std::to_string(kMaxLeases) + " subscribers; this one is refused");
    case Leases::Outcome::added: break;
    }
    publish_dests();
    auto full = status_messages();
    for (const auto& msg : full) last_status_[msg.address] = osc::encode(msg);
    ack.insert(ack.end(), full.begin(), full.end());
    send_to(to, ack);
    TTO_NOTICE("subscriber %s added (lease %lld s)", to.text().c_str(), (long long)secs);
    send_event("subscriber", to.text() + " added");
    return {};
}

Status Server::unsubscribe(const osc::Message& m, const Peer& peer) {
    auto port = m.args.size() == 1 ? integer(m.args[0]) : std::nullopt;
    if (!port || *port < 1 || *port > 65535) return err("unsubscribe needs the port it subscribed with");
    Peer to{peer.ip, uint16_t(*port)};
    if (leases_.unsubscribe(to)) {
        publish_dests();
        TTO_NOTICE("subscriber %s ended (it unsubscribed)", to.text().c_str());
        send_event("subscriber", to.text() + " ended");
    }
    send_to(to, {{addr("unsubscribed"), {int32_t(*port)}}});
    return {};
}

}

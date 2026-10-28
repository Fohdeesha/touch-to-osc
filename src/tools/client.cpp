#include "tools/tools.h"

#include "core/clock.h"
#include "core/fd.h"
#include "core/process.h"
#include "osc/osc.h"

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>

namespace tto {

namespace {

constexpr Nanos kFirstAnswer = 2 * kSec;
constexpr Nanos kQuietAfter = 300 * kMs;
constexpr Nanos kResendEvery = 250 * kMs;

struct Link {
    UniqueFd fd;
    sockaddr_in to{};
    std::string target;
    std::string prefix;

    bool open(const Config& cfg) {
        std::string host = cfg.osc.listen.host == "0.0.0.0" ? "127.0.0.1" : cfg.osc.listen.host;
        target = host + ":" + std::to_string(cfg.osc.listen.port);
        prefix = cfg.osc.prefix;
        to.sin_family = AF_INET;
        to.sin_port = htons(uint16_t(cfg.osc.listen.port));
        inet_pton(AF_INET, host.c_str(), &to.sin_addr);
        fd.reset(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        if (!fd) {
            std::fprintf(stderr, "cannot create a UDP socket: %s\n", errno_str().c_str());
            return false;
        }
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = host == "127.0.0.1" ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
        if (bind(fd.get(), reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
            std::fprintf(stderr, "cannot open a UDP port to listen for the answer: %s\n", errno_str().c_str());
            return false;
        }
        return true;
    }

    void send(const osc::Message& m) {
        auto bytes = osc::encode(m);
        sendto(fd.get(), bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    bool next(std::vector<osc::Message>& out, Nanos until) {
        uint8_t buf[65536];
        for (;;) {
            Nanos left = until - monotonic_ns();
            if (left <= 0) return false;
            pollfd p{fd.get(), POLLIN, 0};
            int r = poll(&p, 1, int((left + kMs - 1) / kMs));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0 || from.sin_addr.s_addr != to.sin_addr.s_addr) continue;
            auto msgs = osc::decode_packet(buf, size_t(n));
            if (!msgs) continue;
            out = std::move(*msgs);
            return true;
        }
    }

    void silent() const {
        std::fprintf(stderr,
                     "no answer from touch-to-osc on %s: is it running (systemctl status touch-to-osc), and is this machine "
                     "(127.0.0.1) in osc.allowed_ips?\n",
                     target.c_str());
    }
};

std::string text(const osc::Arg& a) {
    char b[48];
    if (auto i = std::get_if<int32_t>(&a)) return std::to_string(*i);
    if (auto h = std::get_if<int64_t>(&a)) return std::to_string(*h);
    if (auto f = std::get_if<float>(&a)) {
        std::snprintf(b, sizeof b, "%g", double(*f));
        return b;
    }
    if (auto d = std::get_if<double>(&a)) {
        std::snprintf(b, sizeof b, "%g", *d);
        return b;
    }
    return std::get<std::string>(a);
}

std::string joined(const std::vector<osc::Arg>& args, const char* between = ", ") {
    std::string s;
    for (const auto& a : args) s += (s.empty() ? "" : between) + text(a);
    return s;
}

osc::Arg word_arg(const std::string& w) {
    if (w.rfind("s:", 0) == 0) return w.substr(2);
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(w.c_str(), &end, 10);
    if (!w.empty() && *end == 0 && errno == 0 && v >= INT32_MIN && v <= INT32_MAX) return int32_t(v);
    double d = std::strtod(w.c_str(), &end);
    if (!w.empty() && *end == 0 && std::isfinite(d)) return float(d);
    return w;
}

}

int status_command(const Config& cfg) {
    Link link;
    if (!link.open(cfg)) return 6;
    link.send({cfg.osc.prefix + "/status/send", {}});
    std::map<std::string, std::vector<osc::Arg>> got;
    std::vector<osc::Message> msgs;
    Nanos until = monotonic_ns() + kFirstAnswer;
    while (link.next(msgs, until)) {
        for (auto& m : msgs) {
            if (m.address == cfg.osc.prefix + "/error") {
                std::fprintf(stderr, "touch-to-osc refused: %s\n", m.args.size() == 2 ? text(m.args[1]).c_str() : joined(m.args).c_str());
                return 6;
            }
            if (m.address.rfind(cfg.osc.prefix + "/", 0) == 0) got[m.address.substr(cfg.osc.prefix.size() + 1)] = std::move(m.args);
        }
        until = monotonic_ns() + kQuietAfter;
    }
    if (got.empty()) {
        link.silent();
        return 6;
    }
    auto one = [&](const std::string& k) { return got.count(k) ? joined(got[k]) : std::string("?"); };
    std::printf("touch-to-osc %s (osc_api %s), run %s, on %s, config %s\n", one("version").c_str(), one("osc_api").c_str(), one("run").c_str(), one("host").c_str(),
                one("config").c_str());
    std::printf("health: %s\n", one("health").c_str());
    for (const auto& r : got["reasons"]) std::printf("  - %s\n", text(r).c_str());
    if (!got["overrides"].empty())
        std::printf("saved over OSC (these win over the config file): %s\n", joined(got["overrides"]).c_str());
    std::printf("commands and subscriptions from: %s\n", one("allowed_ips").c_str());
    if (!got["subscribers"].empty()) std::printf("subscribers: %s\n", joined(got["subscribers"]).c_str());
    for (const auto& s : got["unused_screens"]) std::printf("a touchscreen no device reads (port, usb id, name): %s\n", text(s).c_str());
    for (const auto& id : got["devices"]) {
        std::string b = "device/" + text(id) + "/";
        auto dev = [&](const char* k) { return one(b + k); };
        std::printf("\n%s (%s): %s%s\n", text(id).c_str(), dev("label").c_str(), dev("state").c_str(), dev("enabled") == "0" ? ", turned off" : "");
        std::string reason = dev("reason");
        if (!reason.empty() && reason != "?") std::printf("  %s\n", reason.c_str());
        if (dev("connected") == "1")
            std::printf("  reading %s (usb %s) on port %s, %s contact%s down\n", dev("name").c_str(), dev("usb").c_str(), dev("port").c_str(),
                        dev("contacts").c_str(), dev("contacts") == "1" ? "" : "s");
        std::printf("  matches usb \"%s\", serial \"%s\", port \"%s\"; rotation %s, mirror \"%s\", silence %s ms\n", dev("match_usb").c_str(),
                    dev("match_serial").c_str(), dev("match_port").c_str(), dev("rotation").c_str(), dev("mirror").c_str(), dev("silence_ms").c_str());
    }
    return 0;
}

int send_command(const Config& cfg, const std::vector<std::string>& words) {
    if (words.empty()) {
        std::fprintf(stderr, "--send needs an address, such as: touch-to-osc --send device/screen/rotation 90\n");
        return 1;
    }
    Link link;
    if (!link.open(cfg)) return 6;
    std::string address = words[0].rfind("/", 0) == 0 ? words[0] : cfg.osc.prefix + "/" + words[0];
    int32_t seq = 0;
    while (seq == 0) {
        if (getrandom(&seq, sizeof seq, 0) != sizeof seq) seq = int32_t(monotonic_ns() | 1);
    }
    osc::Message req{cfg.osc.prefix + "/request", {seq, address}};
    for (size_t i = 1; i < words.size(); ++i) req.args.push_back(word_arg(words[i]));
    Nanos end = monotonic_ns() + kFirstAnswer;
    std::vector<osc::Message> msgs;
    while (monotonic_ns() < end) {
        link.send(req);
        Nanos until = std::min(end, monotonic_ns() + kResendEvery);
        while (link.next(msgs, until)) {
            for (const auto& m : msgs) {
                if (m.address == cfg.osc.prefix + "/error") {
                    std::fprintf(stderr, "refused: %s\n", m.args.size() == 2 ? text(m.args[1]).c_str() : joined(m.args).c_str());
                    return 6;
                }
                if (m.address != cfg.osc.prefix + "/reply" || m.args.size() != 3 || text(m.args[0]) != std::to_string(seq)) continue;
                if (text(m.args[1]) == "ok") {
                    std::string note = text(m.args[2]);
                    std::printf("ok%s%s\n", note.empty() ? "" : ": ", note.c_str());
                    return 0;
                }
                std::fprintf(stderr, "refused: %s\n", text(m.args[2]).c_str());
                return 6;
            }
        }
    }
    link.silent();
    return 6;
}

}

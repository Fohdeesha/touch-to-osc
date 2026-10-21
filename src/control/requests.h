#pragma once

#include "control/shared.h"
#include "core/clock.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace tto {

struct Peer {
    std::string ip;
    uint16_t port = 0;

    std::string text() const { return ip + ":" + std::to_string(port); }
    bool operator==(const Peer& o) const { return ip == o.ip && port == o.port; }
};

struct Lease {
    Peer peer;
    Nanos until = 0;
};

class Leases {
public:
    enum class Outcome { renewed, added, full };

    Outcome subscribe(const Peer& to, int seconds, Nanos now);
    bool unsubscribe(const Peer& to);
    std::vector<Peer> expire(Nanos now);
    const std::vector<Lease>& all() const { return leases_; }

private:
    std::vector<Lease> leases_;
};

class KeptReplies {
public:
    struct Kept {
        Peer peer;
        int32_t seq = 0;
        std::vector<uint8_t> command;
        bool ok = false;
        std::string note;
        Nanos at = 0;
    };
    enum class Lookup { none, repeat, conflict };

    Lookup find(const Peer& p, int32_t seq, const std::vector<uint8_t>& command, Nanos now, const Kept** out);
    void keep(const Peer& p, int32_t seq, std::vector<uint8_t> command, bool ok, std::string note, Nanos now);
    size_t size() const { return kept_.size(); }

private:
    void expire(Nanos now);
    std::deque<Kept> kept_;
};

constexpr Nanos kKeepReplies = 30 * kSec;
constexpr size_t kKeptReplies = 64;

}

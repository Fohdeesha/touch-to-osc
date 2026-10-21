#include "control/requests.h"

#include <algorithm>

namespace tto {

Leases::Outcome Leases::subscribe(const Peer& to, int seconds, Nanos now) {
    for (auto& l : leases_) {
        if (l.peer == to) {
            l.until = now + Nanos(seconds) * kSec;
            return Outcome::renewed;
        }
    }
    if (leases_.size() >= kMaxLeases) return Outcome::full;
    leases_.push_back(Lease{to, now + Nanos(seconds) * kSec});
    return Outcome::added;
}

bool Leases::unsubscribe(const Peer& to) {
    auto it = std::find_if(leases_.begin(), leases_.end(), [&](const Lease& l) { return l.peer == to; });
    if (it == leases_.end()) return false;
    leases_.erase(it);
    return true;
}

std::vector<Peer> Leases::expire(Nanos now) {
    std::vector<Peer> ended;
    for (auto it = leases_.begin(); it != leases_.end();) {
        if (it->until > now) {
            ++it;
            continue;
        }
        ended.push_back(it->peer);
        it = leases_.erase(it);
    }
    return ended;
}

void KeptReplies::expire(Nanos now) {
    while (!kept_.empty() && now - kept_.front().at > kKeepReplies) kept_.pop_front();
}

KeptReplies::Lookup KeptReplies::find(const Peer& p, int32_t seq, const std::vector<uint8_t>& command, Nanos now, const Kept** out) {
    expire(now);
    for (const auto& k : kept_) {
        if (k.seq != seq || !(k.peer == p)) continue;
        if (out) *out = &k;
        return k.command == command ? Lookup::repeat : Lookup::conflict;
    }
    return Lookup::none;
}

void KeptReplies::keep(const Peer& p, int32_t seq, std::vector<uint8_t> command, bool ok, std::string note, Nanos now) {
    expire(now);
    kept_.push_back(Kept{p, seq, std::move(command), ok, std::move(note), now});
    while (kept_.size() > kKeptReplies) kept_.pop_front();
}

}

#include "control/requests.h"

#include <doctest.h>

using namespace tto;

TEST_CASE("leases: added, renewed, the ninth refused, expired") {
    Leases l;
    Nanos now = 1000 * kSec;
    for (int i = 0; i < 8; ++i) CHECK(l.subscribe(Peer{"10.0.0.1", uint16_t(2000 + i)}, 10, now) == Leases::Outcome::added);
    CHECK(l.subscribe(Peer{"10.0.0.1", 2000}, 10, now + 5 * kSec) == Leases::Outcome::renewed);
    CHECK(l.subscribe(Peer{"10.0.0.2", 2000}, 10, now) == Leases::Outcome::full);
    auto ended = l.expire(now + 11 * kSec);
    CHECK(ended.size() == 7);
    REQUIRE(l.all().size() == 1);
    CHECK(l.all()[0].peer.port == 2000);
    CHECK(l.unsubscribe(Peer{"10.0.0.1", 2000}));
    CHECK(l.all().empty());
}

TEST_CASE("kept replies: an exact repeat is found, another command with the same seq conflicts, both expire") {
    KeptReplies k;
    Peer p{"10.0.0.1", 4000};
    std::vector<uint8_t> a = {1, 2, 3}, b = {4, 5, 6};
    Nanos now = 50 * kSec;
    CHECK(k.find(p, 7, a, now, nullptr) == KeptReplies::Lookup::none);
    k.keep(p, 7, a, true, "", now);
    const KeptReplies::Kept* kept = nullptr;
    CHECK(k.find(p, 7, a, now + kSec, &kept) == KeptReplies::Lookup::repeat);
    REQUIRE(kept);
    CHECK(kept->ok);
    CHECK(k.find(p, 7, b, now + kSec, nullptr) == KeptReplies::Lookup::conflict);
    CHECK(k.find(Peer{"10.0.0.1", 4001}, 7, a, now + kSec, nullptr) == KeptReplies::Lookup::none);
    CHECK(k.find(p, 7, a, now + 31 * kSec, nullptr) == KeptReplies::Lookup::none);
}

TEST_CASE("kept replies keep at most 64") {
    KeptReplies k;
    Peer p{"10.0.0.1", 4000};
    for (int i = 0; i < 70; ++i) k.keep(p, i, {uint8_t(i)}, true, "", kSec);
    CHECK(k.size() == 64);
    CHECK(k.find(p, 0, {0}, kSec, nullptr) == KeptReplies::Lookup::none);
    CHECK(k.find(p, 69, {69}, kSec, nullptr) == KeptReplies::Lookup::repeat);
}

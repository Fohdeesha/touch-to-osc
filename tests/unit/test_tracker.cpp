#include "input/tracker.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace tto;

namespace {

struct Frame {
    std::vector<TouchEvent> events;
};

std::vector<TouchEvent> run(ContactTracker& t, Nanos now = 1) {
    TouchEvent out[kMaxFrameEvents];
    int n = t.end_frame(out, now);
    return std::vector<TouchEvent>(out, out + n);
}

std::vector<TouchEvent> cancel(ContactTracker& t, const char* reason) {
    TouchEvent out[kMaxFrameEvents];
    int n = t.cancel_all(reason, out);
    return std::vector<TouchEvent>(out, out + n);
}

void touch(ContactTracker& t, int slot, int tracking, int x, int y) {
    t.raw(slot).tracking = tracking;
    t.raw(slot).x = x;
    t.raw(slot).y = y;
}

void lift(ContactTracker& t, int slot) { t.raw(slot).tracking = -1; }

}

TEST_CASE("a tap is one down and one up with the same contact") {
    ContactTracker t(10);
    touch(t, 0, 5, 100, 200);
    auto a = run(t);
    REQUIRE(a.size() == 1);
    CHECK(a[0].phase == Phase::down);
    CHECK(a[0].contact == 1);
    lift(t, 0);
    auto b = run(t);
    REQUIRE(b.size() == 1);
    CHECK(b[0].phase == Phase::up);
    CHECK(b[0].contact == 1);
    CHECK(b[0].x == 100);
    CHECK(t.announced() == 0);
}

TEST_CASE("a still finger sends nothing; a moved one sends a move") {
    ContactTracker t(10);
    touch(t, 0, 5, 100, 200);
    run(t);
    CHECK(run(t).empty());
    touch(t, 0, 5, 101, 200);
    auto m = run(t);
    REQUIRE(m.size() == 1);
    CHECK(m[0].phase == Phase::move);
    CHECK(m[0].x == 101);
}

TEST_CASE("contact ids are never reused, even when the kernel reuses slots and tracking ids") {
    ContactTracker t(10);
    touch(t, 0, 7, 1, 1);
    run(t);
    lift(t, 0);
    run(t);
    touch(t, 0, 7, 1, 1);
    auto a = run(t);
    REQUIRE(a.size() == 1);
    CHECK(a[0].contact == 2);
}

TEST_CASE("a new tracking id in the same frame ends the old contact and starts a new one") {
    ContactTracker t(10);
    touch(t, 0, 7, 10, 10);
    run(t);
    touch(t, 0, 8, 50, 50);
    auto a = run(t);
    REQUIRE(a.size() == 2);
    CHECK(a[0].phase == Phase::up);
    CHECK(a[0].contact == 1);
    CHECK(a[0].x == 10);
    CHECK(a[1].phase == Phase::down);
    CHECK(a[1].contact == 2);
    CHECK(a[1].x == 50);
}

TEST_CASE("ten fingers in one frame are ten downs") {
    ContactTracker t(10);
    for (int s = 0; s < 10; ++s) touch(t, s, 100 + s, s * 10, s * 10);
    auto a = run(t);
    CHECK(a.size() == 10);
    for (const auto& e : a) CHECK(e.phase == Phase::down);
    CHECK(t.announced() == 10);
}

TEST_CASE("cancel_all ends every contact once and mutes fingers still down until they lift") {
    ContactTracker t(10);
    touch(t, 0, 1, 10, 10);
    touch(t, 1, 2, 20, 20);
    run(t);
    lift(t, 1);
    auto c = cancel(t, "resync");
    REQUIRE(c.size() == 2);
    CHECK(c[0].phase == Phase::cancel);
    CHECK(std::string(c[0].reason) == "resync");
    CHECK(t.announced() == 0);
    CHECK(cancel(t, "again").empty());
    touch(t, 0, 1, 11, 11);
    CHECK(run(t).empty());
    lift(t, 0);
    CHECK(run(t).empty());
    touch(t, 0, 3, 12, 12);
    auto d = run(t);
    REQUIRE(d.size() == 1);
    CHECK(d[0].phase == Phase::down);
}

TEST_CASE("a muted slot that gets a new tracking id without lifting starts a new contact") {
    ContactTracker t(2);
    touch(t, 0, 1, 10, 10);
    run(t);
    cancel(t, "device silent");
    touch(t, 0, 9, 30, 30);
    auto a = run(t);
    REQUIRE(a.size() == 1);
    CHECK(a[0].phase == Phase::down);
}

TEST_CASE("fingers already down when reading starts are never announced") {
    ContactTracker t(4);
    touch(t, 2, 40, 5, 5);
    t.mute_down_slots();
    touch(t, 2, 40, 6, 6);
    CHECK(run(t).empty());
    lift(t, 2);
    CHECK(run(t).empty());
}

TEST_CASE("a palm is cancelled and stays muted") {
    ContactTracker t(2);
    touch(t, 0, 1, 10, 10);
    run(t);
    t.raw(0).palm = true;
    auto a = run(t);
    REQUIRE(a.size() == 1);
    CHECK(a[0].phase == Phase::cancel);
    CHECK(std::string(a[0].reason) == "palm");
    t.raw(0).palm = false;
    touch(t, 0, 1, 12, 12);
    CHECK(run(t).empty());
}

TEST_CASE("the active list and the oldest contact follow the announced contacts") {
    ContactTracker t(4);
    touch(t, 1, 1, 0, 0);
    run(t, 100);
    touch(t, 3, 2, 0, 0);
    run(t, 200);
    int32_t ids[kMaxSlots];
    REQUIRE(t.active_ids(ids) == 2);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
    CHECK(t.oldest_down() == 100);
    lift(t, 1);
    run(t, 300);
    CHECK(t.oldest_down() == 200);
}

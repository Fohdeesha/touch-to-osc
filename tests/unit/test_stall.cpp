#include "control/stall.h"

#include <doctest.h>

using namespace tto;

TEST_CASE("a reader is overdue only with contacts down, silence on, and no frame for longer than silence plus a second") {
    const Nanos now = 100 * kSec;
    CHECK_FALSE(reader_overdue(200, 0, now - 10 * kSec, now));
    CHECK_FALSE(reader_overdue(0, 2, now - 10 * kSec, now));
    CHECK_FALSE(reader_overdue(200, 2, 0, now));
    CHECK_FALSE(reader_overdue(200, 2, now - 1200 * kMs, now));
    CHECK(reader_overdue(200, 2, now - 1201 * kMs, now));
    CHECK_FALSE(reader_overdue(10000, 1, now - 11 * kSec, now));
    CHECK(reader_overdue(10000, 1, now - 11 * kSec - 1, now));
    CHECK_FALSE(reader_overdue(200, 1, now + kSec, now));
}

TEST_CASE("a stall counts after three looks in a row, and one good look starts the count over") {
    StallWatch w;
    const Nanos s = kSec;
    CHECK_FALSE(w.look(true, 10 * s));
    CHECK_FALSE(w.look(true, 11 * s));
    CHECK_FALSE(w.look(false, 12 * s));
    CHECK_FALSE(w.look(true, 13 * s));
    CHECK_FALSE(w.look(true, 14 * s));
    CHECK(w.look(true, 15 * s));
    CHECK(w.look(true, 16 * s));
    CHECK_FALSE(w.look(false, 17 * s));
    CHECK(kReaderStallSeconds == 3);
}

TEST_CASE("looks that come in a burst, as when a timer catches up after the process was stopped, count as one") {
    StallWatch w;
    const Nanos s = kSec;
    CHECK_FALSE(w.look(true, 20 * s));
    for (int k = 1; k <= 8; ++k) CHECK_FALSE(w.look(true, 20 * s + k * 100 * kMs));
    CHECK_FALSE(w.look(true, 21 * s));
    CHECK_FALSE(w.look(true, 21 * s + 100 * kMs));
    CHECK(w.look(true, 22 * s));
}

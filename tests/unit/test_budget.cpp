#include "core/budget.h"
#include "core/start_guard.h"

#include <doctest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace tto;

namespace {

int lines_in(Budget& b, Nanos from, Nanos to, Nanos step) {
    int n = 0;
    for (Nanos t = from; t < to; t += step) n += b.take(t) ? 1 : 0;
    return n;
}

std::string temp_dir() {
    char name[] = "/tmp/tto-guard-XXXXXX";
    REQUIRE(mkdtemp(name) != nullptr);
    return name;
}

std::string text_of(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}

TEST_CASE("a line that keeps coming is logged in a burst, then slowly") {
    Budget b(0, 60, 60 * kSec);
    Nanos t0 = 5 * kSec;
    CHECK(lines_in(b, t0, t0 + kSec, 10 * kMs) == 60);
    CHECK(b.take_skipped() == 40);
    // an hour of one a second: one a minute gets through
    int hour = lines_in(b, t0 + kSec, t0 + 3601 * kSec, kSec);
    CHECK(hour >= 59);
    CHECK(hour <= 61);
    CHECK(b.take_skipped() == uint64_t(3600 - hour));
}

TEST_CASE("a quiet hour gives the burst back") {
    Budget b(0, 60, 60 * kSec);
    Nanos t = kSec;
    CHECK(lines_in(b, t, t + kSec, kMs) == 60);
    t += 3700 * kSec;
    CHECK(lines_in(b, t, t + kSec, kMs) == 60);
}

TEST_CASE("lines are never closer than the gap") {
    Budget b(5 * kSec, 12, 60 * kSec);
    Nanos t0 = kSec;
    CHECK(b.take(t0));
    CHECK_FALSE(b.take(t0 + 4999 * kMs));
    CHECK(b.take(t0 + 5 * kSec));
    CHECK(b.take_skipped() == 1);
    // a flood for the first minute: one every 5 s, 12 in all, then one a minute
    Budget c(5 * kSec, 12, 60 * kSec);
    CHECK(lines_in(c, t0, t0 + 60 * kSec, 100 * kMs) == 12);
    int later = lines_in(c, t0 + 60 * kSec, t0 + 660 * kSec, 100 * kMs);
    CHECK(later >= 10);
    CHECK(later <= 11);
}

TEST_CASE("what was left out is counted once") {
    Budget b(0, 1, 60 * kSec);
    CHECK(b.take(kSec));
    CHECK_FALSE(b.take(2 * kSec));
    CHECK_FALSE(b.take(3 * kSec));
    CHECK(b.take_skipped() == 2);
    CHECK(b.take_skipped() == 0);
}

TEST_CASE("a start waits longer the more runs before it ended badly") {
    CHECK(start_hold(0) == 0);
    CHECK(start_hold(1) == 0);
    CHECK(start_hold(2) == 2 * kSec);
    CHECK(start_hold(3) == 4 * kSec);
    CHECK(start_hold(4) == 8 * kSec);
    CHECK(start_hold(5) == 16 * kSec);
    CHECK(start_hold(6) == 32 * kSec);
    CHECK(start_hold(7) == 60 * kSec);
    CHECK(start_hold(500) == 60 * kSec);
}

TEST_CASE("starts that did not last are counted in the runtime folder") {
    std::string dir = temp_dir();
    StartGuard g(dir);
    REQUIRE(g.on());
    Nanos t = 1000 * kSec;
    CHECK(g.begin(t) == 0);
    CHECK(text_of(g.file()) == "1 1000\n");
    CHECK(g.begin(t + 2 * kSec) == 1);
    CHECK(g.begin(t + 5 * kSec) == 2);
    CHECK(text_of(g.file()) == "3 1005\n");
    SUBCASE("a run that lasted, or ended on purpose, starts the count again") {
        g.clear();
        CHECK(access(g.file().c_str(), F_OK) != 0);
        CHECK(g.begin(t + 6 * kSec) == 0);
    }
    SUBCASE("a count from long ago is not held against this start") {
        CHECK(g.begin(t + 5 * kSec + kStartMemory + kSec) == 0);
        CHECK(g.begin(t + 5 * kSec + kStartMemory + 2 * kSec) == 1);
    }
    SUBCASE("a count from after now (another boot's clock) is not believed") {
        CHECK(g.begin(t - 100 * kSec) == 0);
    }
    SUBCASE("the count stops at a thousand") {
        for (int i = 0; i < 1100; ++i) g.begin(t + 6 * kSec);
        CHECK(g.begin(t + 6 * kSec) == 1000);
        CHECK(text_of(g.file()) == "1000 1006\n");
    }
    SUBCASE("a file that is not a count is taken for none") {
        std::ofstream(g.file()) << "garbage";
        CHECK(g.begin(t + 6 * kSec) == 0);
        std::ofstream(g.file()) << "-4 1006";
        CHECK(g.begin(t + 7 * kSec) == 0);
        std::ofstream(g.file()) << "2000000 1006";
        CHECK(g.begin(t + 7 * kSec) == 0);
    }
    g.clear();
    rmdir(dir.c_str());
}

TEST_CASE("without a runtime folder nothing is counted") {
    StartGuard off("");
    CHECK_FALSE(off.on());
    CHECK(off.begin(1000 * kSec) == 0);
    CHECK(off.begin(1001 * kSec) == 0);
    off.clear();
    StartGuard relative("run/here");
    CHECK_FALSE(relative.on());
    setenv("RUNTIME_DIRECTORY", "/run/one:/run/two", 1);
    CHECK(runtime_dir_from_env() == "/run/one");
    unsetenv("RUNTIME_DIRECTORY");
    CHECK(runtime_dir_from_env().empty());
}

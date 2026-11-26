#pragma once

#include "core/clock.h"

#include <string>

namespace tto {

constexpr Nanos kStartLasted = 60 * kSec;
constexpr Nanos kStartMemory = 600 * kSec;

// How long a start waits when the `short_runs` runs before it each ended badly within kStartLasted of starting:
// nothing after one (a single crash costs no time), then 2, 4, 8, 16, 32 and 60 s.
Nanos start_hold(int short_runs);

// Remembers, in a file of the service's runtime folder (memory, not disk), that a run started; the file goes when the
// run has lasted or ends on purpose. A service that cannot start, or dies soon after, is started again by systemd
// every second for ever: without this its journal grows by several lines a second.
class StartGuard {
public:
    explicit StartGuard(std::string dir);
    bool on() const { return !file_.empty(); }
    int begin(Nanos boot_now);
    void clear();
    const std::string& file() const { return file_; }

private:
    std::string file_;
};

Nanos boottime_ns();
std::string runtime_dir_from_env();

}

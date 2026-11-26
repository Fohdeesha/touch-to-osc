#pragma once

#include "core/clock.h"

#include <cstdint>

namespace tto {

constexpr int kReaderStallSeconds = 3;
constexpr Nanos kReaderStallGrace = kSec;

// a reader that holds contacts while its screen has sent nothing for longer than silence_ms should have cancelled them
inline bool reader_overdue(int32_t silence_ms, int32_t contacts, Nanos last_frame, Nanos now) {
    return silence_ms > 0 && contacts > 0 && last_frame > 0 && now - last_frame > Nanos(silence_ms) * kMs + kReaderStallGrace;
}

// a stall counts only when looks a second apart saw it kReaderStallSeconds times in a row, so a process that was
// itself stopped for a while (SIGSTOP, a suspended machine) is not taken for a stuck reader when it wakes and its
// timer catches up
class StallWatch {
public:
    bool look(bool overdue, Nanos now) {
        if (!overdue) {
            seen_ = 0;
            return false;
        }
        if (seen_ == 0 || now - last_ >= kSec - 100 * kMs) {
            ++seen_;
            last_ = now;
        }
        return seen_ >= kReaderStallSeconds;
    }

private:
    int seen_ = 0;
    Nanos last_ = 0;
};

}

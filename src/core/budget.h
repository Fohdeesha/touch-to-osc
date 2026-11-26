#pragma once

#include "core/clock.h"

#include <algorithm>
#include <cstdint>

namespace tto {

// For a log line whose cause can last for days: the first `burst` go out as they come (never closer than `gap`),
// after that one every `slow`. What was left out is counted, so the next line that goes out can say so.
class Budget {
public:
    constexpr Budget(Nanos gap, int burst, Nanos slow) : gap_(gap), slow_(slow), burst_(double(burst)) {}

    bool take(Nanos now) {
        if (filled_) tokens_ = std::min(burst_, tokens_ + double(now - filled_at_) / double(slow_));
        else tokens_ = burst_;
        filled_ = true;
        filled_at_ = now;
        if ((sent_ && now - sent_at_ < gap_) || tokens_ < 1.0) {
            ++skipped_;
            return false;
        }
        tokens_ -= 1.0;
        sent_ = true;
        sent_at_ = now;
        return true;
    }

    uint64_t take_skipped() {
        uint64_t n = skipped_;
        skipped_ = 0;
        return n;
    }

private:
    Nanos gap_;
    Nanos slow_;
    double burst_;
    double tokens_ = 0;
    Nanos filled_at_ = 0;
    Nanos sent_at_ = 0;
    uint64_t skipped_ = 0;
    bool filled_ = false;
    bool sent_ = false;
};

}

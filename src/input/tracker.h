#pragma once

#include "core/clock.h"

#include <cstdint>

namespace tto {

constexpr int kMaxSlots = 64;
constexpr int kMaxFrameEvents = 2 * kMaxSlots;

enum class Phase : uint8_t { down, move, up, cancel };

struct TouchEvent {
    Phase phase;
    int32_t contact;
    int32_t x;
    int32_t y;
    const char* reason;
};

struct RawSlot {
    int32_t tracking = -1;
    int32_t x = 0;
    int32_t y = 0;
    bool palm = false;
};

class ContactTracker {
public:
    explicit ContactTracker(int slots = 1) { reset(slots); }

    void reset(int slots);
    int slots() const { return n_; }
    RawSlot& raw(int slot) { return raw_[slot]; }

    int end_frame(TouchEvent* out, Nanos now);
    int cancel_all(const char* reason, TouchEvent* out);
    void mute_down_slots();

    int announced() const;
    Nanos oldest_down() const;
    int active_ids(int32_t* out) const;

private:
    struct Slot {
        int32_t contact = 0;
        int32_t tracking = -1;
        int32_t x = 0;
        int32_t y = 0;
        Nanos down_at = 0;
        bool muted = false;
        int32_t muted_tracking = -1;
    };
    void mute(Slot& s, int32_t tracking) {
        s.muted = true;
        s.muted_tracking = tracking;
    }

    RawSlot raw_[kMaxSlots];
    Slot st_[kMaxSlots];
    int n_ = 1;
    int32_t next_contact_ = 1;
};

}

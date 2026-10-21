#include "input/tracker.h"

#include <algorithm>

namespace tto {

void ContactTracker::reset(int slots) {
    n_ = std::clamp(slots, 1, kMaxSlots);
    for (auto& r : raw_) r = RawSlot{};
    for (auto& s : st_) s = Slot{};
}

int ContactTracker::end_frame(TouchEvent* out, Nanos now) {
    int n = 0;
    for (int i = 0; i < n_; ++i) {
        const RawSlot& r = raw_[i];
        Slot& s = st_[i];
        if (s.muted) {
            if (r.tracking == -1) {
                s.muted = false;
                s.muted_tracking = -1;
                continue;
            }
            if (r.tracking == s.muted_tracking) continue;
            s.muted = false;
        }
        if (s.contact) {
            if (r.tracking == -1) {
                out[n++] = TouchEvent{Phase::up, s.contact, r.x, r.y, nullptr};
                s.contact = 0;
                continue;
            }
            if (r.tracking != s.tracking) {
                out[n++] = TouchEvent{Phase::up, s.contact, s.x, s.y, nullptr};
                s.contact = 0;
            } else if (r.palm) {
                out[n++] = TouchEvent{Phase::cancel, s.contact, s.x, s.y, "palm"};
                s.contact = 0;
                mute(s, r.tracking);
                continue;
            } else {
                if (r.x != s.x || r.y != s.y) {
                    s.x = r.x;
                    s.y = r.y;
                    out[n++] = TouchEvent{Phase::move, s.contact, r.x, r.y, nullptr};
                }
                continue;
            }
        }
        if (r.tracking == -1) continue;
        if (r.palm) {
            mute(s, r.tracking);
            continue;
        }
        s.contact = next_contact_++;
        s.tracking = r.tracking;
        s.x = r.x;
        s.y = r.y;
        s.down_at = now;
        out[n++] = TouchEvent{Phase::down, s.contact, r.x, r.y, nullptr};
    }
    return n;
}

int ContactTracker::cancel_all(const char* reason, TouchEvent* out) {
    int n = 0;
    for (int i = 0; i < n_; ++i) {
        Slot& s = st_[i];
        if (s.contact) {
            out[n++] = TouchEvent{Phase::cancel, s.contact, s.x, s.y, reason};
            s.contact = 0;
        }
    }
    mute_down_slots();
    return n;
}

void ContactTracker::mute_down_slots() {
    for (int i = 0; i < n_; ++i) {
        Slot& s = st_[i];
        if (s.contact) continue;
        if (raw_[i].tracking != -1) mute(s, raw_[i].tracking);
        else s.muted = false;
    }
}

int ContactTracker::announced() const {
    int n = 0;
    for (int i = 0; i < n_; ++i) n += st_[i].contact ? 1 : 0;
    return n;
}

Nanos ContactTracker::oldest_down() const {
    Nanos oldest = 0;
    for (int i = 0; i < n_; ++i) {
        if (st_[i].contact && (oldest == 0 || st_[i].down_at < oldest)) oldest = st_[i].down_at;
    }
    return oldest;
}

int ContactTracker::active_ids(int32_t* out) const {
    int n = 0;
    for (int i = 0; i < n_; ++i) {
        if (st_[i].contact) out[n++] = st_[i].contact;
    }
    return n;
}

}

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace tto {

template <typename T>
class TripleBuffer {
public:
    T& back() { return bufs_[back_]; }

    void publish() { back_ = uint8_t(middle_.exchange(uint8_t(back_ | kFresh), std::memory_order_acq_rel) & kIndex); }

    const T& latest() {
        if (middle_.load(std::memory_order_acquire) & kFresh) front_ = uint8_t(middle_.exchange(front_, std::memory_order_acq_rel) & kIndex);
        return bufs_[front_];
    }

private:
    static constexpr uint8_t kFresh = 4;
    static constexpr uint8_t kIndex = 3;
    std::array<T, 3> bufs_{};
    std::atomic<uint8_t> middle_{1};
    uint8_t back_ = 0;
    uint8_t front_ = 2;
};

}

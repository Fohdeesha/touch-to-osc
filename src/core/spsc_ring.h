#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace tto {

template <typename T, size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "the capacity is a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "ring slots hold plain values");

public:
    bool push(const T& v) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == N) return false;
        slots_[head & (N - 1)] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = slots_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
    static constexpr size_t capacity() { return N; }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::array<T, N> slots_{};
};

}

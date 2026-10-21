#pragma once

#include <cstdint>
#include <ctime>

namespace tto {

using Nanos = int64_t;

constexpr Nanos kUs = 1'000;
constexpr Nanos kMs = 1'000'000;
constexpr Nanos kSec = 1'000'000'000;

inline Nanos monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return Nanos(ts.tv_sec) * kSec + ts.tv_nsec;
}

inline Nanos realtime_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return Nanos(ts.tv_sec) * kSec + ts.tv_nsec;
}

}

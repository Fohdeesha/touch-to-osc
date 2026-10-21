#pragma once

#include "core/clock.h"

#include <atomic>
#include <cstddef>

namespace tto::log {

enum class Level : int { crit = 2, err = 3, warning = 4, notice = 5, info = 6, debug = 7 };

void set_min_level(Level level);
Level min_level();
bool parse_level(const char* name, Level& out);

void write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

bool rate_ok(std::atomic<Nanos>& last, Nanos interval);

}

#define TTO_CRIT(...) ::tto::log::write(::tto::log::Level::crit, __VA_ARGS__)
#define TTO_ERR(...) ::tto::log::write(::tto::log::Level::err, __VA_ARGS__)
#define TTO_WARN(...) ::tto::log::write(::tto::log::Level::warning, __VA_ARGS__)
#define TTO_NOTICE(...) ::tto::log::write(::tto::log::Level::notice, __VA_ARGS__)
#define TTO_INFO(...) ::tto::log::write(::tto::log::Level::info, __VA_ARGS__)
#define TTO_DEBUG(...)                                                             \
    do {                                                                           \
        if (::tto::log::min_level() >= ::tto::log::Level::debug)                   \
            ::tto::log::write(::tto::log::Level::debug, __VA_ARGS__);              \
    } while (0)

#define TTO_LOG_EVERY(interval_ns, level, ...)                                     \
    do {                                                                           \
        static std::atomic<::tto::Nanos> tto_log_last_{0};                         \
        if (::tto::log::rate_ok(tto_log_last_, (interval_ns)))                     \
            ::tto::log::write((level), __VA_ARGS__);                               \
    } while (0)

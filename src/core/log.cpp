#include "core/log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace tto::log {

namespace {

std::atomic<int> g_min{int(Level::info)};
std::atomic<int> g_journal{-1};

bool to_journal() {
    int j = g_journal.load(std::memory_order_relaxed);
    if (j < 0) {
        j = std::getenv("JOURNAL_STREAM") ? 1 : 0;
        g_journal.store(j, std::memory_order_relaxed);
    }
    return j == 1;
}

const char* level_name(Level l) {
    switch (l) {
    case Level::crit: return "CRIT";
    case Level::err: return "ERROR";
    case Level::warning: return "WARN";
    case Level::notice: return "NOTICE";
    case Level::info: return "INFO";
    case Level::debug: return "DEBUG";
    }
    return "?";
}

}

void set_min_level(Level level) { g_min.store(int(level), std::memory_order_relaxed); }

Level min_level() { return Level(g_min.load(std::memory_order_relaxed)); }

bool parse_level(const char* name, Level& out) {
    struct { const char* n; Level l; } table[] = {
        {"crit", Level::crit}, {"error", Level::err}, {"warning", Level::warning},
        {"notice", Level::notice}, {"info", Level::info}, {"debug", Level::debug},
    };
    for (const auto& e : table) {
        if (std::strcmp(e.n, name) == 0) {
            out = e.l;
            return true;
        }
    }
    return false;
}

void write(Level level, const char* fmt, ...) {
    if (int(level) > g_min.load(std::memory_order_relaxed)) return;
    char buf[1536];
    int n = 0;
    if (to_journal()) {
        n = std::snprintf(buf, sizeof buf, "<%d>", int(level));
    } else {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        tm t{};
        localtime_r(&ts.tv_sec, &t);
        n = std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03ld %-6s ", t.tm_year + 1900, t.tm_mon + 1,
                          t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000, level_name(level));
    }
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, sizeof buf - size_t(n) - 1, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    size_t len = std::min(sizeof buf - 2, size_t(n) + size_t(m));
    for (size_t i = size_t(n); i < len; ++i) {
        if (buf[i] == '\n') buf[i] = ' ';
    }
    buf[len++] = '\n';
    ssize_t ignored = ::write(2, buf, len);
    (void)ignored;
}

bool rate_ok(std::atomic<Nanos>& last, Nanos interval) {
    Nanos now = monotonic_ns();
    Nanos prev = last.load(std::memory_order_relaxed);
    if (prev != 0 && now - prev < interval) return false;
    return last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}

}

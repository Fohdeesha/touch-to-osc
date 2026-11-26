#include "core/start_guard.h"

#include "core/fd.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace tto {

Nanos start_hold(int short_runs) {
    if (short_runs <= 1) return 0;
    if (short_runs >= 7) return 60 * kSec;
    return (Nanos(1) << (short_runs - 1)) * kSec;
}

StartGuard::StartGuard(std::string dir) {
    if (!dir.empty() && dir[0] == '/') file_ = dir + "/starts";
}

int StartGuard::begin(Nanos boot_now) {
    if (file_.empty()) return 0;
    int before = 0;
    {
        UniqueFd fd(open(file_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        char buf[64] = {};
        ssize_t n = fd ? read(fd.get(), buf, sizeof buf - 1) : -1;
        int count = 0;
        long long at_s = 0;
        if (n > 0 && std::sscanf(buf, "%d %lld", &count, &at_s) == 2 && count > 0 && count <= 1000000) {
            Nanos at = Nanos(at_s) * kSec;
            if (boot_now >= at && boot_now - at <= kStartMemory) before = count;
        }
    }
    char text[64];
    int len = std::snprintf(text, sizeof text, "%d %lld\n", std::min(before + 1, 1000), (long long)(boot_now / kSec));
    UniqueFd out(open(file_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644));
    if (out && len > 0) {
        ssize_t ignored = write(out.get(), text, size_t(len));
        (void)ignored;
    }
    return before;
}

void StartGuard::clear() {
    if (!file_.empty()) unlink(file_.c_str());
}

Nanos boottime_ns() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return Nanos(ts.tv_sec) * kSec + ts.tv_nsec;
}

std::string runtime_dir_from_env() {
    const char* v = std::getenv("RUNTIME_DIRECTORY");
    if (!v) return {};
    std::string s = v;
    return s.substr(0, s.find(':'));
}

}

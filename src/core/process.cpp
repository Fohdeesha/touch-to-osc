#include "core/process.h"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tto {

std::string errno_str(int e) {
    char buf[128] = {};
    // the GNU strerror_r, which returns the text and may leave buf alone
    return std::string(strerror_r(e, buf, sizeof buf));
}

std::string errno_str() { return errno_str(errno); }

sigset_t stop_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    // a blocked signal is delivered even when it is set to be ignored: what nohup or a shell had ignored stays out
    for (int sig : {SIGINT, SIGHUP, SIGQUIT}) {
        struct sigaction now {};
        if (sigaction(sig, nullptr, &now) == 0 && now.sa_handler == SIG_IGN) continue;
        sigaddset(&set, sig);
    }
    return set;
}

const char* stop_signal_name(int signo) {
    switch (signo) {
    case SIGINT: return "SIGINT";
    case SIGHUP: return "SIGHUP";
    case SIGQUIT: return "SIGQUIT";
    default: return "SIGTERM";
    }
}

void set_thread_name(const char* name) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", name);
    prctl(PR_SET_NAME, buf, 0, 0, 0);
}

void sd_notify_msg(const char* msg, bool may_wait) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t len = std::strlen(path);
    if (len >= sizeof addr.sun_path) return;
    std::memcpy(addr.sun_path, path, len);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    socklen_t alen = socklen_t(offsetof(sockaddr_un, sun_path) + len);
    ssize_t ignored = sendto(fd, msg, std::strlen(msg), MSG_NOSIGNAL | (may_wait ? 0 : MSG_DONTWAIT), reinterpret_cast<sockaddr*>(&addr), alen);
    (void)ignored;
    close(fd);
}

bool sd_watchdog_enabled() {
    const char* usec = std::getenv("WATCHDOG_USEC");
    return usec && std::atoll(usec) > 0;
}

std::string self_exe_path() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return buf;
}

}

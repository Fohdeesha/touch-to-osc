#include "core/process.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tto {

std::string errno_str(int e) { return std::strerror(e); }
std::string errno_str() { return std::strerror(errno); }

void set_thread_name(const char* name) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", name);
    prctl(PR_SET_NAME, buf, 0, 0, 0);
}

void sd_notify_msg(const char* msg) {
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
    ssize_t ignored = sendto(fd, msg, std::strlen(msg), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr), alen);
    (void)ignored;
    close(fd);
}

bool sd_watchdog_enabled() {
    const char* usec = std::getenv("WATCHDOG_USEC");
    return usec && std::atoll(usec) > 0;
}

}

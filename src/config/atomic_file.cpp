#include "config/atomic_file.h"

#include "core/fd.h"
#include "core/process.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tto {

namespace {

std::string dir_of(const std::string& path) {
    auto slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

Status write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return err("write: " + errno_str());
        }
        off += size_t(n);
    }
    return {};
}

}

Status write_file_atomic(const std::string& path, const std::string& contents) {
    std::string dir = dir_of(path);
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    UniqueFd fd(open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640));
    if (!fd) return err("cannot create " + tmp + ": " + errno_str());
    auto st = write_all(fd.get(), contents);
    if (!st) {
        unlink(tmp.c_str());
        return st;
    }
    if (fsync(fd.get()) != 0) {
        int e = errno;
        unlink(tmp.c_str());
        return err("fsync " + tmp + ": " + errno_str(e));
    }
    fd.reset();
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        int e = errno;
        unlink(tmp.c_str());
        return err("rename " + tmp + ": " + errno_str(e));
    }
    UniqueFd dfd(open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dfd) return err("open " + dir + ": " + errno_str());
    if (fsync(dfd.get()) != 0) return err("fsync " + dir + ": " + errno_str());
    return {};
}

Result<std::string> read_file(const std::string& path, size_t max_bytes) {
    UniqueFd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) return err(path + ": " + errno_str());
    std::string out;
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return err(path + ": " + errno_str());
        }
        if (n == 0) break;
        if (out.size() + size_t(n) > max_bytes) return err(path + ": larger than " + std::to_string(max_bytes) + " bytes");
        out.append(buf, size_t(n));
    }
    return out;
}

}

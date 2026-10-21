#pragma once

#include <unistd.h>
#include <utility>

namespace tto {

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) reset(std::exchange(o.fd_, -1));
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    explicit operator bool() const { return valid(); }
    int release() { return std::exchange(fd_, -1); }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}

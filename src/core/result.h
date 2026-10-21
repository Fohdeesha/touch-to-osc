#pragma once

#include <string>
#include <utility>
#include <variant>

namespace tto {

struct Error {
    std::string message;
};

inline Error err(std::string message) { return Error{std::move(message)}; }

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error e) : v_(std::move(e)) {}

    bool ok() const { return std::holds_alternative<T>(v_); }
    explicit operator bool() const { return ok(); }

    T& value() & { return std::get<T>(v_); }
    const T& value() const& { return std::get<T>(v_); }
    T&& value() && { return std::get<T>(std::move(v_)); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    const std::string& error() const { return std::get<Error>(v_).message; }

private:
    std::variant<T, Error> v_;
};

class [[nodiscard]] Status {
public:
    Status() = default;
    Status(Error e) : error_(std::move(e.message)), failed_(true) {}

    static Status success() { return Status(); }
    bool ok() const { return !failed_; }
    explicit operator bool() const { return ok(); }
    const std::string& error() const { return error_; }

private:
    std::string error_;
    bool failed_ = false;
};

}

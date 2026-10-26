#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tto::osc {

using Arg = std::variant<int32_t, float, std::string, int64_t, double>;

struct Message {
    std::string address;
    std::vector<Arg> args;
};

std::vector<uint8_t> encode(const Message& m);
std::vector<uint8_t> encode_bundle(const std::vector<Message>& msgs);
// set when a message is well formed but carries an argument of a type commands never take
struct DecodeIssue {
    std::string address;
    std::string what;
};

std::optional<Message> decode(const uint8_t* data, size_t len, DecodeIssue* issue = nullptr);
std::optional<std::vector<Message>> decode_packet(const uint8_t* data, size_t len, DecodeIssue* issue = nullptr);

bool valid_address(const std::string& a);

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    void begin_bundle();
    void begin_message(const char* address, const char* tags);
    void i(int32_t v);
    void f(float v);
    void h(int64_t v);
    void s(const char* v);
    void end_message();

    bool ok() const { return ok_; }
    size_t size() const { return pos_; }
    const uint8_t* data() const { return buf_; }
    int messages() const { return messages_; }

private:
    void put(const void* p, size_t n);
    void padded(const char* str);
    void be32(uint32_t v);

    uint8_t* buf_;
    size_t cap_;
    size_t pos_ = 0;
    size_t size_at_ = 0;
    bool in_bundle_ = false;
    bool ok_ = true;
    int messages_ = 0;
};

}

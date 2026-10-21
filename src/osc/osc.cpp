#include "osc/osc.h"

#include <cstring>

namespace tto::osc {

namespace {

constexpr size_t kMaxPacket = 65536;
constexpr int kMaxDepth = 4;
const char kBundleTag[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};

void put_padded(std::vector<uint8_t>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
    while (out.size() % 4) out.push_back(0);
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

bool get_padded(const uint8_t* d, size_t len, size_t& pos, std::string& out) {
    size_t start = pos;
    while (pos < len && d[pos] != 0) ++pos;
    if (pos >= len) return false;
    out.assign(reinterpret_cast<const char*>(d + start), pos - start);
    ++pos;
    while (pos % 4) {
        if (pos >= len || d[pos] != 0) return false;
        ++pos;
    }
    return true;
}

bool get_be32(const uint8_t* d, size_t len, size_t& pos, uint32_t& v) {
    if (pos > len || len - pos < 4) return false;
    v = (uint32_t(d[pos]) << 24) | (uint32_t(d[pos + 1]) << 16) | (uint32_t(d[pos + 2]) << 8) | uint32_t(d[pos + 3]);
    pos += 4;
    return true;
}

bool is_bundle(const uint8_t* d, size_t len) { return len >= 16 && std::memcmp(d, kBundleTag, 8) == 0; }

bool unpack(const uint8_t* d, size_t len, int depth, std::vector<Message>& out) {
    if (!is_bundle(d, len)) {
        auto m = decode(d, len);
        if (!m) return false;
        out.push_back(std::move(*m));
        return true;
    }
    if (depth >= kMaxDepth || len % 4) return false;
    size_t pos = 16;
    while (pos < len) {
        uint32_t size;
        if (!get_be32(d, len, pos, size)) return false;
        if (size == 0 || size % 4 || size > len - pos) return false;
        if (!unpack(d + pos, size, depth + 1, out)) return false;
        pos += size;
    }
    return true;
}

}

bool valid_address(const std::string& a) {
    if (a.empty() || a[0] != '/' || a.size() > 512) return false;
    for (unsigned char c : a) {
        if (c < 0x21 || c > 0x7e || c == '#' || c == '*' || c == '?' || c == '[' || c == ']' || c == '{' || c == '}' || c == ',') return false;
    }
    return true;
}

std::vector<uint8_t> encode(const Message& m) {
    std::vector<uint8_t> out;
    put_padded(out, m.address);
    std::string tags = ",";
    for (const auto& a : m.args) tags += std::holds_alternative<int32_t>(a) ? 'i' : std::holds_alternative<float>(a) ? 'f' : std::holds_alternative<int64_t>(a) ? 'h' : 's';
    put_padded(out, tags);
    for (const auto& a : m.args) {
        if (auto i = std::get_if<int32_t>(&a)) put_be32(out, uint32_t(*i));
        else if (auto f = std::get_if<float>(&a)) {
            uint32_t bits;
            std::memcpy(&bits, f, 4);
            put_be32(out, bits);
        } else if (auto h = std::get_if<int64_t>(&a)) {
            put_be32(out, uint32_t(uint64_t(*h) >> 32));
            put_be32(out, uint32_t(uint64_t(*h)));
        } else put_padded(out, std::get<std::string>(a));
    }
    return out;
}

std::vector<uint8_t> encode_bundle(const std::vector<Message>& msgs) {
    std::vector<uint8_t> out(kBundleTag, kBundleTag + 8);
    put_be32(out, 0);
    put_be32(out, 1);
    for (const auto& m : msgs) {
        auto bytes = encode(m);
        put_be32(out, uint32_t(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

std::optional<Message> decode(const uint8_t* d, size_t len) {
    if (len < 8 || len % 4 || len > kMaxPacket) return std::nullopt;
    size_t pos = 0;
    Message m;
    if (!get_padded(d, len, pos, m.address) || !valid_address(m.address)) return std::nullopt;
    std::string tags;
    if (pos == len) return std::nullopt;
    if (!get_padded(d, len, pos, tags) || tags.empty() || tags[0] != ',' || tags.size() > 64) return std::nullopt;
    for (size_t i = 1; i < tags.size(); ++i) {
        char t = tags[i];
        if (t == 'i') {
            uint32_t v;
            if (!get_be32(d, len, pos, v)) return std::nullopt;
            m.args.emplace_back(int32_t(v));
        } else if (t == 'f') {
            uint32_t v;
            if (!get_be32(d, len, pos, v)) return std::nullopt;
            float f;
            std::memcpy(&f, &v, 4);
            m.args.emplace_back(f);
        } else if (t == 'h') {
            uint32_t hi, lo;
            if (!get_be32(d, len, pos, hi) || !get_be32(d, len, pos, lo)) return std::nullopt;
            m.args.emplace_back(int64_t((uint64_t(hi) << 32) | lo));
        } else if (t == 's') {
            std::string s;
            if (!get_padded(d, len, pos, s) || s.size() > 1024) return std::nullopt;
            m.args.emplace_back(std::move(s));
        } else if (t == 'T') {
            m.args.emplace_back(int32_t(1));
        } else if (t == 'F') {
            m.args.emplace_back(int32_t(0));
        } else {
            return std::nullopt;
        }
    }
    if (pos != len) return std::nullopt;
    return m;
}

std::optional<std::vector<Message>> decode_packet(const uint8_t* d, size_t len) {
    if (len > kMaxPacket) return std::nullopt;
    std::vector<Message> out;
    if (!unpack(d, len, 0, out)) return std::nullopt;
    return out;
}

void Writer::put(const void* p, size_t n) {
    if (!ok_ || cap_ - pos_ < n) {
        ok_ = false;
        return;
    }
    std::memcpy(buf_ + pos_, p, n);
    pos_ += n;
}

void Writer::padded(const char* str) {
    size_t n = std::strlen(str);
    put(str, n);
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    put(zeros, 4 - (n % 4));
}

void Writer::be32(uint32_t v) {
    uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    put(b, 4);
}

void Writer::begin_bundle() {
    pos_ = 0;
    ok_ = true;
    messages_ = 0;
    in_bundle_ = true;
    put(kBundleTag, 8);
    be32(0);
    be32(1);
}

void Writer::begin_message(const char* address, const char* tags) {
    if (in_bundle_) {
        size_at_ = pos_;
        be32(0);
    } else {
        pos_ = 0;
        ok_ = true;
        messages_ = 0;
    }
    padded(address);
    char t[34];
    size_t n = std::strlen(tags);
    if (n > 32) {
        ok_ = false;
        return;
    }
    t[0] = ',';
    std::memcpy(t + 1, tags, n + 1);
    padded(t);
}

void Writer::i(int32_t v) { be32(uint32_t(v)); }

void Writer::f(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    be32(bits);
}

void Writer::h(int64_t v) {
    be32(uint32_t(uint64_t(v) >> 32));
    be32(uint32_t(uint64_t(v)));
}

void Writer::s(const char* v) { padded(v); }

void Writer::end_message() {
    if (!ok_) return;
    ++messages_;
    if (!in_bundle_) return;
    uint32_t size = uint32_t(pos_ - size_at_ - 4);
    uint8_t b[4] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
    std::memcpy(buf_ + size_at_, b, 4);
}

}

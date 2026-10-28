#include "osc/osc.h"

#include <doctest.h>

#include <cstring>

using namespace tto;

TEST_CASE("a message survives encode and decode") {
    osc::Message m{"/touch/screen/down", {int32_t(7), 0.5f, 0.25f, int32_t(12), int64_t(1792567761176835)}};
    auto bytes = osc::encode(m);
    auto back = osc::decode(bytes.data(), bytes.size());
    REQUIRE(back);
    CHECK(back->address == m.address);
    REQUIRE(back->args.size() == 5);
    CHECK(std::get<int32_t>(back->args[0]) == 7);
    CHECK(std::get<float>(back->args[1]) == 0.5f);
    CHECK(std::get<int64_t>(back->args[4]) == 1792567761176835);
}

TEST_CASE("the bundle writer produces what the decoder reads, without allocating") {
    uint8_t buf[512];
    osc::Writer w(buf, sizeof buf);
    w.begin_bundle();
    w.begin_message("/touch/screen/down", "iffih");
    w.i(1);
    w.f(0.1f);
    w.f(0.2f);
    w.i(3);
    w.h(4);
    w.end_message();
    w.begin_message("/touch/screen/cancel", "isih");
    w.i(2);
    w.s("resync");
    w.i(3);
    w.h(4);
    w.end_message();
    REQUIRE(w.ok());
    CHECK(w.messages() == 2);
    auto msgs = osc::decode_packet(w.data(), w.size());
    REQUIRE(msgs);
    REQUIRE(msgs->size() == 2);
    CHECK((*msgs)[0].address == "/touch/screen/down");
    CHECK(std::get<float>((*msgs)[0].args[2]) == 0.2f);
    CHECK((*msgs)[1].address == "/touch/screen/cancel");
    CHECK(std::get<std::string>((*msgs)[1].args[1]) == "resync");
}

TEST_CASE("the writer refuses to overrun its buffer") {
    uint8_t buf[40];
    osc::Writer w(buf, sizeof buf);
    w.begin_bundle();
    w.begin_message("/touch/a/very/long/address/indeed", "i");
    w.i(1);
    w.end_message();
    CHECK_FALSE(w.ok());
}

TEST_CASE("encode_bundle and decode_packet agree, and nested bundles unpack") {
    auto inner = osc::encode_bundle({{"/a", {int32_t(1)}}, {"/b", {std::string("x")}}});
    std::vector<uint8_t> outer = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    uint32_t n = uint32_t(inner.size());
    outer.push_back(uint8_t(n >> 24));
    outer.push_back(uint8_t(n >> 16));
    outer.push_back(uint8_t(n >> 8));
    outer.push_back(uint8_t(n));
    outer.insert(outer.end(), inner.begin(), inner.end());
    auto msgs = osc::decode_packet(outer.data(), outer.size());
    REQUIRE(msgs);
    REQUIRE(msgs->size() == 2);
    CHECK((*msgs)[1].address == "/b");
}

TEST_CASE("malformed packets are refused") {
    std::vector<uint8_t> junk = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 99};
    CHECK_FALSE(osc::decode_packet(junk.data(), junk.size()));
    std::vector<uint8_t> noaddr = {'x', 0, 0, 0, ',', 0, 0, 0};
    CHECK_FALSE(osc::decode_packet(noaddr.data(), noaddr.size()));
    auto good = osc::encode({"/touch/ping", {}});
    good.pop_back();
    CHECK_FALSE(osc::decode_packet(good.data(), good.size()));
}

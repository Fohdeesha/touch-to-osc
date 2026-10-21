#include "config/config.h"

#include <doctest.h>

#include <fstream>
#include <sstream>

using namespace tto;

namespace {

const char* kBase = R"(
[osc]
listen = "0.0.0.0:9480"
allowed_ips = ["127.0.0.1"]

[[device]]
id = "face"
usb = "0712:0009"
)";

std::string refused(const std::string& text) {
    auto c = parse_config(text, "t.toml");
    return c ? std::string() : c.error();
}

}

TEST_CASE("the minimal config loads with the documented defaults") {
    auto c = parse_config(kBase, "t.toml");
    REQUIRE(c);
    CHECK(c->osc.prefix == "/touch");
    REQUIRE(c->devices.size() == 1);
    const auto& d = c->devices[0];
    CHECK(d.vendor == 0x0712);
    CHECK(d.product == 0x0009);
    CHECK(d.label == "face");
    CHECK(d.grab);
    CHECK(d.silence_ms == 200);
    CHECK(d.enabled);
}

TEST_CASE("config.example.toml is valid") {
    std::ifstream in(std::string(TTO_SOURCE_DIR) + "/config.example.toml");
    REQUIRE(in);
    std::stringstream ss;
    ss << in.rdbuf();
    auto c = parse_config(ss.str(), "config.example.toml");
    INFO((c ? std::string() : c.error()));
    CHECK(c);
}

TEST_CASE("unknown keys, wrong types and out-of-range values are refused naming the key") {
    CHECK(refused(std::string(kBase) + "colour = \"red\"\n").find("device[1].colour: unknown key") != std::string::npos);
    CHECK(refused(std::string(kBase) + "rotation = 45\n").find("device[1].rotation") != std::string::npos);
    CHECK(refused(std::string(kBase) + "rotation = 300\n").find("device[1].rotation: must be 0, 90, 180 or 270") != std::string::npos);
    CHECK(refused(std::string(kBase) + "grab = 1\n").find("device[1].grab: must be true or false") != std::string::npos);
    CHECK(refused(std::string(kBase) + "silence_ms = 10\n").find("device[1].silence_ms") != std::string::npos);
    CHECK(refused(std::string(kBase) + "mirror = \"z\"\n").find("device[1].mirror") != std::string::npos);
    CHECK(refused(std::string(kBase) + "calibration = [1, 2, 3]\n").find("device[1].calibration") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\nusb = \"712:9\"\n").find("device[1].usb: must be vendor:product") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\nlisten = \"nowhere\"\n[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.listen") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"localhost\"]\n[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.allowed_ips") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\nprefix = \"/touch/\"\n[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.prefix") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"Face\"\nusb = \"0001:0002\"\n").find("device[1].id") != std::string::npos);
}

TEST_CASE("a config that cannot work is refused with what to do") {
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n").find("there is no [[device]]") != std::string::npos);
    CHECK(refused("[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.allowed_ips is empty") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\n").find("set usb (vendor:product) or port") != std::string::npos);
    CHECK(refused(std::string(kBase) + "[[device]]\nid = \"face\"\nport = \"3-3\"\n").find("two devices are called \"face\"") != std::string::npos);
    CHECK(refused(std::string(kBase) + "[[destination]]\naddress = \"10.0.0.1:9000\"\ntouches = false\nstatus = false\n").find("destination[1]: sends nothing") != std::string::npos);
    CHECK(refused("x = = 1").find("t.toml:1:") != std::string::npos);
}

TEST_CASE("destinations and mirrors parse") {
    auto c = parse_config(std::string(kBase) + "mirror = \"xy\"\nrotation = 270\ncalibration = [1, 0, 0, 0, 1, 0]\n[[destination]]\naddress = \"192.168.1.27:9481\"\nstatus = false\n", "t.toml");
    REQUIRE(c);
    CHECK(c->devices[0].mirror_x);
    CHECK(c->devices[0].mirror_y);
    CHECK(c->devices[0].rotation == 270);
    CHECK(c->devices[0].calibration.size() == 6);
    REQUIRE(c->destinations.size() == 1);
    CHECK(c->destinations[0].address.port == 9481);
    CHECK(c->destinations[0].touches);
    CHECK_FALSE(c->destinations[0].status);
}

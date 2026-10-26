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
    CHECK(d.usb_recovery);
    auto off = parse_config(std::string(kBase) + "usb_recovery = false\n", "t.toml");
    REQUIRE(off);
    CHECK_FALSE(off->devices[0].usb_recovery);
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
    CHECK(refused(std::string(kBase) + "colour = \"red\"\n").find("device \"face\" (the 1st [[device]]): colour: unknown key") != std::string::npos);
    CHECK(refused(std::string(kBase) + "rotation = 45\n").find("device \"face\" (the 1st [[device]]): rotation") != std::string::npos);
    CHECK(refused(std::string(kBase) + "rotation = 300\n").find("device \"face\" (the 1st [[device]]): rotation: must be 0, 90, 180 or 270") != std::string::npos);
    CHECK(refused(std::string(kBase) + "grab = 1\n").find("device \"face\" (the 1st [[device]]): grab: must be true or false") != std::string::npos);
    CHECK(refused(std::string(kBase) + "silence_ms = 10\n").find("device \"face\" (the 1st [[device]]): silence_ms") != std::string::npos);
    CHECK(refused(std::string(kBase) + "mirror = \"z\"\n").find("device \"face\" (the 1st [[device]]): mirror") != std::string::npos);
    CHECK(refused(std::string(kBase) + "calibration = [1, 2, 3]\n").find("device \"face\" (the 1st [[device]]): calibration") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\nusb = \"712:9\"\n").find("device \"a\" (the 1st [[device]]): usb: must be vendor:product") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\nlisten = \"nowhere\"\n[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.listen") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"localhost\"]\n[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.allowed_ips") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\nprefix = \"/touch/\"\n[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.prefix") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"Face\"\nusb = \"0001:0002\"\n").find("device \"Face\" (the 1st [[device]]): id: may only use") != std::string::npos);
}

TEST_CASE("an unknown key is answered with the closest known one, and errors name the device") {
    CHECK(refused(std::string(kBase) + "rotaton = 90\n") == "t.toml: device \"face\" (the 1st [[device]]): rotaton: unknown key (did you mean rotation?)");
    CHECK(refused(std::string(kBase) + "colour = 1\n") == "t.toml: device \"face\" (the 1st [[device]]): colour: unknown key");
    CHECK(refused(std::string(kBase) + "match_prot = \"3-3\"\n").find("(did you mean match_port?)") != std::string::npos);
    CHECK(refused(std::string(kBase) + "[[destinations]]\naddress = \"10.0.0.1:9000\"\n") == "t.toml: destinations: unknown key (did you mean destination?)");
    CHECK(refused(std::string(kBase) + "[[device]]\nid = \"back\"\nusb = \"0712:0009\"\nrotation = 45\n")
              .find("device \"back\" (the 2nd [[device]]): rotation: must be") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"device\"\nusb = \"0001:0002\"\n")
              .find("id: \"device\" is a name touch-to-osc uses in its own addresses") != std::string::npos);
}

TEST_CASE("a config that cannot work is refused with what to do") {
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n").find("there is no [[device]]") != std::string::npos);
    CHECK(refused("[[device]]\nid = \"a\"\nusb = \"0001:0002\"\n").find("osc.allowed_ips is empty") != std::string::npos);
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\n").find("set match_usb (vendor:product) or match_port") != std::string::npos);
    CHECK(refused(std::string(kBase) + "[[device]]\nid = \"face\"\nport = \"3-3\"\n").find("two devices are called \"face\"") != std::string::npos);
    CHECK(refused(std::string(kBase) + "[[destination]]\naddress = \"10.0.0.1:9000\"\ntouches = false\nstatus = false\n").find("destination \"10.0.0.1:9000\" (the 1st [[destination]]): sends nothing") != std::string::npos);
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

TEST_CASE("match_usb, match_serial and match_port are the config's names too, and the older ones still work") {
    auto now = parse_config("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\nmatch_usb = \"0712:0009\"\nmatch_serial = \"S1\"\nmatch_port = \"3-3\"\n", "t.toml");
    REQUIRE(now);
    CHECK(now->devices[0].has_usb);
    CHECK(now->devices[0].product == 0x0009);
    CHECK(now->devices[0].serial == "S1");
    CHECK(now->devices[0].port == "3-3");
    auto before = parse_config("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\nusb = \"0712:0009\"\nport = \"3-3\"\n", "t.toml");
    REQUIRE(before);
    CHECK(before->devices[0].port == "3-3");
    CHECK(refused("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\nusb = \"0712:0009\"\nmatch_usb = \"0712:0009\"\n") ==
          "t.toml: device \"a\" (the 1st [[device]]): usb: is the older name of match_usb: give one of them");
}

TEST_CASE("devices that cannot be told apart are named, and ones that can are not") {
    auto two = [](const char* a, const char* b) {
        auto c = parse_config(std::string("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"a\"\n") + a + "\n[[device]]\nid = \"b\"\n" + b + "\n", "t.toml");
        REQUIRE(c);
        return overlap_warnings(*c);
    };
    auto same = two("usb = \"0712:0009\"", "usb = \"0712:0009\"");
    REQUIRE(same.size() == 1);
    CHECK(same[0].find("devices a and b can both read the same touchscreen") == 0);
    CHECK(same[0].find("set match_port") != std::string::npos);
    CHECK(two("usb = \"0712:0009\"\nport = \"3-3\"", "usb = \"0712:0009\"\nport = \"3-4\"").empty());
    CHECK(two("usb = \"0712:0009\"\nport = \"3-3\"", "usb = \"0712:0009\"").empty());
    CHECK(two("usb = \"0712:0009\"", "usb = \"0eef:0001\"").empty());
    auto port = two("port = \"3-3\"", "usb = \"0712:0009\"\nport = \"3-3\"");
    REQUIRE(port.size() == 1);
    CHECK(port[0].find("both name port 3-3") != std::string::npos);
}

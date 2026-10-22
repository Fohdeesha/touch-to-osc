#include "config/settings.h"

#include <doctest.h>

using namespace tto;

namespace {

Config base() {
    auto c = parse_config(R"(
[osc]
allowed_ips = ["127.0.0.1", "192.168.1.27"]
[paths]
settings = "/tmp/x.toml"
[[device]]
id = "face"
usb = "0712:0009"
)",
                          "t.toml");
    REQUIRE(c);
    return *c;
}

}

TEST_CASE("every kind of saved setting survives the settings file and comes back the same") {
    Config b = base();
    Overrides o;
    o.log_level = "debug";
    o.allowed_ips = std::vector<std::string>{"127.0.0.1", "10.0.0.5"};
    DestinationConfig d;
    d.address = HostPort{"192.168.1.27", 9481};
    d.status = false;
    o.saved_destinations = std::vector<DestinationConfig>{d};
    DeviceOverrides& x = o.devices["face"];
    x.label = "the face";
    x.enabled = false;
    x.rotation = 90;
    x.mirror = "x";
    x.calibration = std::vector<double>{1, 0, 0.01, 0, 1, -0.02};
    x.aspect = 1.25;
    x.silence_ms = 300;
    x.grab = false;
    x.match_usb = "1234:abcd";
    x.match_serial = "SN1";
    x.match_port = "3-3";
    x.usb_recovery = false;
    std::string text = settings_text(o, "/etc/touch-to-osc/touch-to-osc.toml");
    auto back = parse_settings(text, "s.toml", b);
    INFO(text);
    REQUIRE(back);
    CHECK(override_names(back->overrides) == override_names(o));
    Config eff = apply_overrides(b, back->overrides);
    const DeviceConfig& f = eff.devices[0];
    CHECK(f.label == "the face");
    CHECK_FALSE(f.enabled);
    CHECK(f.rotation == 90);
    CHECK(f.mirror_x);
    CHECK_FALSE(f.mirror_y);
    CHECK(f.calibration == std::vector<double>{1, 0, 0.01, 0, 1, -0.02});
    CHECK(f.aspect == 1.25);
    CHECK(f.silence_ms == 300);
    CHECK_FALSE(f.grab);
    CHECK(f.vendor == 0x1234);
    CHECK(f.product == 0xabcd);
    CHECK(f.serial == "SN1");
    CHECK(f.port == "3-3");
    CHECK_FALSE(f.usb_recovery);
    CHECK(eff.log.level == "debug");
    CHECK(eff.osc.allowed_ips == std::vector<std::string>{"127.0.0.1", "10.0.0.5"});
    REQUIRE(eff.destinations.size() == 1);
    CHECK(eff.destinations[0].touches);
    CHECK_FALSE(eff.destinations[0].status);
}

TEST_CASE("a setting equal to the config's value is not an override") {
    Config b = base();
    Overrides o;
    o.devices["face"].rotation = 0;
    o.devices["face"].match_usb = "0712:0009";
    o.log_level = "info";
    o.allowed_ips = b.osc.allowed_ips;
    drop_unchanged(o, b);
    CHECK(o.empty());
    o.devices["face"].rotation = 180;
    drop_unchanged(o, b);
    CHECK(override_names(o) == std::vector<std::string>{"device/face/rotation"});
}

TEST_CASE("forgetting one setting or a missing one") {
    Overrides o;
    o.devices["face"].rotation = 90;
    o.devices["face"].label = "x";
    o.log_level = "debug";
    CHECK(forget_override(o, "device/face/rotation"));
    CHECK_FALSE(forget_override(o, "device/face/rotation"));
    CHECK_FALSE(forget_override(o, "device/nope/label"));
    CHECK_FALSE(forget_override(o, "colour"));
    CHECK(override_names(o) == std::vector<std::string>{"log_level", "device/face/label"});
    CHECK(forget_override(o, "device/face/label"));
    CHECK(forget_override(o, "log_level"));
    CHECK(o.empty());
}

TEST_CASE("a settings file with a bad value is refused naming the key, in the config's words") {
    Config b = base();
    auto r = parse_settings("[device.face]\nrotation = 45\n", "s.toml", b);
    REQUIRE_FALSE(r);
    CHECK(r.error().find("device.face.rotation: must be 0, 90, 180 or 270") != std::string::npos);
    CHECK(parse_settings("colour = 1\n", "s.toml", b).error().find("colour: unknown key") != std::string::npos);
    CHECK(parse_settings("[device.face]\nmatch_usb = \"nope\"\n", "s.toml", b).error().find("device.face.match_usb") != std::string::npos);
    CHECK(parse_settings("allowed_ips = []\n", "s.toml", b).error().find("lock out every host") != std::string::npos);
    CHECK(parse_settings("[device.face]\nmatch_usb = \"\"\n", "s.toml", b).error().find("would match nothing") != std::string::npos);
    CHECK(parse_settings("x = = 1", "s.toml", b).error().find("s.toml:1:") != std::string::npos);
}

TEST_CASE("settings for a device no longer in the config are dropped with a note") {
    Config b = base();
    auto r = parse_settings("[device.gone]\nrotation = 90\n[device.face]\nrotation = 180\n", "s.toml", b);
    REQUIRE(r);
    REQUIRE(r->notes.size() == 1);
    CHECK(r->notes[0].find("\"gone\"") != std::string::npos);
    CHECK(override_names(r->overrides) == std::vector<std::string>{"device/face/rotation"});
}

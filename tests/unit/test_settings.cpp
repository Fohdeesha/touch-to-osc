#include "config/settings.h"

#include <doctest.h>

using namespace tto;

namespace {

Config base(const char* extra_osc = "", int rotation = 0) {
    std::string text = std::string("[osc]\nallowed_ips = [\"127.0.0.1\", \"192.168.1.27\"") + extra_osc +
                       "]\n[paths]\nsettings = \"/tmp/x.toml\"\n[[device]]\nid = \"face\"\nusb = \"0712:0009\"\nrotation = " + std::to_string(rotation) + "\n";
    auto c = parse_config(text, "t.toml");
    REQUIRE(c);
    return *c;
}

}

TEST_CASE("every kind of saved setting survives the settings file and comes back the same") {
    Config b = base();
    Overrides o;
    o.log_level = "debug";
    o.allowed_added = {"10.0.0.5"};
    o.allowed_removed = {"192.168.1.27"};
    DestinationConfig d;
    d.address = HostPort{"192.168.1.27", 9481};
    d.status = false;
    o.destinations_added = {d};
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
    std::string text = settings_text(o, Overrides(), "/etc/touch-to-osc/touch-to-osc.toml");
    auto back = parse_settings(text, "s.toml", b);
    INFO(text);
    REQUIRE(back);
    CHECK_FALSE(back->rewrite);
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
    o.allowed_added = b.osc.allowed_ips;
    o.allowed_removed = {"10.9.9.9"};
    drop_unchanged(o, b);
    CHECK(o.empty());
    o.devices["face"].rotation = 180;
    drop_unchanged(o, b);
    CHECK(override_names(o) == std::vector<std::string>{"device/face/rotation"});
}

TEST_CASE("forgetting one setting, a whole list, a whole device or a missing one") {
    Overrides o;
    o.devices["face"].rotation = 90;
    o.devices["face"].label = "x";
    o.unused_devices["gone"].rotation = 180;
    o.log_level = "debug";
    o.allowed_added = {"10.0.0.5"};
    CHECK(forget_override(o, "device/face/rotation"));
    CHECK_FALSE(forget_override(o, "device/face/rotation"));
    CHECK_FALSE(forget_override(o, "device/nope/label"));
    CHECK_FALSE(forget_override(o, "colour"));
    CHECK(override_names(o) == std::vector<std::string>{"log_level", "allowed_ips", "device/face/label"});
    CHECK(forget_override(o, "allowed_ips"));
    CHECK(forget_override(o, "device/gone"));
    CHECK(forget_override(o, "device/face"));
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
    CHECK(parse_settings("allowed_ips_removed = [\"127.0.0.1\", \"192.168.1.27\"]\n", "s.toml", b).error().find("lock out every host") != std::string::npos);
    CHECK(parse_settings("[device.face]\nmatch_usb = \"\"\n", "s.toml", b).error().find("would match nothing") != std::string::npos);
    CHECK(parse_settings("x = = 1", "s.toml", b).error().find("s.toml:1:") != std::string::npos);
}

TEST_CASE("settings for a device no longer in the config are kept but not used") {
    Config b = base();
    auto r = parse_settings("[device.gone]\nrotation = 90\n[device.face]\nrotation = 180\n", "s.toml", b);
    REQUIRE(r);
    REQUIRE(r->notes.size() == 1);
    CHECK(r->notes[0].find("\"gone\" are kept but not used") != std::string::npos);
    CHECK(override_names(r->overrides) == std::vector<std::string>{"device/face/rotation"});
    std::string text = settings_text(r->overrides, Overrides(), "c.toml");
    CHECK(text.find("[device.gone]") != std::string::npos);
}

TEST_CASE("an older settings file's whole lists become changes on top of the config") {
    Config b = base();
    auto r = parse_settings("allowed_ips = [\"127.0.0.1\", \"10.0.0.5\"]\n", "s.toml", b);
    REQUIRE(r);
    CHECK(r->rewrite);
    CHECK(r->overrides.allowed_added == std::vector<std::string>{"10.0.0.5"});
    CHECK(r->overrides.allowed_removed == std::vector<std::string>{"192.168.1.27"});
    CHECK(apply_overrides(b, r->overrides).osc.allowed_ips == std::vector<std::string>{"127.0.0.1", "10.0.0.5"});
}

TEST_CASE("a host added to the config file is never hidden by allowed hosts changed over OSC") {
    Overrides o;
    o.allowed_added = {"10.0.0.5"};
    Config edited = base(", \"192.168.1.90\"");
    CHECK(apply_overrides(edited, o).osc.allowed_ips == std::vector<std::string>{"127.0.0.1", "192.168.1.27", "192.168.1.90", "10.0.0.5"});
}

TEST_CASE("a config value changed after a setting was saved over OSC applies at the next start") {
    const char* saved = "[device.face]\nrotation = 90\n[config_values.device.face]\nrotation = 0\n";
    auto same = parse_settings(saved, "s.toml", base("", 0));
    REQUIRE(same);
    CHECK(same->notes.empty());
    CHECK(apply_overrides(base("", 0), same->overrides).devices[0].rotation == 90);
    auto edited = parse_settings(saved, "s.toml", base("", 180));
    REQUIRE(edited);
    CHECK(edited->rewrite);
    REQUIRE(edited->notes.size() == 1);
    CHECK(edited->notes[0] == "device/face/rotation: the config file changed from 0 to 180 after 90 was saved over OSC, so the config file's 180 applies now");
    CHECK(edited->overrides.empty());
    CHECK(edited->config_values.devices.empty());
    auto older = parse_settings("[device.face]\nrotation = 90\n", "s.toml", base("", 180));
    REQUIRE(older);
    CHECK(older->notes.empty());
    CHECK(apply_overrides(base("", 180), older->overrides).devices[0].rotation == 90);
}

TEST_CASE("what the config file said is recorded when a setting changes, and only then") {
    Config file = base("", 0);
    Overrides values, before, after;
    after.devices["face"].rotation = 90;
    note_config_values(before, after, file, values);
    REQUIRE(values.devices.count("face"));
    CHECK(values.devices["face"].rotation == 0);
    Config edited = base("", 180);
    Overrides later = after;
    later.devices["face"].label = "x";
    note_config_values(after, later, edited, values);
    CHECK(values.devices["face"].rotation == 0);
    CHECK(values.devices["face"].label == std::string("face"));
    Overrides changed = later;
    changed.devices["face"].rotation = 270;
    note_config_values(later, changed, edited, values);
    CHECK(values.devices["face"].rotation == 180);
    Overrides forgot = changed;
    forgot.devices["face"].rotation.reset();
    note_config_values(changed, forgot, edited, values);
    CHECK_FALSE(values.devices["face"].rotation);
    std::string text = settings_text(changed, values, "c.toml");
    CHECK(text.find("[config_values.device.face]") != std::string::npos);
}

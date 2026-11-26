#include "config/settings.h"

#include <doctest.h>

#include <algorithm>

using namespace tto;

namespace {

Config base(const char* extra_osc = "", int rotation = 0) {
    std::string text = std::string("[osc]\nallowed_ips = [\"127.0.0.1\", \"192.168.1.27\"") + extra_osc +
                       "]\n[paths]\nsettings = \"/tmp/x.toml\"\n[[device]]\nid = \"screen\"\nusb = \"0712:0009\"\nrotation = " + std::to_string(rotation) + "\n";
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
    DeviceOverrides& x = o.devices["screen"];
    x.label = "the screen";
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
    Overrides values;
    note_config_values(Overrides(), o, b, values);
    std::string text = settings_text(o, values, "/etc/touch-to-osc/touch-to-osc.toml");
    auto back = parse_settings(text, "s.toml", b);
    INFO(text);
    REQUIRE(back);
    CHECK_FALSE(back->rewrite);
    CHECK(settings_text(back->overrides, back->config_values, "/etc/touch-to-osc/touch-to-osc.toml") == text);
    CHECK(override_names(back->overrides) == override_names(o));
    Config eff = apply_overrides(b, back->overrides);
    const DeviceConfig& f = eff.devices[0];
    CHECK(f.label == "the screen");
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
    o.devices["screen"].rotation = 0;
    o.devices["screen"].match_usb = "0712:0009";
    o.log_level = "info";
    o.allowed_added = b.osc.allowed_ips;
    o.allowed_removed = {"10.9.9.9"};
    drop_unchanged(o, b);
    CHECK(o.empty());
    o.devices["screen"].rotation = 180;
    drop_unchanged(o, b);
    CHECK(override_names(o) == std::vector<std::string>{"device/screen/rotation"});
}

TEST_CASE("forgetting one setting, a whole list, a whole device or a missing one") {
    Overrides o;
    o.devices["screen"].rotation = 90;
    o.devices["screen"].label = "x";
    o.unused_devices["gone"].rotation = 180;
    o.log_level = "debug";
    o.allowed_added = {"10.0.0.5"};
    CHECK(forget_override(o, "device/screen/rotation"));
    CHECK_FALSE(forget_override(o, "device/screen/rotation"));
    CHECK_FALSE(forget_override(o, "device/nope/label"));
    CHECK_FALSE(forget_override(o, "colour"));
    CHECK(override_names(o) == std::vector<std::string>{"log_level", "allowed_ips", "device/screen/label"});
    CHECK(forget_override(o, "allowed_ips"));
    CHECK(forget_override(o, "device/gone"));
    CHECK(forget_override(o, "device/screen"));
    CHECK(forget_override(o, "log_level"));
    CHECK(o.empty());
}

TEST_CASE("a settings file with a bad value is refused naming the key, in the config's words") {
    Config b = base();
    auto r = parse_settings("[device.screen]\nrotation = 45\n", "s.toml", b);
    REQUIRE_FALSE(r);
    CHECK(r.error().find("device.screen.rotation: must be 0, 90, 180 or 270") != std::string::npos);
    CHECK(parse_settings("colour = 1\n", "s.toml", b).error().find("colour: unknown key") != std::string::npos);
    CHECK(parse_settings("[device.screen]\nmatch_usb = \"nope\"\n", "s.toml", b).error().find("device.screen.match_usb") != std::string::npos);
    CHECK(parse_settings("allowed_ips = []\n", "s.toml", b).error().find("lock out every host") != std::string::npos);
    CHECK(parse_settings("x = = 1", "s.toml", b).error().find("s.toml:1:") != std::string::npos);
    CHECK(parse_settings("[[devices_added]]\nmatch_usb = \"0712:0009\"\n", "s.toml", b).error().find("devices_added[1].id: is missing") != std::string::npos);
    CHECK(parse_settings("allowed_ips_added = [\"10.0.0.5\\u0000\"]\n", "s.toml", b).error().find("allowed_ips_added: may only use printable ASCII") !=
          std::string::npos);
    CHECK(parse_settings("[device.screen]\nsilence_ms = 1e300\n", "s.toml", b).error().find("device.screen.silence_ms: must be between") != std::string::npos);
    CHECK(parse_settings("[device.screen]\ncalibration = [1, 2, 3]\n", "s.toml", b).error().find("device.screen.calibration: must be six numbers") != std::string::npos);
}

TEST_CASE("a saved setting the config file has since made impossible is dropped by itself, and the rest stays") {
    Config b = base();
    auto hosts = parse_settings("allowed_ips_removed = [\"127.0.0.1\", \"192.168.1.27\"]\n[device.screen]\nrotation = 90\n", "s.toml", b);
    REQUIRE(hosts);
    CHECK(hosts->rewrite);
    REQUIRE(hosts->notes.size() == 1);
    CHECK(hosts->notes[0].find("would lock out every host: they are allowed again") != std::string::npos);
    CHECK(hosts->overrides.allowed_removed.empty());
    CHECK(apply_overrides(b, hosts->overrides).osc.allowed_ips == b.osc.allowed_ips);
    CHECK(apply_overrides(b, hosts->overrides).devices[0].rotation == 90);

    auto match = parse_settings("[device.screen]\nmatch_usb = \"\"\nrotation = 90\n", "s.toml", b);
    REQUIRE(match);
    CHECK(match->rewrite);
    REQUIRE(match->notes.size() == 1);
    CHECK(match->notes[0].find("device/screen: its match_usb and match_port saved over OSC would leave it matching nothing") == 0);
    Config eff = apply_overrides(b, match->overrides);
    CHECK(eff.devices[0].has_usb);
    CHECK(eff.devices[0].rotation == 90);

    auto flat = parse_settings("[device.screen]\ncalibration = [0, 0, 0.5, 0, 0, 0.5]\nrotation = 90\n", "s.toml", b);
    REQUIRE(flat);
    CHECK(flat->rewrite);
    REQUIRE(flat->notes.size() == 1);
    CHECK(flat->notes[0] == "device/screen/calibration: the calibration saved over OSC would put every touch on one line, so it is dropped");
    CHECK(apply_overrides(b, flat->overrides).devices[0].calibration.empty());
    CHECK(apply_overrides(b, flat->overrides).devices[0].rotation == 90);

    auto devices = parse_settings("devices_removed = [\"screen\"]\nlog_level = \"debug\"\n", "s.toml", b);
    REQUIRE(devices);
    CHECK(devices->rewrite);
    REQUIRE(devices->notes.size() == 1);
    CHECK(devices->notes[0].find("would leave no device: they are read again") != std::string::npos);
    CHECK(apply_overrides(b, devices->overrides).devices.size() == 1);
    CHECK(apply_overrides(b, devices->overrides).log.level == "debug");
}

TEST_CASE("settings for a device no longer in the config are kept but not used") {
    Config b = base();
    auto r = parse_settings("[device.gone]\nrotation = 90\n[device.screen]\nrotation = 180\n", "s.toml", b);
    REQUIRE(r);
    REQUIRE(r->notes.size() == 1);
    CHECK(r->notes[0].find("\"gone\" are kept but not used") != std::string::npos);
    CHECK(override_names(r->overrides) == std::vector<std::string>{"device/screen/rotation"});
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
    auto on_copy = parse_settings("allowed_ips = [\"127.0.0.1\", \"10.0.0.5\"]\n", "s.toml", b, false);
    REQUIRE(on_copy);
    CHECK_FALSE(on_copy->rewrite);
    CHECK(on_copy->overrides.allowed_added == std::vector<std::string>{"10.0.0.5"});
}

TEST_CASE("a host added to the config file is never hidden by allowed hosts changed over OSC") {
    Overrides o;
    o.allowed_added = {"10.0.0.5"};
    Config edited = base(", \"192.168.1.90\"");
    CHECK(apply_overrides(edited, o).osc.allowed_ips == std::vector<std::string>{"127.0.0.1", "192.168.1.27", "192.168.1.90", "10.0.0.5"});
}

TEST_CASE("a config value changed after a setting was saved over OSC applies at the next start") {
    const char* saved = "[device.screen]\nrotation = 90\n[config_values.device.screen]\nrotation = 0\n";
    auto same = parse_settings(saved, "s.toml", base("", 0));
    REQUIRE(same);
    CHECK(same->notes.empty());
    CHECK(apply_overrides(base("", 0), same->overrides).devices[0].rotation == 90);
    auto edited = parse_settings(saved, "s.toml", base("", 180));
    REQUIRE(edited);
    CHECK(edited->rewrite);
    REQUIRE(edited->notes.size() == 1);
    CHECK(edited->notes[0] == "device/screen/rotation: the config file changed from 0 to 180 after 90 was saved over OSC, so the config file's 180 applies now");
    CHECK(edited->overrides.empty());
    CHECK(edited->config_values.devices.empty());
    auto older = parse_settings("[device.screen]\nrotation = 90\n", "s.toml", base("", 180));
    REQUIRE(older);
    CHECK(older->notes.empty());
    CHECK(apply_overrides(base("", 180), older->overrides).devices[0].rotation == 90);
    CHECK(older->rewrite);
    REQUIRE(older->config_values.devices.count("screen"));
    CHECK(older->config_values.devices["screen"].rotation == 180);
}

TEST_CASE("the listen address and the prefix follow the same rule: a config value changed afterwards applies") {
    auto with = [](const char* listen, const char* prefix) {
        auto c = parse_config(std::string("[osc]\nlisten = \"") + listen + "\"\nprefix = \"" + prefix +
                                  "\"\nallowed_ips = [\"127.0.0.1\"]\n[[device]]\nid = \"screen\"\nmatch_usb = \"0712:0009\"\n",
                              "t.toml");
        REQUIRE(c);
        return *c;
    };
    Config file = with("0.0.0.0:9480", "/touch");
    Overrides over, values;
    over.listen = HostPort{"0.0.0.0", 9500};
    over.prefix = "/panel";
    note_config_values(Overrides(), over, file, values);
    REQUIRE(values.listen);
    CHECK(values.listen->text() == "0.0.0.0:9480");
    CHECK(values.prefix == std::optional<std::string>("/touch"));
    std::string saved = settings_text(over, values, "t.toml");
    CHECK(saved.find("[config_values]") != std::string::npos);

    auto same = parse_settings(saved, "s.toml", file);
    REQUIRE(same);
    CHECK(same->notes.empty());
    CHECK_FALSE(same->rewrite);
    CHECK(apply_overrides(file, same->overrides).osc.listen.port == 9500);
    CHECK(apply_overrides(file, same->overrides).osc.prefix == "/panel");

    Config moved = with("0.0.0.0:9600", "/touch");
    auto listen = parse_settings(saved, "s.toml", moved);
    REQUIRE(listen);
    CHECK(listen->rewrite);
    REQUIRE(listen->notes.size() == 1);
    CHECK(listen->notes[0] == "listen: the config file changed from \"0.0.0.0:9480\" to \"0.0.0.0:9600\" after \"0.0.0.0:9500\" was saved over OSC, so the "
                              "config file's \"0.0.0.0:9600\" applies now");
    CHECK(apply_overrides(moved, listen->overrides).osc.listen.port == 9600);
    CHECK(apply_overrides(moved, listen->overrides).osc.prefix == "/panel");

    Config renamed = with("0.0.0.0:9480", "/wall");
    auto prefix = parse_settings(saved, "s.toml", renamed);
    REQUIRE(prefix);
    REQUIRE(prefix->notes.size() == 1);
    CHECK(prefix->notes[0].find("prefix: the config file changed from \"/touch\" to \"/wall\"") == 0);
    CHECK(apply_overrides(renamed, prefix->overrides).osc.prefix == "/wall");
    CHECK(apply_overrides(renamed, prefix->overrides).osc.listen.port == 9500);

    auto copy = parse_settings(saved, "s.toml", moved, false);
    REQUIRE(copy);
    CHECK(copy->notes.empty());
    CHECK(apply_overrides(moved, copy->overrides).osc.listen.port == 9500);

    Overrides gone = over;
    gone.listen.reset();
    note_config_values(over, gone, file, values);
    CHECK_FALSE(values.listen);
    CHECK(values.prefix);

    // a file saved by a version that did not keep what the config said: kept as it is, and the rule holds from now on
    std::string older = "listen = \"0.0.0.0:9500\"\nprefix = \"/panel\"\nlog_level = \"debug\"\n";
    auto first = parse_settings(older, "s.toml", moved);
    REQUIRE(first);
    CHECK(first->notes.empty());
    CHECK(first->rewrite);
    CHECK(apply_overrides(moved, first->overrides).osc.listen.port == 9500);
    REQUIRE(first->config_values.listen);
    CHECK(first->config_values.listen->text() == "0.0.0.0:9600");
    CHECK(first->config_values.prefix == std::optional<std::string>("/touch"));
    CHECK(first->config_values.log_level == std::optional<std::string>("info"));
    std::string rewritten = settings_text(first->overrides, first->config_values, "t.toml");
    auto later = parse_settings(rewritten, "s.toml", with("0.0.0.0:9700", "/touch"));
    REQUIRE(later);
    CHECK(apply_overrides(with("0.0.0.0:9700", "/touch"), later->overrides).osc.listen.port == 9700);
    CHECK(apply_overrides(with("0.0.0.0:9700", "/touch"), later->overrides).osc.prefix == "/panel");

    // on the copy of the config that last worked nothing is judged against the file, and nothing is written back
    auto on_copy = parse_settings(older, "s.toml", moved, false);
    REQUIRE(on_copy);
    CHECK_FALSE(on_copy->rewrite);
    CHECK_FALSE(on_copy->config_values.listen);
    CHECK(apply_overrides(moved, on_copy->overrides).osc.listen.port == 9500);
}

TEST_CASE("a saved listen address or destination that would have touch-to-osc send to itself is dropped") {
    auto c = parse_config("[osc]\nallowed_ips = [\"127.0.0.1\"]\n[[destination]]\naddress = \"127.0.0.1:9500\"\n[[device]]\nid = \"screen\"\nmatch_usb = \"0712:0009\"\n",
                          "t.toml");
    REQUIRE(c);
    auto listen = parse_settings("listen = \"0.0.0.0:9500\"\nlog_level = \"debug\"\n", "s.toml", *c);
    REQUIRE(listen);
    CHECK_FALSE(listen->overrides.listen);
    CHECK(listen->overrides.log_level);
    REQUIRE(listen->notes.size() == 1);
    CHECK(listen->notes[0].find("is the address of a destination now") != std::string::npos);
    auto dest = parse_settings("[[saved_destinations_added]]\naddress = \"127.0.0.1:9480\"\ntouches = true\nstatus = true\n", "s.toml", *c);
    REQUIRE(dest);
    CHECK(dest->overrides.destinations_added.empty());
    CHECK(sends_to_itself(HostPort{"0.0.0.0", 9480}, HostPort{"127.0.0.1", 9480}));
    CHECK(sends_to_itself(HostPort{"192.168.1.50", 9480}, HostPort{"192.168.1.50", 9480}));
    CHECK_FALSE(sends_to_itself(HostPort{"192.168.1.50", 9480}, HostPort{"127.0.0.1", 9480}));
    CHECK_FALSE(sends_to_itself(HostPort{"0.0.0.0", 9480}, HostPort{"127.0.0.1", 9481}));
    CHECK_FALSE(sends_to_itself(HostPort{"0.0.0.0", 9480}, HostPort{"192.168.1.27", 9480}));
}

TEST_CASE("what the config file said is recorded when a setting changes, and only then") {
    Config file = base("", 0);
    Overrides values, before, after;
    after.devices["screen"].rotation = 90;
    note_config_values(before, after, file, values);
    REQUIRE(values.devices.count("screen"));
    CHECK(values.devices["screen"].rotation == 0);
    Config edited = base("", 180);
    Overrides later = after;
    later.devices["screen"].label = "x";
    note_config_values(after, later, edited, values);
    CHECK(values.devices["screen"].rotation == 0);
    CHECK(values.devices["screen"].label == std::string("screen"));
    Overrides changed = later;
    changed.devices["screen"].rotation = 270;
    note_config_values(later, changed, edited, values);
    CHECK(values.devices["screen"].rotation == 180);
    Overrides forgot = changed;
    forgot.devices["screen"].rotation.reset();
    note_config_values(changed, forgot, edited, values);
    CHECK_FALSE(values.devices["screen"].rotation);
    std::string text = settings_text(changed, values, "c.toml");
    CHECK(text.find("[config_values.device.screen]") != std::string::npos);
}

TEST_CASE("the listen address, the prefix and devices added or removed over OSC survive the settings file") {
    Config b = base();
    Overrides o;
    o.listen = HostPort{"0.0.0.0", 9490};
    o.prefix = "/screen";
    DeviceConfig two;
    two.id = "back";
    two.label = "back";
    two.has_usb = true;
    two.vendor = 0x0712;
    two.product = 0x0009;
    two.port = "3-4";
    o.devices_added = {two};
    o.devices["back"].rotation = 90;
    auto back = parse_settings(settings_text(o, Overrides(), "t.toml"), "s.toml", b);
    REQUIRE(back);
    const Overrides& r = back->overrides;
    REQUIRE(r.listen);
    CHECK(r.listen->text() == "0.0.0.0:9490");
    CHECK(r.prefix == std::optional<std::string>("/screen"));
    REQUIRE(r.devices_added.size() == 1);
    CHECK(r.devices_added[0].id == "back");
    CHECK(r.devices_added[0].port == "3-4");
    CHECK(r.devices.count("back") == 1);
    Config eff = apply_overrides(b, r);
    REQUIRE(eff.devices.size() == 2);
    CHECK(eff.devices[1].id == "back");
    CHECK(eff.devices[1].rotation == 90);
    CHECK(eff.osc.prefix == "/screen");
    CHECK(eff.osc.listen.port == 9490);
    std::vector<std::string> names = override_names(r);
    CHECK(std::find(names.begin(), names.end(), "devices") != names.end());
    CHECK(std::find(names.begin(), names.end(), "listen") != names.end());

    Overrides gone;
    gone.devices_removed = {"screen"};
    gone.devices_added = {two};
    auto removed = parse_settings(settings_text(gone, Overrides(), "t.toml"), "s.toml", b);
    REQUIRE(removed);
    Config only = apply_overrides(b, removed->overrides);
    REQUIRE(only.devices.size() == 1);
    CHECK(only.devices[0].id == "back");
    Overrides forget = r;
    CHECK(forget_override(forget, "devices"));
    CHECK(forget.devices_added.empty());
    CHECK(forget_override(forget, "listen"));
    CHECK_FALSE(forget.listen);
}

TEST_CASE("a device added over OSC that the config file now has by the same id is the config file's") {
    Config b = base();
    Overrides o;
    DeviceConfig twin;
    twin.id = "screen";
    twin.label = "screen";
    twin.port = "3-9";
    o.devices_added = {twin};
    auto back = parse_settings(settings_text(o, Overrides(), "t.toml"), "s.toml", b);
    REQUIRE(back);
    CHECK(back->overrides.devices_added.empty());
    CHECK(back->rewrite);
    REQUIRE(back->notes.size() == 1);
    CHECK(back->notes[0].find("the config file's applies") != std::string::npos);
    CHECK_FALSE(parse_settings("prefix = \"touch\"\n", "s.toml", b));
    CHECK_FALSE(parse_settings("listen = \"nowhere\"\n", "s.toml", b));
    CHECK(parse_settings("[[devices_added]]\nid = \"device\"\nmatch_usb = \"0712:0009\"\n", "s.toml", b).error().find("is a name touch-to-osc uses") !=
          std::string::npos);
}

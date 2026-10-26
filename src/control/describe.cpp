#include "control/server.h"

#include "control/usb.h"

#include <algorithm>
#include <cstdio>

namespace tto {

namespace {

std::string json(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20 || c >= 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

std::string json(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

std::string json(int v) { return std::to_string(v); }
std::string json(bool v) { return v ? "1" : "0"; }

std::string json(const std::vector<double>& v) {
    std::string s = "[";
    for (double x : v) s += (s.size() > 1 ? "," : "") + json(x);
    return s + "]";
}

std::string json(const std::vector<std::string>& v) {
    std::string s = "[";
    for (const auto& x : v) s += (s.size() > 1 ? "," : "") + json(x);
    return s + "]";
}

class Item {
public:
    template <typename T>
    Item& add(const char* key, const T& v) {
        s_ += (s_.size() > 1 ? "," : "") + json(std::string(key)) + ":" + json(v);
        return *this;
    }
    Item& raw(const char* key, const std::string& v) {
        s_ += (s_.size() > 1 ? "," : "") + json(std::string(key)) + ":" + v;
        return *this;
    }
    std::string text() const { return s_ + "}"; }

private:
    std::string s_ = "{";
};

std::string arg(const char* name, const char* type, const std::string& range = std::string()) {
    Item a;
    a.add("name", std::string(name)).add("type", std::string(type));
    if (!range.empty()) a.raw("range", range);
    return a.text();
}

}

std::vector<std::string> Server::describe_items() const {
    std::vector<std::string> out;
    auto setting = [&](const std::string& address, const std::string& device, const char* label, const char* type, const char* effect, const char* help) {
        Item i;
        i.add("kind", std::string("setting")).add("address", address);
        if (!device.empty()) i.add("device", device);
        i.add("label", std::string(label)).add("type", std::string(type)).add("effect", std::string(effect)).add("help", std::string(help));
        return i;
    };
    auto action = [&](const std::string& address, const std::string& device, const char* label, const std::string& args, const std::string& confirm,
                      const char* help) {
        Item i;
        i.add("kind", std::string("action")).add("address", address);
        if (!device.empty()) i.add("device", device);
        i.add("label", std::string(label)).raw("args", "[" + args + "]");
        if (!confirm.empty()) i.add("confirm", confirm);
        i.add("help", std::string(help));
        out.push_back(i.text());
    };
    auto source = [](bool saved) { return std::string(saved ? "saved" : "config"); };

    for (size_t k = 0; k < cfg_.devices.size(); ++k) {
        const DeviceConfig& d = cfg_.devices[k];
        const DeviceConfig* found = file_device(d.id);
        const DeviceConfig& f = found ? *found : d;
        bool added = std::none_of(base_.devices.begin(), base_.devices.end(), [&](const DeviceConfig& x) { return x.id == d.id; });
        auto it = over_.devices.find(d.id);
        DeviceOverrides none;
        const DeviceOverrides& o = it == over_.devices.end() ? none : it->second;
        Item dev;
        dev.add("kind", std::string("device")).add("id", d.id).add("label", d.label).add("source", std::string(added ? "saved" : "config"));
        out.push_back(dev.text());
        std::string b = "device/" + d.id + "/";
        out.push_back(setting(b + "label", d.id, "Label", "text", "live", "What people call it, used in health messages.")
                          .add("max_length", 64).add("value", d.label).add("config", f.label).add("source", source(bool(o.label))).text());
        out.push_back(setting(b + "enabled", d.id, "Enabled", "flag", "cancels", "Turned off, the screen sends no touches; its contacts are cancelled.")
                          .add("value", d.enabled).add("config", f.enabled).add("source", source(bool(o.enabled))).text());
        out.push_back(setting(b + "rotation", d.id, "Rotation", "int", "cancels", "Turns touch positions clockwise to match the display.")
                          .raw("values", "[0,90,180,270]").add("unit", std::string("degrees")).add("value", d.rotation).add("config", f.rotation)
                          .add("source", source(bool(o.rotation))).text());
        out.push_back(setting(b + "mirror", d.id, "Mirror", "text", "cancels", "Flips touch positions after the rotation: x left-right, y top-bottom.")
                          .raw("values", "[\"\",\"x\",\"y\",\"xy\"]").add("value", d.mirror_text()).add("config", f.mirror_text())
                          .add("source", source(bool(o.mirror))).text());
        out.push_back(setting(b + "calibration", d.id, "Calibration", "floats", "cancels",
                              "Six numbers a b c d e f: x' = a*x + b*y + c, y' = d*x + e*y + f, lining the touch panel up with the display; none for no correction.")
                          .raw("counts", "[0,6]").raw("range", "[-100,100]").add("value", d.calibration).add("config", f.calibration)
                          .add("source", source(bool(o.calibration))).text());
        out.push_back(setting(b + "aspect", d.id, "Aspect", "float", "live", "Width / height of the display area; 0 takes it from the device.")
                          .raw("values", "[0]").raw("range", "[0.1,10]").add("value", d.aspect).add("config", f.aspect).add("source", source(bool(o.aspect)))
                          .text());
        out.push_back(setting(b + "silence_ms", d.id, "Silence cancel", "int", "live",
                              "Cancels the contacts that are down when the screen sends nothing for this long; 0 is off, needed for screens that report only changes.")
                          .raw("values", "[0]").raw("range", "[50,10000]").add("unit", std::string("ms")).add("value", d.silence_ms).add("config", f.silence_ms)
                          .add("source", source(bool(o.silence_ms))).text());
        out.push_back(setting(b + "grab", d.id, "Hold exclusively", "flag", "reopens", "Holds the screen so nothing else on the machine acts on its touches.")
                          .add("value", d.grab).add("config", f.grab).add("source", source(bool(o.grab))).text());
        out.push_back(setting(b + "match_usb", d.id, "USB id", "text", "reopens", "The vvvv:pppp of the screen to read, or empty to match by port alone.")
                          .add("value", d.usb_setting()).add("config", f.usb_setting()).add("source", source(bool(o.match_usb))).text());
        out.push_back(setting(b + "match_serial", d.id, "USB serial", "text", "reopens", "The screen's USB serial, when two screens of one model differ in it.")
                          .add("value", d.serial).add("config", f.serial).add("source", source(bool(o.match_serial))).text());
        out.push_back(setting(b + "match_port", d.id, "USB port", "text", "reopens", "The USB socket the screen is plugged into, such as 3-3, to tell identical screens apart.")
                          .add("value", d.port).add("config", f.port).add("source", source(bool(o.match_port))).text());
        out.push_back(setting(b + "usb_recovery", d.id, "USB recovery", "flag", "live", "Resets a USB connection the kernel gave up on while the screen is gone.")
                          .add("value", d.usb_recovery).add("config", f.usb_recovery).add("source", source(bool(o.usb_recovery))).text());
        auto ref = usb_port_of(k);
        action(b + "usb_reset", d.id, "Reset USB", "",
               ref ? "Every device on " + usb_target_text(*ref) + " drops for a moment." : std::string("Every device on that USB hub drops for a moment."),
               "Resets the USB hub the screen's port belongs to now.");
        action(b + "calibrate", d.id, "Guided calibration", arg("seconds", "int", "[0,60]"), "",
               "Pauses this screen's touches and reports each touch before calibration, for an app that shows targets; 0 ends it.");
        action(b + "calibration/fit", d.id, "Fit calibration", arg("points", "floats", "[12,64]"), "",
               "Fits and saves the calibration from x y u v points: where each target was shown and where it was touched.");
    }
    out.push_back(setting("log_level", "", "Log level", "text", "live", "How much goes into the journal.")
                      .raw("values", "[\"crit\",\"error\",\"warning\",\"notice\",\"info\",\"debug\"]").add("value", cfg_.log.level).add("config", base_.log.level)
                      .add("source", source(bool(over_.log_level))).text());
    out.push_back(setting("allowed_ips", "", "Allowed hosts", "list", "live", "The hosts that may subscribe and send commands.")
                      .add("value", cfg_.osc.allowed_ips).add("config", base_.osc.allowed_ips)
                      .add("source", source(!over_.allowed_added.empty() || !over_.allowed_removed.empty())).text());
    std::vector<std::string> dests, file_dests;
    for (const auto& d : cfg_.destinations) dests.push_back(destination_text(d));
    for (const auto& d : base_.destinations) file_dests.push_back(destination_text(d));
    out.push_back(setting("saved_destinations", "", "Saved destinations", "list", "live", "Receivers that get everything without subscribing.")
                      .add("value", dests).add("config", file_dests)
                      .add("source", source(!over_.destinations_added.empty() || !over_.destinations_removed.empty())).text());
    out.push_back(setting("listen", "", "Listen address", "text", "restart", "The UDP address and port touch-to-osc takes commands on.")
                      .add("value", cfg_.osc.listen.text()).add("config", base_.osc.listen.text()).add("source", source(bool(over_.listen))).text());
    out.push_back(setting("prefix", "", "Address prefix", "text", "restart", "Every OSC address starts with this.")
                      .add("value", cfg_.osc.prefix).add("config", base_.osc.prefix).add("source", source(bool(over_.prefix))).text());
    action("allowed_ips/add", "", "Allow a host", arg("ip", "text"), "", "Lets a host subscribe and send commands.");
    action("allowed_ips/remove", "", "Remove a host", arg("ip", "text"), "", "Stops a host from subscribing and sending commands.");
    action("saved_destinations/add", "", "Add a destination", arg("address", "text") + "," + arg("touches", "flag") + "," + arg("status", "flag"), "",
           "Sends everything to IP:PORT without a subscription.");
    action("saved_destinations/remove", "", "Remove a destination", arg("address", "text"), "", "Stops sending to IP:PORT.");
    action("devices/add", "", "Add a screen", arg("id", "text") + "," + arg("match_usb", "text") + "," + arg("match_port", "text"),
           "touch-to-osc restarts to add it: every contact is cancelled.", "Adds a device for a touchscreen; unused_screens lists the ones no device reads.");
    action("devices/remove", "", "Remove a screen", arg("id", "text"), "touch-to-osc restarts to remove it: every contact is cancelled.",
           "Removes a device.");
    action("identify", "", "Identify screens", arg("seconds", "int", "[0,60]"), "",
           "Pauses every screen's touches and reports which screen each touch lands on; 0 ends it.");
    action("settings/forget", "", "Forget saved settings", arg("name", "text"), "Settings changed here go back to the config file's values.",
           "Forgets one saved setting by its name in overrides, device/<id> for all of one device's, or all of them with no name.");
    action("restart", "", "Restart", "", "Every contact is cancelled and touch-to-osc starts again; touches stop for about a second.",
           "Starts touch-to-osc again from its config file and saved settings.");
    return out;
}

}

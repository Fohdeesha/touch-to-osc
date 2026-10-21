#include "config/config.h"

#include "config/strict_toml.h"
#include "core/log.h"
#include "osc/osc.h"

#include <arpa/inet.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace tto {

namespace {

void host_port(StrictTable& o, const char* key, HostPort& out, bool wildcard_ok) {
    std::string s;
    if (!o.str(key, s, 64, false)) return;
    HostPort hp;
    if (!parse_host_port(s, hp)) {
        o.fail(key, "must be an IPv4 address and a port, like \"192.168.1.27:9481\"");
        return;
    }
    if (!wildcard_ok && hp.host == "0.0.0.0") {
        o.fail(key, "must name a host, not 0.0.0.0");
        return;
    }
    out = hp;
}

template <typename F>
void section(StrictTable& root, const char* name, F&& fn) {
    const toml::node* v = root.take(name);
    if (!v) return;
    StrictTable o(v, name);
    if (o.ok()) fn(o);
    o.finish();
    if (!o.ok()) root.adopt(o.error());
}

template <typename F>
void table_list(StrictTable& root, const char* name, size_t max_items, F&& fn) {
    const toml::node* v = root.take(name);
    if (!v) return;
    if (!v->is_array_of_tables()) {
        root.fail(name, "must be written as [[" + std::string(name) + "]] tables");
        return;
    }
    const toml::array& a = *v->as_array();
    if (a.size() > max_items) {
        root.fail(name, "at most " + std::to_string(max_items) + " allowed");
        return;
    }
    size_t index = 0;
    for (auto&& e : a) {
        ++index;
        StrictTable o(&e, std::string(name) + "[" + std::to_string(index) + "]");
        if (o.ok()) fn(o);
        o.finish();
        if (!o.ok()) {
            root.adopt(o.error());
            return;
        }
    }
}

bool valid_id(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

bool parse_usb(const std::string& s, uint16_t& vendor, uint16_t& product) {
    if (s.size() != 9 || s[4] != ':') return false;
    auto hex = [](const std::string& h, uint16_t& out) {
        unsigned v = 0;
        for (char c : h) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        out = uint16_t(v);
        return true;
    };
    return hex(s.substr(0, 4), vendor) && hex(s.substr(5, 4), product);
}

void device_table(StrictTable& o, DeviceConfig& d) {
    o.str("id", d.id, 32, false);
    if (o.ok() && d.id.empty()) o.fail("id", "is missing: give the device a short name such as \"face\"");
    if (o.ok() && !valid_id(d.id)) o.fail("id", "may only use a-z, 0-9, _ and - (it becomes part of OSC addresses)");
    o.str("label", d.label, 64);
    std::string usb;
    if (o.str("usb", usb, 16) && !usb.empty()) {
        if (o.check("usb", check_usb(usb, &d.vendor, &d.product))) d.has_usb = true;
    }
    o.str("serial", d.serial, 128);
    o.str("port", d.port, 64);
    if (o.ok() && !d.has_usb && d.port.empty())
        o.fail("", "says nothing about which device to read: set usb (vendor:product) or port (both are shown by touch-to-osc --list)");
    o.boolean("grab", d.grab);
    long long rotation = 0;
    if (o.integer("rotation", rotation, -100000, 100000) && o.check("rotation", check_rotation(rotation))) d.rotation = int(rotation);
    std::string mirror;
    if (o.str("mirror", mirror, 4) && o.check("mirror", check_mirror(mirror))) set_mirror(d, mirror);
    double aspect = 0;
    if (o.number("aspect", aspect) && o.check("aspect", check_aspect(aspect))) d.aspect = aspect;
    std::vector<double> cal;
    if (o.numbers("calibration", cal) && o.check("calibration", check_calibration(cal))) d.calibration = cal;
    long long silence = 0;
    if (o.integer("silence_ms", silence, -100000, 100000) && o.check("silence_ms", check_silence(silence))) d.silence_ms = int(silence);
    o.boolean("enabled", d.enabled);
    if (o.ok() && d.label.empty()) d.label = d.id;
}

}

bool parse_host_port(const std::string& s, HostPort& out) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size() || s.size() - colon > 6) return false;
    std::string host = s.substr(0, colon);
    if (!check_ipv4(host).empty()) return false;
    int port = 0;
    for (size_t i = colon + 1; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        port = port * 10 + (s[i] - '0');
    }
    if (port < 1 || port > 65535) return false;
    out.host = host;
    out.port = port;
    return true;
}

std::string usb_text(uint16_t vendor, uint16_t product) {
    char b[16];
    std::snprintf(b, sizeof b, "%04x:%04x", vendor, product);
    return b;
}

std::string check_rotation(long long v) {
    return (v == 0 || v == 90 || v == 180 || v == 270) ? std::string() : std::string("must be 0, 90, 180 or 270");
}

std::string check_mirror(const std::string& v) {
    return (v.empty() || v == "x" || v == "y" || v == "xy") ? std::string() : std::string("must be \"\", \"x\", \"y\" or \"xy\"");
}

std::string check_aspect(double v) {
    if (std::isfinite(v) && (v == 0.0 || (v >= 0.1 && v <= 10.0))) return {};
    return "must be 0 (taken from the device) or between 0.1 and 10";
}

std::string check_calibration(const std::vector<double>& v) {
    bool finite = true;
    for (double x : v) finite = finite && std::isfinite(x) && std::abs(x) <= 100.0;
    if (v.empty() || (v.size() == 6 && finite)) return {};
    return "must be six numbers (an affine map, each at most 100), or none";
}

std::string check_silence(long long v) {
    return (v == 0 || (v >= 50 && v <= 10000)) ? std::string() : std::string("must be 0 (off) or between 50 and 10000");
}

std::string check_usb(const std::string& v, uint16_t* vendor, uint16_t* product) {
    if (v.empty()) return {};
    uint16_t a = 0, b = 0;
    if (!parse_usb(v, a, b)) return "must be vendor:product in hex, like \"0712:0009\" (touch-to-osc --list shows it)";
    if (vendor) *vendor = a;
    if (product) *product = b;
    return {};
}

std::string check_text(const std::string& v, size_t max_len) {
    if (v.size() > max_len) return "is too long (at most " + std::to_string(max_len) + " characters)";
    for (unsigned char c : v) {
        if (c < 0x20 || c >= 0x7f) return "may only use printable ASCII characters";
    }
    return {};
}

std::string check_ipv4(const std::string& v) {
    in_addr a{};
    if (inet_pton(AF_INET, v.c_str(), &a) == 1) return {};
    return "'" + v + "' is not an IPv4 address";
}

std::string check_log_level(const std::string& v) {
    log::Level l;
    if (log::parse_level(v.c_str(), l)) return {};
    return "must be crit, error, warning, notice, info or debug";
}

std::string check_destination(const std::string& v, HostPort& out) {
    if (!parse_host_port(v, out)) return "must be an IPv4 address and a port, like \"192.168.1.27:9481\"";
    if (out.host == "0.0.0.0") return "must name a host, not 0.0.0.0";
    return {};
}

void set_mirror(DeviceConfig& d, const std::string& m) {
    d.mirror_x = m.find('x') != std::string::npos;
    d.mirror_y = m.find('y') != std::string::npos;
}

std::string DeviceConfig::match_text() const {
    std::string s;
    if (has_usb) s = "usb " + usb_text(vendor, product);
    if (!serial.empty()) s += (s.empty() ? "" : ", ") + std::string("serial ") + serial;
    if (!port.empty()) s += (s.empty() ? "" : ", ") + std::string("port ") + port;
    return s;
}

std::string DeviceConfig::mirror_text() const {
    if (mirror_x && mirror_y) return "xy";
    if (mirror_x) return "x";
    if (mirror_y) return "y";
    return "";
}

std::string DeviceConfig::usb_setting() const { return has_usb ? usb_text(vendor, product) : std::string(); }

Result<Config> parse_config(const std::string& toml_text, const std::string& source_name) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_text, source_name);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << source_name << ":" << e.source().begin.line << ":" << e.source().begin.column << ": " << e.description();
        return err(os.str());
    }
    Config c;
    StrictTable root(&tbl, "");
    section(root, "osc", [&](StrictTable& o) {
        host_port(o, "listen", c.osc.listen, true);
        o.str("prefix", c.osc.prefix, 64, false);
        if (o.ok() && (!osc::valid_address(c.osc.prefix) || c.osc.prefix.back() == '/'))
            o.fail("prefix", "must look like \"/touch\": a slash, then letters, without a slash at the end");
        o.strings("allowed_ips", c.osc.allowed_ips, kMaxAllowedIps, 64);
        for (const auto& ip : c.osc.allowed_ips) {
            if (o.ok()) o.check("allowed_ips", check_ipv4(ip));
        }
        o.boolean("open_to_any", c.osc.open_to_any);
    });
    section(root, "log", [&](StrictTable& o) {
        if (o.str("level", c.log.level, 16, false)) o.check("level", check_log_level(c.log.level));
    });
    section(root, "paths", [&](StrictTable& o) {
        if (o.str("settings", c.paths.settings, 256) && !c.paths.settings.empty() && c.paths.settings.front() != '/')
            o.fail("settings", "must be an absolute path, or \"\" to keep changes made over OSC only until a restart");
    });
    table_list(root, "destination", kMaxDestinations, [&](StrictTable& o) {
        DestinationConfig d;
        host_port(o, "address", d.address, false);
        if (o.ok() && d.address.port == 0) o.fail("address", "is missing: give the receiver as \"IP:PORT\"");
        o.boolean("touches", d.touches);
        o.boolean("status", d.status);
        if (o.ok() && !d.touches && !d.status) o.fail("", "sends nothing: set touches or status (or both) to true");
        c.destinations.push_back(d);
    });
    table_list(root, "device", kMaxDevices, [&](StrictTable& o) {
        DeviceConfig d;
        device_table(o, d);
        c.devices.push_back(d);
    });
    root.finish();
    if (!root.ok()) return err(source_name + ": " + root.error());
    if (c.osc.allowed_ips.empty() && !c.osc.open_to_any)
        return err(source_name + ": osc.allowed_ips is empty: list the hosts that may subscribe and send commands (\"127.0.0.1\" for this machine), or set open_to_any = true");
    if (c.devices.empty()) return err(source_name + ": there is no [[device]]: add one for each touchscreen (see config.example.toml)");
    std::set<std::string> ids;
    for (const auto& d : c.devices) {
        if (!ids.insert(d.id).second) return err(source_name + ": two devices are called \"" + d.id + "\": every device id must be different");
    }
    std::set<std::string> dests;
    for (const auto& d : c.destinations) {
        if (!dests.insert(d.address.text()).second) return err(source_name + ": destination " + d.address.text() + " is listed twice");
    }
    return c;
}

Result<Config> load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) return err(path + ": cannot be read (check the path, and that the file is readable)");
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_config(ss.str(), path);
}

}

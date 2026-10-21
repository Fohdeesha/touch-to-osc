#include "config/config.h"

#include "core/log.h"
#include "osc/osc.h"

#include <toml++/toml.hpp>

#include <arpa/inet.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace tto {

namespace {

class StrictTable {
public:
    StrictTable(const toml::node* n, std::string path) : t_(n ? n->as_table() : nullptr), path_(std::move(path)) {
        if (!t_) fail("", "must be a table");
    }

    const toml::node* take(const char* key) {
        if (!t_) return nullptr;
        const toml::node* v = t_->get(key);
        if (!v) return nullptr;
        used_.insert(key);
        return v;
    }

    void str(const char* key, std::string& out, size_t max_len, bool allow_empty = true) {
        const toml::node* v = take(key);
        if (!v) return;
        if (!v->is_string()) return fail(key, "must be a string");
        std::string s(v->as_string()->get());
        if (s.size() > max_len) return fail(key, "is too long (at most " + std::to_string(max_len) + " characters)");
        if (!allow_empty && s.empty()) return fail(key, "must not be empty");
        for (unsigned char c : s) {
            if (c < 0x20 || c == 0x7f) return fail(key, "contains control characters");
        }
        out = std::move(s);
    }

    void boolean(const char* key, bool& out) {
        const toml::node* v = take(key);
        if (!v) return;
        if (!v->is_boolean()) return fail(key, "must be true or false");
        out = v->as_boolean()->get();
    }

    template <typename I>
    void integer(const char* key, I& out, long long lo, long long hi) {
        const toml::node* v = take(key);
        if (!v) return;
        long long n = 0;
        if (v->is_integer()) {
            n = v->as_integer()->get();
        } else if (v->is_floating_point()) {
            double d = v->as_floating_point()->get();
            if (!std::isfinite(d) || d != std::floor(d)) return fail(key, "must be a whole number");
            n = (long long)d;
        } else {
            return fail(key, "must be a number");
        }
        if (n < lo || n > hi) return fail(key, "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        out = I(n);
    }

    void number(const char* key, double& out, double lo, double hi) {
        const toml::node* v = take(key);
        if (!v) return;
        double d = 0;
        if (v->is_integer()) d = double(v->as_integer()->get());
        else if (v->is_floating_point()) d = v->as_floating_point()->get();
        else return fail(key, "must be a number");
        if (!std::isfinite(d) || d < lo || d > hi) return fail(key, "must be between " + fmt(lo) + " and " + fmt(hi));
        out = d;
    }

    void fail(const std::string& key, const std::string& what) {
        if (!error_.empty()) return;
        std::string where = path_;
        if (!key.empty()) where += where.empty() ? key : "." + key;
        error_ = (where.empty() ? std::string("the file") : where) + ": " + what;
    }

    void finish() {
        if (!error_.empty() || !t_) return;
        for (auto&& [k, v] : *t_) {
            (void)v;
            std::string key(k.str());
            if (!used_.count(key)) return fail(key, "unknown key");
        }
    }

    void adopt(const std::string& e) {
        if (error_.empty()) error_ = e;
    }

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

private:
    static std::string fmt(double d) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", d);
        return b;
    }
    const toml::table* t_;
    std::string path_;
    std::set<std::string> used_;
    std::string error_;
};

bool valid_ipv4(const std::string& s) {
    in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

void string_list(StrictTable& o, const char* key, std::vector<std::string>& out, size_t max_items, size_t max_len) {
    const toml::node* v = o.take(key);
    if (!v) return;
    if (!v->is_array()) return o.fail(key, "must be a list of strings");
    const toml::array& a = *v->as_array();
    if (a.size() > max_items) return o.fail(key, "has too many entries (at most " + std::to_string(max_items) + ")");
    std::vector<std::string> items;
    for (auto&& e : a) {
        if (!e.is_string() || e.as_string()->get().empty() || e.as_string()->get().size() > max_len)
            return o.fail(key, "must be a list of non-empty strings");
        items.emplace_back(e.as_string()->get());
    }
    out = std::move(items);
}

void host_port(StrictTable& o, const char* key, HostPort& out, bool wildcard_ok) {
    std::string s;
    o.str(key, s, 64, false);
    if (!o.ok() || s.empty()) return;
    HostPort hp;
    if (!parse_host_port(s, hp)) return o.fail(key, "must be an IPv4 address and a port, like \"192.168.1.27:9481\"");
    if (!wildcard_ok && hp.host == "0.0.0.0") return o.fail(key, "must name a host, not 0.0.0.0");
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
    if (!v->is_array_of_tables()) return root.fail(name, "must be written as [[" + std::string(name) + "]] tables");
    const toml::array& a = *v->as_array();
    if (a.size() > max_items) return root.fail(name, "at most " + std::to_string(max_items) + " allowed");
    size_t index = 0;
    for (auto&& e : a) {
        ++index;
        StrictTable o(&e, std::string(name) + "[" + std::to_string(index) + "]");
        if (o.ok()) fn(o);
        o.finish();
        if (!o.ok()) return root.adopt(o.error());
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
    o.str("usb", usb, 16);
    if (o.ok() && !usb.empty()) {
        if (!parse_usb(usb, d.vendor, d.product)) o.fail("usb", "must be vendor:product in hex, like \"0712:0009\" (touch-to-osc --list shows it)");
        d.has_usb = true;
    }
    o.str("serial", d.serial, 128);
    o.str("port", d.port, 64);
    if (o.ok() && !d.has_usb && d.port.empty())
        o.fail("", "says nothing about which device to read: set usb (vendor:product) or port (both are shown by touch-to-osc --list)");
    o.boolean("grab", d.grab);
    o.integer("rotation", d.rotation, 0, 359);
    if (o.ok() && d.rotation % 90 != 0) o.fail("rotation", "must be 0, 90, 180 or 270");
    std::string mirror;
    o.str("mirror", mirror, 4);
    if (o.ok()) {
        if (mirror == "x") d.mirror_x = true;
        else if (mirror == "y") d.mirror_y = true;
        else if (mirror == "xy") d.mirror_x = d.mirror_y = true;
        else if (!mirror.empty()) o.fail("mirror", "must be \"\", \"x\", \"y\" or \"xy\"");
    }
    o.number("aspect", d.aspect, 0.0, 10.0);
    if (o.ok() && d.aspect != 0.0 && d.aspect < 0.1) o.fail("aspect", "must be 0 (taken from the device) or between 0.1 and 10");
    if (const toml::node* cal = o.take("calibration")) {
        if (!cal->is_array()) {
            o.fail("calibration", "must be a list of six numbers, or [] for none");
        } else {
            std::vector<double> c;
            for (auto&& e : *cal->as_array()) {
                if (e.is_integer()) c.push_back(double(e.as_integer()->get()));
                else if (e.is_floating_point() && std::isfinite(e.as_floating_point()->get())) c.push_back(e.as_floating_point()->get());
                else c.push_back(NAN);
            }
            bool finite = true;
            for (double x : c) finite = finite && std::isfinite(x) && std::abs(x) <= 100.0;
            if (!c.empty() && (c.size() != 6 || !finite)) o.fail("calibration", "must be a list of six numbers (an affine map, each at most 100), or [] for none");
            else d.calibration = c;
        }
    }
    o.integer("silence_ms", d.silence_ms, 0, 10000);
    if (o.ok() && d.silence_ms != 0 && d.silence_ms < 50) o.fail("silence_ms", "must be 0 (off) or between 50 and 10000");
    o.boolean("enabled", d.enabled);
    if (o.ok() && d.label.empty()) d.label = d.id;
}

}

bool parse_host_port(const std::string& s, HostPort& out) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size() || s.size() - colon > 6) return false;
    std::string host = s.substr(0, colon);
    if (!valid_ipv4(host)) return false;
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
        string_list(o, "allowed_ips", c.osc.allowed_ips, 64, 64);
        for (const auto& ip : c.osc.allowed_ips) {
            if (o.ok() && !valid_ipv4(ip)) o.fail("allowed_ips", "'" + ip + "' is not an IPv4 address");
        }
        o.boolean("open_to_any", c.osc.open_to_any);
    });
    section(root, "log", [&](StrictTable& o) {
        o.str("level", c.log.level, 16, false);
        log::Level l;
        if (o.ok() && !log::parse_level(c.log.level.c_str(), l)) o.fail("level", "must be crit, error, warning, notice, info or debug");
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
        std::string key = d.address.host + ":" + std::to_string(d.address.port);
        if (!dests.insert(key).second) return err(source_name + ": destination " + key + " is listed twice");
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

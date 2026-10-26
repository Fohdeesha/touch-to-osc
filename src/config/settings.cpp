#include "config/settings.h"

#include "config/atomic_file.h"
#include "config/strict_toml.h"
#include "core/process.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace tto {

namespace {

template <typename F, typename... X>
void each(F&& f, X&... x) {
    f("label", x.label...);
    f("enabled", x.enabled...);
    f("rotation", x.rotation...);
    f("mirror", x.mirror...);
    f("calibration", x.calibration...);
    f("aspect", x.aspect...);
    f("silence_ms", x.silence_ms...);
    f("grab", x.grab...);
    f("match_usb", x.match_usb...);
    f("match_serial", x.match_serial...);
    f("match_port", x.match_port...);
    f("usb_recovery", x.usb_recovery...);
}

void put(toml::table& t, const char* k, const std::string& v) { t.insert(k, v); }
void put(toml::table& t, const char* k, bool v) { t.insert(k, v); }
void put(toml::table& t, const char* k, int v) { t.insert(k, int64_t(v)); }
void put(toml::table& t, const char* k, double v) { t.insert(k, v); }
void put(toml::table& t, const char* k, const std::vector<double>& v) {
    toml::array a;
    for (double x : v) a.push_back(x);
    t.insert(k, std::move(a));
}

std::string value_text(const std::string& v) { return "\"" + v + "\""; }
std::string value_text(bool v) { return v ? "true" : "false"; }
std::string value_text(int v) { return std::to_string(v); }
std::string value_text(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}
std::string value_text(const std::vector<double>& v) {
    std::string s = "[";
    for (double x : v) s += (s.size() > 1 ? ", " : "") + value_text(x);
    return s + "]";
}

toml::table device_table(const DeviceOverrides& x) {
    toml::table d;
    each(
        [&](const char* k, const auto& v) {
            if (v) put(d, k, *v);
        },
        x);
    return d;
}

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

void parse_device(StrictTable& d, DeviceOverrides& x) {
    std::string s;
    if (d.str("label", s, 64)) x.label = s;
    bool b = false;
    if (d.boolean("enabled", b)) x.enabled = b;
    long long n = 0;
    if (d.integer("rotation", n, -100000, 100000) && d.check("rotation", check_rotation(n))) x.rotation = int(n);
    if (d.str("mirror", s, 4) && d.check("mirror", check_mirror(s))) x.mirror = s;
    std::vector<double> cal;
    if (d.numbers("calibration", cal) && d.check("calibration", check_calibration(cal))) x.calibration = cal;
    double a = 0;
    if (d.number("aspect", a) && d.check("aspect", check_aspect(a))) x.aspect = a;
    if (d.integer("silence_ms", n, -100000, 100000) && d.check("silence_ms", check_silence(n))) x.silence_ms = int(n);
    if (d.boolean("grab", b)) x.grab = b;
    if (d.str("match_usb", s, 16) && d.check("match_usb", check_usb(s, nullptr, nullptr))) x.match_usb = s;
    if (d.str("match_serial", s, 128)) x.match_serial = s;
    if (d.str("match_port", s, 64)) x.match_port = s;
    if (d.boolean("usb_recovery", b)) x.usb_recovery = b;
}

bool parse_destinations(StrictTable& root, const char* key, std::vector<DestinationConfig>& out) {
    const toml::node* v = root.take(key);
    if (!v) return false;
    if (!v->is_array()) return root.fail(key, "must be a list of tables");
    if (v->as_array()->size() > kMaxDestinations) return root.fail(key, "at most " + std::to_string(kMaxDestinations) + " allowed");
    std::vector<DestinationConfig> list;
    std::set<std::string> seen;
    size_t index = 0;
    for (auto&& e : *v->as_array()) {
        ++index;
        StrictTable d(&e, std::string(key) + "[" + std::to_string(index) + "]");
        DestinationConfig dc;
        std::string address;
        if (d.str("address", address, 64, false)) d.check("address", check_destination(address, dc.address));
        if (d.ok() && address.empty()) d.fail("address", "is missing");
        d.boolean("touches", dc.touches);
        d.boolean("status", dc.status);
        d.boolean("plain", dc.plain);
        if (d.ok() && !dc.touches && !dc.status) d.fail("", "sends nothing");
        if (d.ok() && !seen.insert(dc.address.text()).second) d.fail("address", dc.address.text() + " is listed twice");
        d.finish();
        if (!d.ok()) {
            root.adopt(d.error());
            return false;
        }
        list.push_back(dc);
    }
    out = std::move(list);
    return root.ok();
}

bool parse_ips(StrictTable& root, const char* key, std::vector<std::string>& out) {
    std::vector<std::string> ips;
    if (!root.strings(key, ips, kMaxAllowedIps, 64)) return false;
    for (const auto& ip : ips) {
        if (!root.check(key, check_ipv4(ip))) return false;
    }
    out = std::move(ips);
    return true;
}

void parse_config_values(const toml::node* v, Overrides& out) {
    Overrides vals;
    StrictTable t(v, "config_values");
    std::string level;
    if (t.str("log_level", level, 16, false) && t.check("log_level", check_log_level(level))) vals.log_level = level;
    if (const toml::node* devs = t.take("device"); devs && devs->is_table()) {
        for (auto&& [k, node] : *devs->as_table()) {
            std::string id(k.str());
            StrictTable d(&node, "config_values.device." + id);
            DeviceOverrides x;
            if (d.ok()) parse_device(d, x);
            d.finish();
            if (d.ok() && !x.empty()) vals.devices[id] = x;
        }
    }
    t.finish();
    if (t.ok()) out = std::move(vals);
}

}

bool DeviceOverrides::empty() const {
    bool any = false;
    each([&](const char*, const auto& v) { any = any || v.has_value(); }, *this);
    return !any;
}

bool Overrides::empty() const {
    return !log_level && !listen && !prefix && devices_added.empty() && devices_removed.empty() && allowed_added.empty() && allowed_removed.empty() &&
           destinations_added.empty() && destinations_removed.empty() && devices.empty() && unused_devices.empty();
}

std::string destination_text(const DestinationConfig& d) {
    std::string s = d.address.text();
    if (d.touches) s += " touches";
    if (d.status) s += " status";
    if (d.plain) s += " plain";
    return s;
}

DeviceOverrides device_values(const DeviceConfig& d) {
    DeviceOverrides v;
    v.label = d.label;
    v.enabled = d.enabled;
    v.rotation = d.rotation;
    v.mirror = d.mirror_text();
    v.calibration = d.calibration;
    v.aspect = d.aspect;
    v.silence_ms = d.silence_ms;
    v.grab = d.grab;
    v.match_usb = d.usb_setting();
    v.match_serial = d.serial;
    v.match_port = d.port;
    v.usb_recovery = d.usb_recovery;
    return v;
}

Config apply_overrides(const Config& base, const Overrides& o) {
    Config c = base;
    if (o.log_level) c.log.level = *o.log_level;
    if (o.listen) c.osc.listen = *o.listen;
    if (o.prefix) c.osc.prefix = *o.prefix;
    c.devices.erase(std::remove_if(c.devices.begin(), c.devices.end(), [&](const DeviceConfig& d) { return has(o.devices_removed, d.id); }), c.devices.end());
    for (const auto& d : o.devices_added) {
        bool known = std::any_of(c.devices.begin(), c.devices.end(), [&](const DeviceConfig& x) { return x.id == d.id; });
        if (!known && c.devices.size() < kMaxDevices) c.devices.push_back(d);
    }
    std::vector<std::string> ips;
    for (const auto& ip : base.osc.allowed_ips) {
        if (!has(o.allowed_removed, ip)) ips.push_back(ip);
    }
    for (const auto& ip : o.allowed_added) {
        if (!has(ips, ip) && ips.size() < kMaxAllowedIps) ips.push_back(ip);
    }
    c.osc.allowed_ips = ips;
    std::vector<DestinationConfig> dests;
    for (const auto& d : base.destinations) {
        if (!has(o.destinations_removed, d.address.text())) dests.push_back(d);
    }
    for (const auto& d : o.destinations_added) {
        bool listed = std::any_of(dests.begin(), dests.end(), [&](const DestinationConfig& x) { return x.address.text() == d.address.text(); });
        if (!listed && dests.size() < kMaxDestinations) dests.push_back(d);
    }
    c.destinations = dests;
    for (auto& d : c.devices) {
        auto it = o.devices.find(d.id);
        if (it == o.devices.end()) continue;
        const DeviceOverrides& x = it->second;
        if (x.label) d.label = x.label->empty() ? d.id : *x.label;
        if (x.enabled) d.enabled = *x.enabled;
        if (x.rotation) d.rotation = *x.rotation;
        if (x.mirror) set_mirror(d, *x.mirror);
        if (x.calibration) d.calibration = *x.calibration;
        if (x.aspect) d.aspect = *x.aspect;
        if (x.silence_ms) d.silence_ms = *x.silence_ms;
        if (x.grab) d.grab = *x.grab;
        if (x.match_usb) {
            d.has_usb = !x.match_usb->empty();
            d.vendor = d.product = 0;
            if (d.has_usb) check_usb(*x.match_usb, &d.vendor, &d.product);
        }
        if (x.match_serial) d.serial = *x.match_serial;
        if (x.match_port) d.port = *x.match_port;
        if (x.usb_recovery) d.usb_recovery = *x.usb_recovery;
    }
    return c;
}

void drop_unchanged(Overrides& o, const Config& base) {
    if (o.log_level && *o.log_level == base.log.level) o.log_level.reset();
    if (o.listen && o.listen->host == base.osc.listen.host && o.listen->port == base.osc.listen.port) o.listen.reset();
    if (o.prefix && *o.prefix == base.osc.prefix) o.prefix.reset();
    auto in_base = [&](const std::string& id) { return std::any_of(base.devices.begin(), base.devices.end(), [&](const DeviceConfig& d) { return d.id == id; }); };
    o.devices_added.erase(std::remove_if(o.devices_added.begin(), o.devices_added.end(), [&](const DeviceConfig& d) { return in_base(d.id); }), o.devices_added.end());
    o.devices_removed.erase(std::remove_if(o.devices_removed.begin(), o.devices_removed.end(), [&](const std::string& id) { return !in_base(id); }),
                            o.devices_removed.end());
    const auto& allowed = base.osc.allowed_ips;
    std::vector<std::string> added;
    for (const auto& ip : o.allowed_added) {
        if (!has(allowed, ip) && !has(added, ip)) added.push_back(ip);
    }
    o.allowed_added = added;
    o.allowed_removed.erase(std::remove_if(o.allowed_removed.begin(), o.allowed_removed.end(), [&](const std::string& ip) { return !has(allowed, ip); }),
                            o.allowed_removed.end());
    std::vector<DestinationConfig> dests;
    for (const auto& d : o.destinations_added) {
        auto same = std::find(base.destinations.begin(), base.destinations.end(), d);
        if (same != base.destinations.end()) {
            o.destinations_removed.erase(std::remove(o.destinations_removed.begin(), o.destinations_removed.end(), d.address.text()), o.destinations_removed.end());
            continue;
        }
        dests.push_back(d);
    }
    o.destinations_added = dests;
    o.destinations_removed.erase(std::remove_if(o.destinations_removed.begin(), o.destinations_removed.end(),
                                                [&](const std::string& a) {
                                                    return std::none_of(base.destinations.begin(), base.destinations.end(),
                                                                        [&](const DestinationConfig& d) { return d.address.text() == a; });
                                                }),
                                 o.destinations_removed.end());
    for (const auto& d : base.devices) {
        auto it = o.devices.find(d.id);
        if (it == o.devices.end()) continue;
        DeviceOverrides& x = it->second;
        DeviceOverrides now = device_values(d);
        if (x.label && (x.label->empty() ? d.id : *x.label) == d.label) x.label.reset();
        each(
            [](const char* k, auto& over, const auto& cur) {
                if (std::string(k) != "label" && over && cur && *over == *cur) over.reset();
            },
            x, now);
        if (x.empty()) o.devices.erase(it);
    }
}

std::vector<std::string> override_names(const Overrides& o) {
    std::vector<std::string> out;
    if (o.log_level) out.push_back("log_level");
    if (o.listen) out.push_back("listen");
    if (o.prefix) out.push_back("prefix");
    if (!o.devices_added.empty() || !o.devices_removed.empty()) out.push_back("devices");
    if (!o.allowed_added.empty() || !o.allowed_removed.empty()) out.push_back("allowed_ips");
    if (!o.destinations_added.empty() || !o.destinations_removed.empty()) out.push_back("saved_destinations");
    for (const auto& [id, x] : o.devices) {
        std::string b = "device/" + id + "/";
        each(
            [&](const char* k, const auto& v) {
                if (v) out.push_back(b + k);
            },
            x);
    }
    return out;
}

bool forget_override(Overrides& o, const std::string& name) {
    auto reset = [](auto& v) {
        bool had = bool(v);
        v.reset();
        return had;
    };
    if (name == "log_level") return reset(o.log_level);
    if (name == "listen") return reset(o.listen);
    if (name == "prefix") return reset(o.prefix);
    if (name == "devices") {
        bool had = !o.devices_added.empty() || !o.devices_removed.empty();
        o.devices_added.clear();
        o.devices_removed.clear();
        return had;
    }
    if (name == "allowed_ips") {
        bool had = !o.allowed_added.empty() || !o.allowed_removed.empty();
        o.allowed_added.clear();
        o.allowed_removed.clear();
        return had;
    }
    if (name == "saved_destinations") {
        bool had = !o.destinations_added.empty() || !o.destinations_removed.empty();
        o.destinations_added.clear();
        o.destinations_removed.clear();
        return had;
    }
    if (name.rfind("device/", 0) != 0) return false;
    size_t slash = name.find('/', 7);
    std::string id = name.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
    if (slash == std::string::npos) return o.devices.erase(id) + o.unused_devices.erase(id) > 0;
    auto it = o.devices.find(id);
    if (it == o.devices.end()) return false;
    std::string key = name.substr(slash + 1);
    bool had = false;
    each(
        [&](const char* k, auto& v) {
            if (key == k) had = reset(v);
        },
        it->second);
    if (it->second.empty()) o.devices.erase(it);
    return had;
}

void note_config_values(const Overrides& before, const Overrides& after, const Config& file, Overrides& values) {
    if (!after.log_level) values.log_level.reset();
    else if (after.log_level != before.log_level) values.log_level = file.log.level;
    for (auto it = values.devices.begin(); it != values.devices.end();) {
        if (!after.devices.count(it->first) && !after.unused_devices.count(it->first)) it = values.devices.erase(it);
        else ++it;
    }
    for (const auto& [id, x] : after.devices) {
        auto f = std::find_if(file.devices.begin(), file.devices.end(), [&](const DeviceConfig& d) { return d.id == id; });
        DeviceOverrides now = f == file.devices.end() ? DeviceOverrides() : device_values(*f);
        auto b = before.devices.find(id);
        DeviceOverrides was = b == before.devices.end() ? DeviceOverrides() : b->second;
        DeviceOverrides& v = values.devices[id];
        each(
            [](const char*, const auto& over, const auto& old, const auto& file_now, auto& slot) {
                if (!over) slot.reset();
                else if (over != old) slot = file_now;
            },
            x, was, now, v);
        if (v.empty()) values.devices.erase(id);
    }
}

std::string settings_text(const Overrides& o, const Overrides& values, const std::string& config_path) {
    toml::table t;
    if (o.log_level) t.insert("log_level", *o.log_level);
    if (o.listen) t.insert("listen", o.listen->text());
    if (o.prefix) t.insert("prefix", *o.prefix);
    auto strings = [](const std::vector<std::string>& v) {
        toml::array a;
        for (const auto& s : v) a.push_back(s);
        return a;
    };
    if (!o.allowed_added.empty()) t.insert("allowed_ips_added", strings(o.allowed_added));
    if (!o.allowed_removed.empty()) t.insert("allowed_ips_removed", strings(o.allowed_removed));
    if (!o.destinations_added.empty()) {
        toml::array a;
        for (const auto& d : o.destinations_added) {
            toml::table one{{"address", d.address.text()}, {"touches", d.touches}, {"status", d.status}};
            if (d.plain) one.insert("plain", true);
            a.push_back(std::move(one));
        }
        t.insert("saved_destinations_added", std::move(a));
    }
    if (!o.destinations_removed.empty()) t.insert("saved_destinations_removed", strings(o.destinations_removed));
    if (!o.devices_added.empty()) {
        toml::array a;
        for (const auto& d : o.devices_added) a.push_back(toml::table{{"id", d.id}, {"match_usb", d.usb_setting()}, {"match_port", d.port}});
        t.insert("devices_added", std::move(a));
    }
    if (!o.devices_removed.empty()) t.insert("devices_removed", strings(o.devices_removed));
    if (!o.devices.empty() || !o.unused_devices.empty()) {
        toml::table devices;
        for (const auto& [id, x] : o.devices) devices.insert(id, device_table(x));
        for (const auto& [id, x] : o.unused_devices) devices.insert(id, device_table(x));
        t.insert("device", std::move(devices));
    }
    if (values.log_level || !values.devices.empty()) {
        toml::table v;
        if (values.log_level) v.insert("log_level", *values.log_level);
        if (!values.devices.empty()) {
            toml::table devices;
            for (const auto& [id, x] : values.devices) devices.insert(id, device_table(x));
            v.insert("device", std::move(devices));
        }
        t.insert("config_values", std::move(v));
    }
    std::ostringstream os;
    os << "# Written by touch-to-osc: the settings changed over OSC, on top of " << config_path << ".\n"
       << "# They win over that file, unless the file's own value was changed after the setting was saved here\n"
       << "# (config_values says what the file said then). Send <prefix>/settings/forget to drop them,\n"
       << "# or delete this file and restart.\n\n"
       << t << "\n";
    return os.str();
}

Result<SettingsLoad> parse_settings(const std::string& text, const std::string& source, const Config& base) {
    toml::table tbl;
    try {
        tbl = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << source << ":" << e.source().begin.line << ":" << e.source().begin.column << ": " << e.description();
        return err(os.str());
    }
    SettingsLoad out;
    Overrides& o = out.overrides;
    StrictTable root(&tbl, "");
    std::string level;
    if (root.str("log_level", level, 16, false) && root.check("log_level", check_log_level(level))) o.log_level = level;
    std::string given;
    HostPort listen;
    if (root.str("listen", given, 64, false) && root.check("listen", check_listen(given, listen))) o.listen = listen;
    if (root.str("prefix", given, 64, false) && root.check("prefix", check_prefix(given))) o.prefix = given;
    if (const toml::node* v = root.take("devices_added")) {
        if (!v->is_array_of_tables() || v->as_array()->size() > kMaxDevices) {
            root.fail("devices_added", "must be a list of at most " + std::to_string(kMaxDevices) + " tables");
        } else {
            size_t index = 0;
            for (auto&& e : *v->as_array()) {
                StrictTable d(&e, "devices_added[" + std::to_string(++index) + "]");
                DeviceConfig dc;
                std::string usb;
                if (d.str("id", dc.id, 32, false)) d.check("id", check_device_id(dc.id));
                if (d.str("match_usb", usb, 16) && !usb.empty() && d.check("match_usb", check_usb(usb, &dc.vendor, &dc.product))) dc.has_usb = true;
                d.str("match_port", dc.port, 64);
                if (d.ok() && !dc.has_usb && dc.port.empty()) d.fail("", "matches nothing: it needs match_usb or match_port");
                dc.label = dc.id;
                d.finish();
                if (!d.ok()) {
                    root.adopt(d.error());
                    break;
                }
                if (std::any_of(o.devices_added.begin(), o.devices_added.end(), [&](const DeviceConfig& x) { return x.id == dc.id; })) {
                    root.fail("devices_added", "\"" + dc.id + "\" is listed twice");
                    break;
                }
                if (std::any_of(base.devices.begin(), base.devices.end(), [&](const DeviceConfig& x) { return x.id == dc.id; })) {
                    out.notes.push_back("device \"" + dc.id + "\" was added over OSC and the config file has one by that id now: the config file's applies");
                    out.rewrite = true;
                    continue;
                }
                o.devices_added.push_back(dc);
            }
        }
    }
    std::vector<std::string> removed;
    if (root.strings("devices_removed", removed, kMaxDevices, 32)) {
        for (const auto& id : removed) {
            if (std::any_of(base.devices.begin(), base.devices.end(), [&](const DeviceConfig& x) { return x.id == id; })) o.devices_removed.push_back(id);
            else out.rewrite = true;
        }
    }
    parse_ips(root, "allowed_ips_added", o.allowed_added);
    parse_ips(root, "allowed_ips_removed", o.allowed_removed);
    std::vector<std::string> whole;
    if (parse_ips(root, "allowed_ips", whole)) {
        if (whole.empty() && !base.osc.open_to_any) {
            root.fail("allowed_ips", "is empty, which would lock out every host");
        } else {
            for (const auto& ip : whole) {
                if (!has(base.osc.allowed_ips, ip)) o.allowed_added.push_back(ip);
            }
            for (const auto& ip : base.osc.allowed_ips) {
                if (!has(whole, ip)) o.allowed_removed.push_back(ip);
            }
            out.rewrite = true;
        }
    }
    parse_destinations(root, "saved_destinations_added", o.destinations_added);
    std::vector<std::string> gone;
    if (root.strings("saved_destinations_removed", gone, kMaxDestinations, 64)) {
        for (const auto& a : gone) {
            HostPort hp;
            if (!root.check("saved_destinations_removed", check_destination(a, hp))) break;
            o.destinations_removed.push_back(hp.text());
        }
    }
    std::vector<DestinationConfig> list;
    if (parse_destinations(root, "saved_destinations", list)) {
        for (const auto& d : list) {
            if (std::find(base.destinations.begin(), base.destinations.end(), d) == base.destinations.end()) o.destinations_added.push_back(d);
        }
        for (const auto& d : base.destinations) {
            if (std::find(list.begin(), list.end(), d) == list.end()) o.destinations_removed.push_back(d.address.text());
        }
        out.rewrite = true;
    }
    if (const toml::node* v = root.take("device")) {
        if (!v->is_table()) {
            root.fail("device", "must be a table of devices");
        } else {
            for (auto&& [k, node] : *v->as_table()) {
                std::string id(k.str());
                bool known = std::any_of(base.devices.begin(), base.devices.end(), [&](const DeviceConfig& d) { return d.id == id; }) ||
                             std::any_of(o.devices_added.begin(), o.devices_added.end(), [&](const DeviceConfig& d) { return d.id == id; });
                StrictTable d(&node, "device." + id);
                DeviceOverrides x;
                if (d.ok()) parse_device(d, x);
                d.finish();
                if (!d.ok()) {
                    root.adopt(d.error());
                    break;
                }
                if (x.empty()) continue;
                if (known) {
                    o.devices[id] = x;
                } else {
                    o.unused_devices[id] = x;
                    out.notes.push_back("saved settings for device \"" + id + "\" are kept but not used: no [[device]] in the config has that id now; put the id back, or send settings/forget device/" + id + " to drop them");
                }
            }
        }
    }
    if (const toml::node* v = root.take("config_values")) parse_config_values(v, out.config_values);
    root.finish();
    if (!root.ok()) return err(source + ": " + root.error());
    Config eff = apply_overrides(base, o);
    if (eff.devices.empty()) return err(source + ": with these settings no device would be left");
    for (const auto& d : eff.devices) {
        if (!d.has_usb && d.port.empty()) return err(source + ": device." + d.id + " would match nothing: it has neither usb nor port");
    }
    if (!eff.osc.open_to_any && eff.osc.allowed_ips.empty()) return err(source + ": with these settings no host would be allowed, which would lock out every host");
    if (o.log_level && out.config_values.log_level && base.log.level != *out.config_values.log_level) {
        out.notes.push_back("log_level: the config file changed from " + value_text(*out.config_values.log_level) + " to " + value_text(base.log.level) + " after " +
                            value_text(*o.log_level) + " was saved over OSC, so the config file's " + value_text(base.log.level) + " applies now");
        o.log_level.reset();
        out.config_values.log_level.reset();
        out.rewrite = true;
    }
    for (const auto& d : base.devices) {
        auto it = o.devices.find(d.id);
        auto vit = out.config_values.devices.find(d.id);
        if (it == o.devices.end() || vit == out.config_values.devices.end()) continue;
        DeviceOverrides now = device_values(d);
        each(
            [&](const char* k, auto& over, const auto& was, const auto& file_now) {
                if (!over || !was || !file_now || *was == *file_now) return;
                out.notes.push_back("device/" + d.id + "/" + k + ": the config file changed from " + value_text(*was) + " to " + value_text(*file_now) + " after " +
                                    value_text(*over) + " was saved over OSC, so the config file's " + value_text(*file_now) + " applies now");
                over.reset();
                out.rewrite = true;
            },
            it->second, vit->second, now);
    }
    drop_unchanged(o, base);
    note_config_values(o, o, base, out.config_values);
    return out;
}

namespace {

std::vector<std::string> keep_turned_off(const std::string& text, const Config& base, Overrides& o) {
    std::vector<std::string> kept;
    toml::table tbl;
    try {
        tbl = toml::parse(text, std::string("settings"));
    } catch (const toml::parse_error&) {
        return kept;
    }
    const toml::table* devices = tbl["device"].as_table();
    if (!devices) return kept;
    for (const auto& d : base.devices) {
        const toml::node* v = devices->get(d.id);
        const toml::table* t = v ? v->as_table() : nullptr;
        const toml::node* e = t ? t->get("enabled") : nullptr;
        if (e && e->is_boolean() && !e->as_boolean()->get() && d.enabled) {
            o.devices[d.id].enabled = false;
            kept.push_back(d.id);
        }
    }
    return kept;
}

}

SettingsLoad load_settings(const Config& base, bool set_aside_refused) {
    SettingsLoad none;
    const std::string& path = base.paths.settings;
    struct stat st {};
    if (path.empty() || stat(path.c_str(), &st) != 0) return none;
    none.file_exists = true;
    if (access(path.c_str(), R_OK) != 0 && (errno == EACCES || errno == EPERM)) {
        none.no_permission = true;
        none.problem = "the saved settings in " + path + " cannot be read (permission denied): touch-to-osc runs without them; run it as root, as its service does";
        return none;
    }
    auto text = read_file(path, 1 << 20);
    Result<SettingsLoad> parsed = text ? parse_settings(*text, path, base) : Result<SettingsLoad>(err(text.error()));
    if (parsed) {
        parsed->file_exists = true;
        return std::move(*parsed);
    }
    none.problem = (text ? "the settings file was refused: " : "the settings file cannot be read: ") + parsed.error();
    if (text) {
        auto kept = keep_turned_off(*text, base, none.overrides);
        for (const auto& id : kept) none.problem += "; device " + id + " stays turned off as it was saved";
    }
    if (set_aside_refused) {
        std::string aside = path + ".refused";
        if (std::rename(path.c_str(), aside.c_str()) == 0) {
            none.problem += "; it was set aside as " + aside + " and touch-to-osc runs on the config file: set those settings again, or fix " + aside +
                            ", rename it back to " + path + " and restart touch-to-osc";
            none.file_exists = false;
        } else {
            none.problem += "; setting it aside failed (" + errno_str() + ") and touch-to-osc runs on the config file";
        }
    }
    return none;
}

}

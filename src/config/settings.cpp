#include "config/settings.h"

#include "config/atomic_file.h"
#include "config/strict_toml.h"
#include "core/process.h"

#include <cerrno>
#include <cstdio>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace tto {

namespace {

const char* const kDeviceKeys[] = {"label", "enabled", "rotation", "mirror", "calibration", "aspect", "silence_ms", "grab", "match_usb", "match_serial", "match_port"};

template <typename T>
void drop_if(std::optional<T>& v, const T& base) {
    if (v && *v == base) v.reset();
}

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
}

}

bool DeviceOverrides::empty() const {
    return !label && !enabled && !rotation && !mirror && !calibration && !aspect && !silence_ms && !grab && !match_usb && !match_serial && !match_port;
}

bool Overrides::empty() const { return !log_level && !allowed_ips && !saved_destinations && devices.empty(); }

std::string destination_text(const DestinationConfig& d) {
    std::string s = d.address.text();
    if (d.touches) s += " touches";
    if (d.status) s += " status";
    return s;
}

Config apply_overrides(const Config& base, const Overrides& o) {
    Config c = base;
    if (o.log_level) c.log.level = *o.log_level;
    if (o.allowed_ips) c.osc.allowed_ips = *o.allowed_ips;
    if (o.saved_destinations) c.destinations = *o.saved_destinations;
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
    }
    return c;
}

void drop_unchanged(Overrides& o, const Config& base) {
    drop_if(o.log_level, base.log.level);
    drop_if(o.allowed_ips, base.osc.allowed_ips);
    drop_if(o.saved_destinations, base.destinations);
    for (const auto& d : base.devices) {
        auto it = o.devices.find(d.id);
        if (it == o.devices.end()) continue;
        DeviceOverrides& x = it->second;
        if (x.label && (x.label->empty() ? d.id : *x.label) == d.label) x.label.reset();
        drop_if(x.enabled, d.enabled);
        drop_if(x.rotation, d.rotation);
        drop_if(x.mirror, d.mirror_text());
        drop_if(x.calibration, d.calibration);
        drop_if(x.aspect, d.aspect);
        drop_if(x.silence_ms, d.silence_ms);
        drop_if(x.grab, d.grab);
        drop_if(x.match_usb, d.usb_setting());
        drop_if(x.match_serial, d.serial);
        drop_if(x.match_port, d.port);
        if (x.empty()) o.devices.erase(it);
    }
}

std::vector<std::string> override_names(const Overrides& o) {
    std::vector<std::string> out;
    if (o.log_level) out.push_back("log_level");
    if (o.allowed_ips) out.push_back("allowed_ips");
    if (o.saved_destinations) out.push_back("saved_destinations");
    for (const auto& [id, x] : o.devices) {
        std::string b = "device/" + id + "/";
        bool set[] = {bool(x.label), bool(x.enabled), bool(x.rotation), bool(x.mirror), bool(x.calibration), bool(x.aspect),
                      bool(x.silence_ms), bool(x.grab), bool(x.match_usb), bool(x.match_serial), bool(x.match_port)};
        for (size_t i = 0; i < sizeof set / sizeof set[0]; ++i) {
            if (set[i]) out.push_back(b + kDeviceKeys[i]);
        }
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
    if (name == "allowed_ips") return reset(o.allowed_ips);
    if (name == "saved_destinations") return reset(o.saved_destinations);
    if (name.rfind("device/", 0) != 0) return false;
    size_t slash = name.find('/', 7);
    if (slash == std::string::npos) return false;
    auto it = o.devices.find(name.substr(7, slash - 7));
    if (it == o.devices.end()) return false;
    std::string key = name.substr(slash + 1);
    DeviceOverrides& x = it->second;
    bool had = false;
    if (key == "label") had = reset(x.label);
    else if (key == "enabled") had = reset(x.enabled);
    else if (key == "rotation") had = reset(x.rotation);
    else if (key == "mirror") had = reset(x.mirror);
    else if (key == "calibration") had = reset(x.calibration);
    else if (key == "aspect") had = reset(x.aspect);
    else if (key == "silence_ms") had = reset(x.silence_ms);
    else if (key == "grab") had = reset(x.grab);
    else if (key == "match_usb") had = reset(x.match_usb);
    else if (key == "match_serial") had = reset(x.match_serial);
    else if (key == "match_port") had = reset(x.match_port);
    if (x.empty()) o.devices.erase(it);
    return had;
}

std::string settings_text(const Overrides& o, const std::string& config_path) {
    toml::table t;
    if (o.log_level) t.insert("log_level", *o.log_level);
    if (o.allowed_ips) {
        toml::array a;
        for (const auto& ip : *o.allowed_ips) a.push_back(ip);
        t.insert("allowed_ips", std::move(a));
    }
    if (o.saved_destinations) {
        toml::array a;
        for (const auto& d : *o.saved_destinations) a.push_back(toml::table{{"address", d.address.text()}, {"touches", d.touches}, {"status", d.status}});
        t.insert("saved_destinations", std::move(a));
    }
    if (!o.devices.empty()) {
        toml::table devices;
        for (const auto& [id, x] : o.devices) {
            toml::table d;
            if (x.label) d.insert("label", *x.label);
            if (x.enabled) d.insert("enabled", *x.enabled);
            if (x.rotation) d.insert("rotation", int64_t(*x.rotation));
            if (x.mirror) d.insert("mirror", *x.mirror);
            if (x.calibration) {
                toml::array a;
                for (double v : *x.calibration) a.push_back(v);
                d.insert("calibration", std::move(a));
            }
            if (x.aspect) d.insert("aspect", *x.aspect);
            if (x.silence_ms) d.insert("silence_ms", int64_t(*x.silence_ms));
            if (x.grab) d.insert("grab", *x.grab);
            if (x.match_usb) d.insert("match_usb", *x.match_usb);
            if (x.match_serial) d.insert("match_serial", *x.match_serial);
            if (x.match_port) d.insert("match_port", *x.match_port);
            devices.insert(id, std::move(d));
        }
        t.insert("device", std::move(devices));
    }
    std::ostringstream os;
    os << "# Written by touch-to-osc: the settings changed over OSC, on top of " << config_path << ".\n"
       << "# They win over that file. Send <prefix>/settings/forget to drop them (or delete this file and restart).\n\n"
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
    std::vector<std::string> ips;
    if (root.strings("allowed_ips", ips, kMaxAllowedIps, 64)) {
        for (const auto& ip : ips) {
            if (root.ok()) root.check("allowed_ips", check_ipv4(ip));
        }
        if (root.ok() && ips.empty() && !base.osc.open_to_any) root.fail("allowed_ips", "is empty, which would lock out every host");
        if (root.ok()) o.allowed_ips = ips;
    }
    if (const toml::node* v = root.take("saved_destinations")) {
        if (!v->is_array()) {
            root.fail("saved_destinations", "must be a list of tables");
        } else if (v->as_array()->size() > kMaxDestinations) {
            root.fail("saved_destinations", "at most " + std::to_string(kMaxDestinations) + " allowed");
        } else {
            std::vector<DestinationConfig> list;
            std::set<std::string> seen;
            size_t index = 0;
            for (auto&& e : *v->as_array()) {
                ++index;
                StrictTable d(&e, "saved_destinations[" + std::to_string(index) + "]");
                DestinationConfig dc;
                std::string address;
                if (d.str("address", address, 64, false)) d.check("address", check_destination(address, dc.address));
                if (d.ok() && address.empty()) d.fail("address", "is missing");
                d.boolean("touches", dc.touches);
                d.boolean("status", dc.status);
                if (d.ok() && !dc.touches && !dc.status) d.fail("", "sends nothing");
                if (d.ok() && !seen.insert(dc.address.text()).second) d.fail("address", dc.address.text() + " is listed twice");
                d.finish();
                if (!d.ok()) {
                    root.adopt(d.error());
                    break;
                }
                list.push_back(dc);
            }
            if (root.ok()) o.saved_destinations = list;
        }
    }
    if (const toml::node* v = root.take("device")) {
        if (!v->is_table()) {
            root.fail("device", "must be a table of devices");
        } else {
            for (auto&& [k, node] : *v->as_table()) {
                std::string id(k.str());
                bool known = false;
                for (const auto& d : base.devices) known = known || d.id == id;
                if (!known) {
                    out.notes.push_back("saved settings for device \"" + id + "\" were dropped: it is no longer in the config");
                    continue;
                }
                StrictTable d(&node, "device." + id);
                DeviceOverrides x;
                if (d.ok()) parse_device(d, x);
                d.finish();
                if (!d.ok()) {
                    root.adopt(d.error());
                    break;
                }
                if (!x.empty()) o.devices[id] = x;
            }
        }
    }
    root.finish();
    if (!root.ok()) return err(source + ": " + root.error());
    Config eff = apply_overrides(base, o);
    for (const auto& d : eff.devices) {
        if (!d.has_usb && d.port.empty()) return err(source + ": device." + d.id + " would match nothing: it has neither usb nor port");
    }
    drop_unchanged(o, base);
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
            none.problem += "; it was set aside as " + aside + " and touch-to-osc runs on the config file";
            none.file_exists = false;
        } else {
            none.problem += "; setting it aside failed (" + errno_str() + ") and touch-to-osc runs on the config file";
        }
    }
    return none;
}

}

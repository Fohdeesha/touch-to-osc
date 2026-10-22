#pragma once

#include "config/config.h"
#include "core/result.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tto {

struct DeviceOverrides {
    std::optional<std::string> label;
    std::optional<bool> enabled;
    std::optional<int> rotation;
    std::optional<std::string> mirror;
    std::optional<std::vector<double>> calibration;
    std::optional<double> aspect;
    std::optional<int> silence_ms;
    std::optional<bool> grab;
    std::optional<std::string> match_usb;
    std::optional<std::string> match_serial;
    std::optional<std::string> match_port;
    std::optional<bool> usb_recovery;

    bool empty() const;
};

struct Overrides {
    std::optional<std::string> log_level;
    std::optional<std::vector<std::string>> allowed_ips;
    std::optional<std::vector<DestinationConfig>> saved_destinations;
    std::map<std::string, DeviceOverrides> devices;

    bool empty() const;
};

struct SettingsLoad {
    Overrides overrides;
    std::vector<std::string> notes;
    std::string problem;
    bool file_exists = false;
};

Config apply_overrides(const Config& base, const Overrides& o);
void drop_unchanged(Overrides& o, const Config& base);
std::vector<std::string> override_names(const Overrides& o);
bool forget_override(Overrides& o, const std::string& name);
std::string settings_text(const Overrides& o, const std::string& config_path);
Result<SettingsLoad> parse_settings(const std::string& text, const std::string& source, const Config& base);
SettingsLoad load_settings(const Config& base, bool set_aside_refused);
std::string destination_text(const DestinationConfig& d);

}

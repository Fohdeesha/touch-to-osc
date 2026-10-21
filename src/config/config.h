#pragma once

#include "core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tto {

struct HostPort {
    std::string host;
    int port = 0;
};

struct DestinationConfig {
    HostPort address;
    bool touches = true;
    bool status = true;
};

struct DeviceConfig {
    std::string id;
    std::string label;
    bool has_usb = false;
    uint16_t vendor = 0;
    uint16_t product = 0;
    std::string serial;
    std::string port;
    bool grab = true;
    int rotation = 0;
    bool mirror_x = false;
    bool mirror_y = false;
    double aspect = 0;
    std::vector<double> calibration;
    int silence_ms = 200;
    bool enabled = true;

    std::string match_text() const;
    std::string mirror_text() const;
};

struct Config {
    struct Osc {
        HostPort listen{"0.0.0.0", 9480};
        std::string prefix = "/touch";
        std::vector<std::string> allowed_ips;
        bool open_to_any = false;
    } osc;
    struct Log {
        std::string level = "info";
    } log;
    std::vector<DestinationConfig> destinations;
    std::vector<DeviceConfig> devices;
};

constexpr size_t kMaxDevices = 16;
constexpr size_t kMaxDestinations = 16;

Result<Config> parse_config(const std::string& toml_text, const std::string& source_name);
Result<Config> load_config(const std::string& path);

bool parse_host_port(const std::string& s, HostPort& out);
std::string usb_text(uint16_t vendor, uint16_t product);

}

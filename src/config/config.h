#pragma once

#include "core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tto {

struct HostPort {
    std::string host;
    int port = 0;

    std::string text() const { return host + ":" + std::to_string(port); }
};

struct DestinationConfig {
    HostPort address;
    bool touches = true;
    bool status = true;

    bool operator==(const DestinationConfig& o) const {
        return address.host == o.address.host && address.port == o.address.port && touches == o.touches && status == o.status;
    }
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
    std::string usb_setting() const;
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
    struct Paths {
        std::string settings = "/var/lib/touch-to-osc/settings.toml";
    } paths;
    std::vector<DestinationConfig> destinations;
    std::vector<DeviceConfig> devices;
};

constexpr size_t kMaxDevices = 16;
constexpr size_t kMaxDestinations = 16;
constexpr size_t kMaxAllowedIps = 64;

Result<Config> parse_config(const std::string& toml_text, const std::string& source_name);
Result<Config> load_config(const std::string& path);

bool parse_host_port(const std::string& s, HostPort& out);
std::string usb_text(uint16_t vendor, uint16_t product);

std::string check_rotation(long long v);
std::string check_mirror(const std::string& v);
std::string check_aspect(double v);
std::string check_calibration(const std::vector<double>& v);
std::string check_silence(long long v);
std::string check_usb(const std::string& v, uint16_t* vendor, uint16_t* product);
std::string check_text(const std::string& v, size_t max_len);
std::string check_ipv4(const std::string& v);
std::string check_log_level(const std::string& v);
std::string check_destination(const std::string& v, HostPort& out);
void set_mirror(DeviceConfig& d, const std::string& m);

}

#pragma once

#include "config/config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tto {

struct NodeInfo {
    std::string node;
    std::string sysname;
    int number = -1;
    std::string name;
    uint16_t bustype = 0;
    uint16_t vendor = 0;
    uint16_t product = 0;
    std::string phys;
    std::string usb_port;
    std::string usb_serial;
    bool direct = false;
    bool mt_slots = false;
    bool mt_xy = false;
    bool abs_xy = false;
    bool btn_touch = false;
};

enum class TouchKind { none, multitouch, single };

struct SysPaths {
    std::string sys_class_input = "/sys/class/input";
    std::string dev_input = "/dev/input";
};

std::vector<NodeInfo> scan_nodes(const SysPaths& paths = SysPaths());
TouchKind touch_kind(const NodeInfo& n, std::string* why = nullptr);
bool matches(const DeviceConfig& d, const NodeInfo& n);
std::string printable(const std::string& s);
std::string usb_identity(const NodeInfo& n);

}

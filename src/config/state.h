#pragma once

#include <map>
#include <string>

namespace tto {

struct State {
    std::map<std::string, std::string> usb_ports;
    std::string config_path;
    std::string config_text;
    std::string config_saved;
};

State parse_state(const std::string& text, const std::string& source, std::string* problem);
std::string state_text(const State& s);
State load_state(const std::string& path, std::string* problem);
std::string usb_reset_marker(const std::string& state_path);
std::string local_time_text();

}

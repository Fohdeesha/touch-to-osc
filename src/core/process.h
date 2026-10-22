#pragma once

#include <string>

namespace tto {

void set_thread_name(const char* name);

void sd_notify_msg(const char* msg);
bool sd_watchdog_enabled();

std::string self_exe_path();

std::string errno_str(int e);
std::string errno_str();

}

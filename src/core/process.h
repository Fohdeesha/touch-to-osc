#pragma once

#include <csignal>
#include <string>

namespace tto {

sigset_t stop_signals();
const char* stop_signal_name(int signo);

void set_thread_name(const char* name);

void sd_notify_msg(const char* msg, bool may_wait = false);
bool sd_watchdog_enabled();

std::string self_exe_path();

std::string errno_str(int e);
std::string errno_str();

}

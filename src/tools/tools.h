#pragma once

#include "config/config.h"
#include "input/probe.h"

#include <string>
#include <vector>

namespace tto {

int list_devices(const Config* cfg, const SysPaths& paths, bool all);
int dump_device(const Config& cfg, const std::string& id, const SysPaths& paths);
int status_command(const Config& cfg);
int send_command(const Config& cfg, const std::vector<std::string>& words);

}

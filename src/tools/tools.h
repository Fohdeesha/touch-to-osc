#pragma once

#include "config/config.h"
#include "input/probe.h"

#include <string>

namespace tto {

int list_devices(const Config* cfg, const SysPaths& paths);
int dump_device(const Config& cfg, const std::string& id, const SysPaths& paths);

}

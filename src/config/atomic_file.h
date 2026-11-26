#pragma once

#include "core/result.h"

#include <string>

namespace tto {

Status write_file_atomic(const std::string& path, const std::string& contents);
Status remove_file_durable(const std::string& path);
int remove_stale_temp_files(const std::string& path);
Result<std::string> read_file(const std::string& path, size_t max_bytes);

}

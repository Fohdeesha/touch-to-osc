#pragma once

#include <string>
#include <vector>

namespace tto {

std::string closest_name(const std::string& word, const std::vector<std::string>& names);
std::string did_you_mean(const std::string& word, const std::vector<std::string>& names);

}

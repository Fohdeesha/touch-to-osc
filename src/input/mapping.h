#pragma once

#include "config/config.h"

#include <cstdint>

namespace tto {

struct Axis {
    int32_t min = 0;
    int32_t max = 0;
    int32_t resolution = 0;
};

class Mapping {
public:
    Mapping() = default;
    Mapping(const DeviceConfig& cfg, Axis x, Axis y);

    void map(int32_t raw_x, int32_t raw_y, float& x, float& y) const;
    float aspect() const { return aspect_; }
    float width_mm() const { return w_mm_; }
    float height_mm() const { return h_mm_; }

private:
    Axis x_, y_;
    int rotation_ = 0;
    bool mirror_x_ = false;
    bool mirror_y_ = false;
    bool calibrated_ = false;
    double c_[6] = {1, 0, 0, 0, 1, 0};
    float aspect_ = 1.0f;
    float w_mm_ = 0;
    float h_mm_ = 0;
};

}

#include "input/mapping.h"

#include <algorithm>

namespace tto {

namespace {

double unit(int32_t v, const Axis& a) {
    if (a.max <= a.min) return 0.0;
    return (double(v) - double(a.min)) / (double(a.max) - double(a.min));
}

}

Mapping::Mapping(const DeviceConfig& cfg, Axis x, Axis y)
    : x_(x), y_(y), rotation_(cfg.rotation), mirror_x_(cfg.mirror_x), mirror_y_(cfg.mirror_y) {
    if (cfg.calibration.size() == 6) {
        calibrated_ = true;
        std::copy(cfg.calibration.begin(), cfg.calibration.end(), c_);
    }
    double w = double(x.max) - double(x.min), h = double(y.max) - double(y.min);
    if (x.resolution > 0 && y.resolution > 0) {
        w_mm_ = float(w / x.resolution);
        h_mm_ = float(h / y.resolution);
        w = w_mm_;
        h = h_mm_;
    }
    double a = (w > 0 && h > 0) ? w / h : 1.0;
    if (rotation_ == 90 || rotation_ == 270) a = 1.0 / a;
    aspect_ = cfg.aspect > 0 ? float(cfg.aspect) : float(a);
}

void Mapping::map(int32_t raw_x, int32_t raw_y, float& x, float& y) const {
    double u = unit(raw_x, x_), v = unit(raw_y, y_);
    double px, py;
    switch (rotation_) {
    case 90: px = 1.0 - v; py = u; break;
    case 180: px = 1.0 - u; py = 1.0 - v; break;
    case 270: px = v; py = 1.0 - u; break;
    default: px = u; py = v; break;
    }
    if (mirror_x_) px = 1.0 - px;
    if (mirror_y_) py = 1.0 - py;
    if (calibrated_) {
        double cx = c_[0] * px + c_[1] * py + c_[2];
        double cy = c_[3] * px + c_[4] * py + c_[5];
        px = cx;
        py = cy;
    }
    x = float(std::clamp(px, 0.0, 1.0));
    y = float(std::clamp(py, 0.0, 1.0));
}

}

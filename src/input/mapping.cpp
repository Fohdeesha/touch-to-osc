#include "input/mapping.h"

#include <algorithm>
#include <cmath>

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

Result<CalFit> fit_calibration(const std::vector<CalPoint>& p) {
    if (p.size() < 3 || p.size() > 16) return err("calibration/fit takes 3 to 16 points, four numbers each: x y where the target was shown, u v where it was touched");
    double n = double(p.size()), mu = 0, mv = 0, mx = 0, my = 0;
    for (const auto& q : p) {
        mu += q.u;
        mv += q.v;
        mx += q.x;
        my += q.y;
    }
    mu /= n;
    mv /= n;
    mx /= n;
    my /= n;
    double suu = 0, svv = 0, suv = 0, sux = 0, svx = 0, suy = 0, svy = 0;
    for (const auto& q : p) {
        double du = q.u - mu, dv = q.v - mv;
        suu += du * du;
        svv += dv * dv;
        suv += du * dv;
        sux += du * (q.x - mx);
        svx += dv * (q.x - mx);
        suy += du * (q.y - my);
        svy += dv * (q.y - my);
    }
    suu /= n;
    svv /= n;
    suv /= n;
    sux /= n;
    svx /= n;
    suy /= n;
    svy /= n;
    double det = suu * svv - suv * suv;
    double half = (suu + svv) / 2;
    double least = half - std::sqrt(std::max(0.0, half * half - det));
    if (!(least >= 1e-4))
        return err("the touched points lie on one line (or nearly), so they cannot fix a calibration: use targets spread over the screen, at least three not in a row");
    CalFit f;
    double ax = (sux * svv - svx * suv) / det, bx = (svx * suu - sux * suv) / det;
    double ay = (suy * svv - svy * suv) / det, by = (svy * suu - suy * suv) / det;
    f.c = {ax, bx, mx - ax * mu - bx * mv, ay, by, my - ay * mu - by * mv};
    for (size_t i = 0; i < p.size(); ++i) {
        double fx = f.c[0] * p[i].u + f.c[1] * p[i].v + f.c[2];
        double fy = f.c[3] * p[i].u + f.c[4] * p[i].v + f.c[5];
        double miss = std::hypot(fx - p[i].x, fy - p[i].y);
        if (miss > f.worst) {
            f.worst = miss;
            f.worst_at = i;
        }
    }
    return f;
}

void Mapping::oriented(int32_t raw_x, int32_t raw_y, double& px, double& py) const {
    double u = unit(raw_x, x_), v = unit(raw_y, y_);
    switch (rotation_) {
    case 90: px = 1.0 - v; py = u; break;
    case 180: px = 1.0 - u; py = 1.0 - v; break;
    case 270: px = v; py = 1.0 - u; break;
    default: px = u; py = v; break;
    }
    if (mirror_x_) px = 1.0 - px;
    if (mirror_y_) py = 1.0 - py;
}

void Mapping::map_uncalibrated(int32_t raw_x, int32_t raw_y, float& u, float& v) const {
    double px, py;
    oriented(raw_x, raw_y, px, py);
    u = float(px);
    v = float(py);
}

void Mapping::map(int32_t raw_x, int32_t raw_y, float& x, float& y) const {
    double px, py;
    oriented(raw_x, raw_y, px, py);
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

#include "input/mapping.h"

#include <doctest.h>

using namespace tto;

namespace {

struct Pt {
    float x, y;
};

Pt at(const Mapping& m, int rx, int ry) {
    Pt p;
    m.map(rx, ry, p.x, p.y);
    return p;
}

DeviceConfig dev(int rotation, const char* mirror = "") {
    DeviceConfig d;
    d.id = "t";
    d.rotation = rotation;
    std::string m = mirror;
    d.mirror_x = m.find('x') != std::string::npos;
    d.mirror_y = m.find('y') != std::string::npos;
    return d;
}

const Axis kX{0, 1920, 4};
const Axis kY{0, 1080, 4};

}

TEST_CASE("an axis spanning the whole int32 range maps without overflowing") {
    const Axis wide{INT32_MIN, INT32_MAX, 0};
    Mapping m(dev(0), wide, wide);
    Pt mid = at(m, 0, 0);
    CHECK(mid.x == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(mid.y == doctest::Approx(0.5).epsilon(1e-6));
    Pt hi = at(m, INT32_MAX, INT32_MIN);
    CHECK(hi.x == doctest::Approx(1.0));
    CHECK(hi.y == doctest::Approx(0.0));
    CHECK(m.aspect() == doctest::Approx(1.0));
}

namespace {

void corner(const Mapping& m, int rx, int ry, float ex, float ey) {
    Pt p = at(m, rx, ry);
    CHECK(p.x == doctest::Approx(ex));
    CHECK(p.y == doctest::Approx(ey));
}

}

TEST_CASE("rotation 0 maps the panel corners to the picture corners") {
    Mapping m(dev(0), kX, kY);
    corner(m, 0, 0, 0, 0);
    corner(m, 1920, 0, 1, 0);
    corner(m, 1920, 1080, 1, 1);
    corner(m, 0, 1080, 0, 1);
    CHECK(m.aspect() == doctest::Approx(16.0 / 9.0));
    CHECK(m.width_mm() == doctest::Approx(480));
}

TEST_CASE("rotation 90 turns touches clockwise: the panel's top left lands top right") {
    Mapping m(dev(90), kX, kY);
    corner(m, 0, 0, 1, 0);
    corner(m, 1920, 0, 1, 1);
    corner(m, 1920, 1080, 0, 1);
    corner(m, 0, 1080, 0, 0);
    CHECK(m.aspect() == doctest::Approx(9.0 / 16.0));
}

TEST_CASE("rotation 180 and 270") {
    Mapping a(dev(180), kX, kY);
    corner(a, 0, 0, 1, 1);
    corner(a, 1920, 1080, 0, 0);
    Mapping b(dev(270), kX, kY);
    corner(b, 0, 0, 0, 1);
    corner(b, 1920, 0, 0, 0);
}

TEST_CASE("mirror flips the picture position") {
    Mapping x(dev(0, "x"), kX, kY);
    corner(x, 0, 0, 1, 0);
    Mapping y(dev(0, "y"), kX, kY);
    corner(y, 0, 0, 0, 1);
    Mapping xy(dev(90, "xy"), kX, kY);
    corner(xy, 0, 0, 0, 1);
}

TEST_CASE("positions are clamped to 0..1 and a calibration is applied last") {
    Mapping m(dev(0), kX, kY);
    corner(m, -50, 2000, 0, 1);
    DeviceConfig d = dev(0);
    d.calibration = {0.5, 0, 0.25, 0, 0.5, 0.25};
    Mapping c(d, kX, kY);
    corner(c, 960, 540, 0.5f, 0.5f);
    corner(c, 0, 0, 0.25f, 0.25f);
}

TEST_CASE("a configured aspect wins over the device's") {
    DeviceConfig d = dev(0);
    d.aspect = 1.25;
    Mapping m(d, kX, kY);
    CHECK(m.aspect() == doctest::Approx(1.25));
}

TEST_CASE("a calibration fit recovers the numbers that line the touches up, and refuses points it cannot use") {
    const double c[6] = {1.05, 0.02, -0.03, -0.01, 0.97, 0.04};
    std::vector<CalPoint> pts;
    for (double x : {0.1, 0.5, 0.9}) {
        for (double y : {0.1, 0.9}) {
            // where a target shown at x, y was touched, given that the calibration c takes u, v to x, y
            double det = c[0] * c[4] - c[1] * c[3];
            double u = ((x - c[2]) * c[4] - (y - c[5]) * c[1]) / det, v = ((y - c[5]) * c[0] - (x - c[2]) * c[3]) / det;
            pts.push_back({x, y, u, v});
        }
    }
    auto fit = fit_calibration(pts);
    REQUIRE(fit);
    REQUIRE(fit->c.size() == 6);
    for (int i = 0; i < 6; ++i) CHECK(fit->c[size_t(i)] == doctest::Approx(c[i]).epsilon(1e-9));
    CHECK(fit->worst < 1e-9);
    pts[2].x += 0.1;
    auto off = fit_calibration(pts);
    REQUIRE(off);
    CHECK(off->worst > 0.04);
    CHECK(off->worst_at == 2);
    CHECK_FALSE(fit_calibration({{0.1, 0.1, 0.1, 0.1}, {0.9, 0.9, 0.9, 0.9}}));
    auto line = fit_calibration({{0.1, 0.1, 0.1, 0.1}, {0.5, 0.5, 0.5, 0.5}, {0.9, 0.9, 0.9, 0.9}});
    REQUIRE_FALSE(line);
    CHECK(line.error().find("lie on one line") != std::string::npos);
    std::vector<CalPoint> many(17, CalPoint{0.5, 0.5, 0.5, 0.5});
    CHECK_FALSE(fit_calibration(many));
}

TEST_CASE("positions before calibration are turned and mirrored but neither calibrated nor clamped") {
    DeviceConfig d = dev(90, "x");
    d.calibration = {2, 0, 0, 0, 2, 0};
    Mapping m(d, kX, kY);
    float u, v, x, y;
    m.map_uncalibrated(1920, 0, u, v);
    m.map(1920, 0, x, y);
    CHECK(u == doctest::Approx(0.0));
    CHECK(v == doctest::Approx(1.0));
    m.map_uncalibrated(960, 540, u, v);
    m.map(960, 540, x, y);
    CHECK(u == doctest::Approx(0.5));
    CHECK(x == doctest::Approx(1.0));
    Mapping wide(dev(0), Axis{0, 1000, 0}, Axis{0, 1000, 0});
    wide.map_uncalibrated(1500, -500, u, v);
    CHECK(u == doctest::Approx(1.5));
    CHECK(v == doctest::Approx(-0.5));
}

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

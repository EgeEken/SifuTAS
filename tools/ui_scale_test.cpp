#include <cassert>
#include <cmath>
#include <limits>

#include "../src/ui/scale.h"

int main() {
    using sifucoop::ui::UiScaleForViewport;
    assert(UiScaleForViewport(-1.f, -1.f, 1.f) == 0.f);
    assert(UiScaleForViewport(0.f, 1080.f, 1.f) == 0.f);
    assert(UiScaleForViewport(1920.f, 0.f, 1.f) == 0.f);
    assert(UiScaleForViewport(std::numeric_limits<float>::infinity(), 1080.f, 1.f) == 0.f);
    assert(UiScaleForViewport(1920.f, 1080.f, std::numeric_limits<float>::quiet_NaN()) == 0.f);
    assert(std::fabs(UiScaleForViewport(1280.f, 720.f, 1.f) - .8f) < .0001f);
    assert(std::fabs(UiScaleForViewport(1920.f, 1080.f, 1.f) - 1.f) < .0001f);
    assert(UiScaleForViewport(1.f, 1.f, 1.f) >= .1f);
}

#pragma once

#include <algorithm>
#include <cmath>

namespace sifucoop::ui {

inline float UiScaleForViewport(float width, float height, float dpi_scale) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(dpi_scale) ||
        width <= 0.f || height <= 0.f || dpi_scale <= 0.f) return 0.f;
    const float scale = std::min({1.1f, dpi_scale, width / 1600.f, height / 820.f});
    if (!std::isfinite(scale) || scale <= 0.f) return 0.f;
    return std::max(scale, .1f);
}

}

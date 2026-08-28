#include "getnative/filter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace getnative {
namespace {

[[nodiscard]] constexpr double square(double x) noexcept { return x * x; }
[[nodiscard]] constexpr double cube(double x) noexcept { return x * x * x; }

[[nodiscard]] double sinc(double x) noexcept {
    if (x == 0.0) {
        return 1.0;
    }
    const double pix = std::numbers::pi * x;
    return std::sin(pix) / pix;
}

} // namespace

std::int32_t Filter::support() const {
    switch (type) {
    case KernelType::bilinear: return 1;
    case KernelType::bicubic: return 2;
    case KernelType::lanczos:
        if (taps <= 0 || taps >= 16) {
            throw std::invalid_argument("Lanczos taps must be in the zimg-compatible range 1..15");
        }
        return taps;
    case KernelType::spline16: return 2;
    case KernelType::spline36: return 3;
    case KernelType::spline64: return 4;
    }
    throw std::invalid_argument("unknown filter type");
}

std::int32_t Filter::effective_support() const {
    // Relational checks only: std::isfinite folds to a constant under the
    // planner's -ffast-math mode, so non-finite blur must be rejected via
    // comparisons the optimizer cannot assume away.
    if (!(blur > 0.0) || !(blur <= std::numeric_limits<double>::max())) {
        throw std::invalid_argument("blur must be finite and greater than zero");
    }
    constexpr auto maximum_support =
        (std::numeric_limits<std::int32_t>::max() - 1) / 2;
    const std::int32_t base_support = support();
    const double scaled_support = static_cast<double>(base_support) * blur;
    if (!(scaled_support <= static_cast<double>(maximum_support))) {
        throw std::length_error("effective filter support is too large");
    }
    const double effective = std::ceil(scaled_support);
    if (!(effective >= 1.0)
        || !(effective <= static_cast<double>(maximum_support))) {
        throw std::length_error("effective filter support is too large");
    }
    return static_cast<std::int32_t>(effective);
}

double Filter::weight(double distance) const noexcept {
    const double stretched = blur == 1.0 ? distance : distance / blur;
    double x = std::abs(stretched);
    switch (type) {
    case KernelType::bilinear:
        return std::max(1.0 - x, 0.0);
    case KernelType::bicubic:
        if (x < 1.0) {
            return ((12.0 - 9.0 * b - 6.0 * c) * cube(x)
                    + (-18.0 + 12.0 * b + 6.0 * c) * square(x) + (6.0 - 2.0 * b)) / 6.0;
        }
        if (x < 2.0) {
            return ((-b - 6.0 * c) * cube(x) + (6.0 * b + 30.0 * c) * square(x)
                    + (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) / 6.0;
        }
        return 0.0;
    case KernelType::lanczos:
        return taps > 0 && x < static_cast<double>(taps)
            ? sinc(x) * sinc(x / static_cast<double>(taps)) : 0.0;
    case KernelType::spline16:
        if (x < 1.0) return 1.0 - x / 5.0 - 9.0 * square(x) / 5.0 + cube(x);
        if (x < 2.0) { x -= 1.0; return -7.0 * x / 15.0 + 4.0 * square(x) / 5.0 - cube(x) / 3.0; }
        return 0.0;
    case KernelType::spline36:
        if (x < 1.0) return 1.0 - 3.0 * x / 209.0 - 453.0 * square(x) / 209.0 + 13.0 * cube(x) / 11.0;
        if (x < 2.0) { x -= 1.0; return -156.0 * x / 209.0 + 270.0 * square(x) / 209.0 - 6.0 * cube(x) / 11.0; }
        if (x < 3.0) { x -= 2.0; return 26.0 * x / 209.0 - 45.0 * square(x) / 209.0 + cube(x) / 11.0; }
        return 0.0;
    case KernelType::spline64:
        if (x < 1.0) return 1.0 - 3.0 * x / 2911.0 - 6387.0 * square(x) / 2911.0 + 49.0 * cube(x) / 41.0;
        if (x < 2.0) { x -= 1.0; return -2328.0 * x / 2911.0 + 4032.0 * square(x) / 2911.0 - 24.0 * cube(x) / 41.0; }
        if (x < 3.0) { x -= 2.0; return 582.0 * x / 2911.0 - 1008.0 * square(x) / 2911.0 + 6.0 * cube(x) / 41.0; }
        if (x < 4.0) { x -= 3.0; return -97.0 * x / 2911.0 + 168.0 * square(x) / 2911.0 - cube(x) / 41.0; }
        return 0.0;
    }
    return 0.0;
}

} // namespace getnative

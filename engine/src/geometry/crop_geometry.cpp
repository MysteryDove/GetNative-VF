#include "getnative/crop_geometry.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace getnative {
namespace {

void require_finite(double value, const char* name) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string{name} + " must be finite");
    }
}

std::int64_t parity_canvas(std::int64_t base, double active) {
    const auto half_difference = python_int((static_cast<double>(base) - active) / 2.0);
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (half_difference < minimum / 2 || half_difference > maximum / 2) {
        throw std::out_of_range("parity canvas multiplication is outside int64 range");
    }

    const auto doubled = half_difference * 2;
    if ((doubled > 0 && base < minimum + doubled)
        || (doubled < 0 && base > maximum + doubled)) {
        throw std::out_of_range("parity canvas subtraction is outside int64 range");
    }

    const auto canvas = base - doubled;
    if (canvas <= 0) {
        throw std::invalid_argument("canvas dimensions must be positive");
    }
    return canvas;
}

void require_positive_canvas(std::int64_t width, std::int64_t height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("canvas dimensions must be positive");
    }
}

} // namespace

std::int64_t python_round_to_even(double value) {
    require_finite(value, "round value");
    constexpr auto low = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto high = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (value < low || value >= high) {
        throw std::out_of_range("round value is outside int64 range");
    }

    const double lower = std::floor(value);
    const double fraction = value - lower;
    if (fraction < 0.5) {
        return static_cast<std::int64_t>(lower);
    }
    if (fraction > 0.5) {
        return static_cast<std::int64_t>(lower + 1.0);
    }

    const auto lower_integer = static_cast<std::int64_t>(lower);
    return lower_integer % 2 == 0 ? lower_integer : lower_integer + 1;
}

std::int64_t python_int(double value) {
    require_finite(value, "int value");
    constexpr auto low = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto high = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (value < low || value >= high) {
        throw std::out_of_range("int value is outside int64 range");
    }
    return static_cast<std::int64_t>(value);
}

Geometry descale_geometry(
    std::int64_t source_width,
    std::int64_t source_height,
    double active_width,
    double active_height,
    std::optional<std::int64_t> base_height) {
    if (source_width <= 0 || source_height <= 0) {
        throw std::invalid_argument("source dimensions must be positive");
    }
    require_finite(active_width, "active width");
    require_finite(active_height, "active height");
    if (active_width <= 0.0 || active_height <= 0.0) {
        throw std::invalid_argument("active dimensions must be positive");
    }

    if (!base_height) {
        const auto width = python_round_to_even(active_width);
        const auto height = python_round_to_even(active_height);
        require_positive_canvas(width, height);
        return {width, height, 0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
    }
    if (*base_height <= 0) {
        throw std::invalid_argument("base height must be positive");
    }

    const auto base_width = python_round_to_even(
        static_cast<double>(source_width) / static_cast<double>(source_height) *
        static_cast<double>(*base_height));
    const auto width = parity_canvas(base_width, active_width);
    const auto height = parity_canvas(*base_height, active_height);
    return {
        width,
        height,
        (static_cast<double>(width) - active_width) / 2.0,
        (static_cast<double>(height) - active_height) / 2.0,
        active_width,
        active_height,
    };
}

Geometry descale_geometry_pro(
    double active_width,
    double active_height,
    std::optional<std::int64_t> base_height,
    std::optional<std::int64_t> base_width) {
    require_finite(active_width, "active width");
    require_finite(active_height, "active height");
    if (active_width <= 0.0 || active_height <= 0.0) {
        throw std::invalid_argument("active dimensions must be positive");
    }
    if (base_height && *base_height <= 0) {
        throw std::invalid_argument("base height must be positive");
    }
    if (base_width && *base_width <= 0) {
        throw std::invalid_argument("base width must be positive");
    }

    const auto height = base_height ? parity_canvas(*base_height, active_height)
                                    : python_round_to_even(active_height);
    const auto width = base_width ? parity_canvas(*base_width, active_width)
                                  : python_round_to_even(active_width);
    require_positive_canvas(width, height);
    return {
        width,
        height,
        base_width ? (static_cast<double>(width) - active_width) / 2.0 : 0.0,
        base_height ? (static_cast<double>(height) - active_height) / 2.0 : 0.0,
        base_width ? active_width : static_cast<double>(width),
        base_height ? active_height : static_cast<double>(height),
    };
}

} // namespace getnative

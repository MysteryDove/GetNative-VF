#pragma once

#include <cstdint>

namespace getnative {

enum class KernelType : std::uint8_t {
    bilinear,
    bicubic,
    lanczos,
    spline16,
    spline36,
    spline64,
};

// Largest accepted kernel blur. Support, plan size and factorization cost all
// scale with blur, so an unbounded value turns a typo into gigabytes per plan.
inline constexpr double maximum_filter_blur = 16.0;

struct Filter {
    KernelType type = KernelType::bicubic;
    double b = 0.0;
    double c = 0.5;
    std::int32_t taps = 3;
    double blur = 1.0;

    [[nodiscard]] static constexpr Filter bilinear(double blur = 1.0) noexcept {
        return {KernelType::bilinear, 0.0, 0.0, 1, blur};
    }
    [[nodiscard]] static constexpr Filter bicubic(
        double b = 0.0, double c = 0.5, double blur = 1.0) noexcept {
        return {KernelType::bicubic, b, c, 2, blur};
    }
    [[nodiscard]] static constexpr Filter lanczos(
        std::int32_t taps = 3, double blur = 1.0) noexcept {
        return {KernelType::lanczos, 0.0, 0.0, taps, blur};
    }
    [[nodiscard]] static constexpr Filter spline16(double blur = 1.0) noexcept {
        return {KernelType::spline16, 0.0, 0.0, 2, blur};
    }
    [[nodiscard]] static constexpr Filter spline36(double blur = 1.0) noexcept {
        return {KernelType::spline36, 0.0, 0.0, 3, blur};
    }
    [[nodiscard]] static constexpr Filter spline64(double blur = 1.0) noexcept {
        return {KernelType::spline64, 0.0, 0.0, 4, blur};
    }

    [[nodiscard]] std::int32_t support() const;
    [[nodiscard]] std::int32_t effective_support() const;
    [[nodiscard]] double weight(double distance) const noexcept;
};

} // namespace getnative

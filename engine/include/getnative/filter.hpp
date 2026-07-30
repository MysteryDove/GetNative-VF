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

struct Filter {
    KernelType type = KernelType::bicubic;
    double b = 0.0;
    double c = 0.5;
    std::int32_t taps = 3;

    [[nodiscard]] static constexpr Filter bilinear() noexcept {
        return {KernelType::bilinear, 0.0, 0.0, 1};
    }
    [[nodiscard]] static constexpr Filter bicubic(double b = 0.0, double c = 0.5) noexcept {
        return {KernelType::bicubic, b, c, 2};
    }
    [[nodiscard]] static constexpr Filter lanczos(std::int32_t taps = 3) noexcept {
        return {KernelType::lanczos, 0.0, 0.0, taps};
    }
    [[nodiscard]] static constexpr Filter spline16() noexcept {
        return {KernelType::spline16, 0.0, 0.0, 2};
    }
    [[nodiscard]] static constexpr Filter spline36() noexcept {
        return {KernelType::spline36, 0.0, 0.0, 3};
    }
    [[nodiscard]] static constexpr Filter spline64() noexcept {
        return {KernelType::spline64, 0.0, 0.0, 4};
    }

    [[nodiscard]] std::int32_t support() const;
    [[nodiscard]] double weight(double distance) const noexcept;
};

} // namespace getnative

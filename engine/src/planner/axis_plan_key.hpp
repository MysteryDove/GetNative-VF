#pragma once

#include "getnative/axis_plan.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace getnative::detail {

struct PlanKey {
    std::int32_t source_size;
    std::int32_t destination_size;
    std::uint64_t active_length;
    std::uint64_t shift;
    KernelType type;
    std::uint64_t b;
    std::uint64_t c;
    std::int32_t taps;
    std::uint64_t blur;
    BorderMode border;

    friend bool operator==(const PlanKey &, const PlanKey &) = default;
};

struct PlanKeyHash {
    [[nodiscard]] std::size_t operator()(const PlanKey &key) const noexcept {
        std::size_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::uint64_t value) {
            hash ^= static_cast<std::size_t>(value);
            hash *= 1099511628211ULL;
        };
        mix(static_cast<std::uint32_t>(key.source_size));
        mix(static_cast<std::uint32_t>(key.destination_size));
        mix(key.active_length);
        mix(key.shift);
        mix(static_cast<std::uint8_t>(key.type));
        mix(key.b);
        mix(key.c);
        mix(static_cast<std::uint32_t>(key.taps));
        mix(key.blur);
        mix(static_cast<std::uint8_t>(key.border));
        return hash;
    }
};

[[nodiscard]] inline PlanKey plan_key(const AxisPlanRequest &request) noexcept {
    return {
        request.source_size,
        request.destination_size,
        std::bit_cast<std::uint64_t>(request.active_length),
        std::bit_cast<std::uint64_t>(request.shift),
        request.filter.type,
        std::bit_cast<std::uint64_t>(request.filter.b),
        std::bit_cast<std::uint64_t>(request.filter.c),
        request.filter.taps,
        std::bit_cast<std::uint64_t>(request.filter.blur),
        request.border,
    };
}

} // namespace getnative::detail

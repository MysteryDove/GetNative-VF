#pragma once

#include "getnative/types.hpp"

#include <cstdint>
#include <optional>

namespace getnative {

[[nodiscard]] std::int64_t python_round_to_even(double value);
[[nodiscard]] std::int64_t python_int(double value);

[[nodiscard]] Geometry descale_geometry(
    std::int64_t source_width,
    std::int64_t source_height,
    double active_width,
    double active_height,
    std::optional<std::int64_t> base_height = std::nullopt);

[[nodiscard]] Geometry descale_geometry_pro(
    double active_width,
    double active_height,
    std::optional<std::int64_t> base_height = std::nullopt,
    std::optional<std::int64_t> base_width = std::nullopt);

[[nodiscard]] Geometry resolve_candidate_geometry(
    std::int64_t source_width,
    std::int64_t source_height,
    GeometryAxisMode axis_mode,
    double candidate,
    std::optional<std::int64_t> base_height = std::nullopt,
    std::optional<std::int64_t> base_width = std::nullopt);

} // namespace getnative

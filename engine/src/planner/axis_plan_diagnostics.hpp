#pragma once

#include "getnative/axis_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace getnative::detail {

enum class TapEvaluationMode {
    recompute,
    reuse,
};

enum class BicubicGeometryMode {
    independent,
    reuse,
};

struct AxisPlanGeometry;

struct AxisPlanBuildScratch {
    std::vector<std::uint32_t> descale_offsets;
    std::vector<std::int32_t> descale_indices;
    std::vector<double> descale_weights;
    std::vector<std::uint32_t> forward_offsets;
    std::vector<std::int32_t> forward_indices;
    std::vector<double> forward_weights;
    std::vector<double> normal_bands;
};

[[nodiscard]] AxisPlan build_axis_plan_with_tap_evaluation(
    const AxisPlanRequest &request,
    TapEvaluationMode mode);

[[nodiscard]] std::shared_ptr<const AxisPlanGeometry> build_axis_plan_geometry(
    const AxisPlanRequest &request);
[[nodiscard]] std::size_t axis_plan_geometry_bytes(
    const AxisPlanGeometry &geometry) noexcept;
[[nodiscard]] AxisPlan build_axis_plan_with_geometry(
    const AxisPlanRequest &request,
    TapEvaluationMode tap_evaluation,
    const AxisPlanGeometry *geometry,
    AxisPlanBuildScratch *scratch = nullptr);

} // namespace getnative::detail

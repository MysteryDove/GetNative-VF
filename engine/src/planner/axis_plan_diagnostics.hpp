#pragma once

#include "getnative/axis_plan.hpp"

#include <cstddef>
#include <memory>

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
    const AxisPlanGeometry *geometry);

} // namespace getnative::detail

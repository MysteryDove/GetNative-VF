#pragma once

#include "getnative/axis_plan.hpp"

namespace getnative::detail {

enum class TapEvaluationMode {
    recompute,
    reuse,
};

[[nodiscard]] AxisPlan build_axis_plan_with_tap_evaluation(
    const AxisPlanRequest &request,
    TapEvaluationMode mode);

} // namespace getnative::detail

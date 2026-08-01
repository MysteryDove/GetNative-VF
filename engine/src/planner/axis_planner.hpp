#pragma once

#include "axis_plan_diagnostics.hpp"
#include "getnative/axis_plan.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#ifndef GETNATIVE_PLANNER_REUSE_TAPS
#define GETNATIVE_PLANNER_REUSE_TAPS 1
#endif

namespace getnative::detail {

struct AxisPlanBatchOptions {
    std::size_t worker_count = 0;
    std::function<void(std::size_t)> before_build;
    std::function<void(std::size_t)> failure_observed;
    TapEvaluationMode tap_evaluation = GETNATIVE_PLANNER_REUSE_TAPS != 0
        ? TapEvaluationMode::reuse : TapEvaluationMode::recompute;
    BicubicGeometryMode bicubic_geometry = BicubicGeometryMode::reuse;
};

struct AxisPlanBatchResult {
    std::vector<std::shared_ptr<const AxisPlan>> plans;
    std::size_t unique_key_count = 0;
    std::size_t physical_build_count = 0;
    std::size_t peak_active_builds = 0;
    std::size_t effective_worker_count = 0;
    std::size_t bicubic_geometry_family_count = 0;
    std::size_t bicubic_geometry_plan_count = 0;
    std::size_t bicubic_geometry_build_count = 0;
    std::size_t bicubic_geometry_scratch_bytes = 0;
};

[[nodiscard]] AxisPlanBatchResult build_axis_plans(
    std::span<const AxisPlanRequest> requests,
    AxisPlanBatchOptions options = {});

} // namespace getnative::detail

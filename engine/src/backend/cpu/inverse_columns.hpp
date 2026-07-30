#pragma once

#include "getnative/cpu_analysis.hpp"

#include <cstdint>
#include <string_view>

namespace getnative::detail {

enum class ColumnDispatchPolicy : std::uint8_t {
    automatic,
    scalar_only,
    required_simd,
};

[[nodiscard]] bool column_simd_available() noexcept;
[[nodiscard]] std::string_view column_simd_name() noexcept;

void inverse_columns_scalar_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count);

void inverse_columns_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count,
    ColumnDispatchPolicy policy = ColumnDispatchPolicy::automatic);

[[nodiscard]] double analyze_axis_candidate_with_column_policy_f32(
    ConstImageView source, const AxisPlan &axis, AnalysisAxes axis_direction,
    const MetricSpec &metric, CpuWorkspace &workspace, ColumnDispatchPolicy policy);

} // namespace getnative::detail

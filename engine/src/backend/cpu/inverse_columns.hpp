#pragma once

#include "getnative/cpu_analysis.hpp"
#include "getnative/cpu_features.hpp"

#include <cstdint>
#include <string_view>

namespace getnative::detail {

enum class ColumnDispatchPolicy : std::uint8_t {
    automatic,
    scalar_only,
    required_simd,
    sse2_strict,
    avx2_strict,
    avx512_strict,
};

using AbsoluteDifferenceBlockFunction = void (*)(
    const float *source, const float *reconstruction, float *differences) noexcept;
using AbsoluteDifferenceNorm1RowFunction = double (*)(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
using VerticalReconstructionBlockFunction = void (*)(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept;
using VerticalReconstructionNorm1RowFunction = double (*)(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;

struct AnalysisRowDispatch {
    std::int32_t lanes = 1;
    AbsoluteDifferenceBlockFunction absolute_difference = nullptr;
    AbsoluteDifferenceNorm1RowFunction absolute_difference_norm1 = nullptr;
    VerticalReconstructionBlockFunction vertical_reconstruction = nullptr;
    VerticalReconstructionNorm1RowFunction vertical_reconstruction_norm1 = nullptr;
};

[[nodiscard]] bool column_simd_available() noexcept;
[[nodiscard]] std::string_view column_simd_name() noexcept;
[[nodiscard]] ColumnDispatchPolicy column_dispatch_policy(
    CpuIsaRequest request) noexcept;
void validate_column_dispatch_policy(ColumnDispatchPolicy policy);
[[nodiscard]] AnalysisRowDispatch analysis_row_dispatch(
    ColumnDispatchPolicy policy);

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

[[nodiscard]] double analyze_candidate_with_column_policy_f32(
    ConstImageView source, const AxisPlan &horizontal, const AxisPlan &vertical,
    const MetricSpec &metric, CpuWorkspace &workspace, ColumnDispatchPolicy policy);

[[nodiscard]] std::vector<CandidateResult> analyze_batch_with_column_policy_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, ColumnDispatchPolicy policy,
    std::size_t worker_count = 0, std::size_t workspace_limit_elements = 0);

} // namespace getnative::detail

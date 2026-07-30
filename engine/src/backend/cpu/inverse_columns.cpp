#include "inverse_columns.hpp"

#include <stdexcept>

namespace getnative::detail {

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
void inverse_columns_neon_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) noexcept;
#endif

namespace {

void validate_columns(const AxisPlan &plan, const float *input,
                      std::ptrdiff_t input_row_stride, float *output,
                      std::ptrdiff_t output_row_stride, std::int32_t column_count) {
    if (!plan.valid() || input == nullptr || output == nullptr
        || input_row_stride == 0 || output_row_stride == 0 || column_count < 0) {
        throw std::invalid_argument("invalid inverse column arguments");
    }
}

} // namespace

bool column_simd_available() noexcept {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return true;
#else
    return false;
#endif
}

std::string_view column_simd_name() noexcept {
    return column_simd_available() ? "neon-f32x4" : "scalar";
}

void inverse_columns_scalar_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) {
    validate_columns(
        plan, input, input_row_stride, output, output_row_stride, column_count);
    for (std::int32_t column = 0; column < column_count; ++column) {
        inverse_axis_f32(plan, input + column, input_row_stride,
                         output + column, output_row_stride);
    }
}

void inverse_columns_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count,
    ColumnDispatchPolicy policy) {
    validate_columns(
        plan, input, input_row_stride, output, output_row_stride, column_count);
    if (policy == ColumnDispatchPolicy::scalar_only || column_count == 0) {
        inverse_columns_scalar_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    inverse_columns_neon_f32(
        plan, input, input_row_stride, output, output_row_stride, column_count);
#else
    if (policy == ColumnDispatchPolicy::required_simd) {
        throw std::runtime_error("required adjacent-column SIMD is unavailable");
    }
    inverse_columns_scalar_f32(
        plan, input, input_row_stride, output, output_row_stride, column_count);
#endif
}

} // namespace getnative::detail

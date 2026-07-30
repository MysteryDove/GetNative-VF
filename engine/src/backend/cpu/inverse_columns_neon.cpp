#include "inverse_columns.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

#include <arm_neon.h>

#include <algorithm>
#include <cstddef>

namespace getnative::detail {
namespace {

template <std::int32_t FixedHalfBandwidth>
void inverse_columns_neon_impl(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) noexcept {
    const std::int32_t destination_size = plan.destination_size;
    const std::int32_t half_bandwidth = FixedHalfBandwidth == 0
        ? plan.half_bandwidth : FixedHalfBandwidth;
    const auto factor_row_width = static_cast<std::size_t>(destination_size);
    std::int32_t column = 0;
    for (; column <= column_count - 4; column += 4) {
        for (std::int32_t i = 0; i < destination_size; ++i) {
            float32x4_t sum = vdupq_n_f32(0.0F);
            for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(i)];
                 p < plan.transpose_offsets[static_cast<std::size_t>(i) + 1U]; ++p) {
                const float32x4_t values = vld1q_f32(
                    input + static_cast<std::ptrdiff_t>(plan.transpose_indices[p])
                        * input_row_stride + column);
                const float32x4_t product = vmulq_n_f32(values, plan.transpose_weights[p]);
                sum = vaddq_f32(sum, product);
            }
            const std::int32_t available = std::min(half_bandwidth, i);
            for (std::int32_t distance = available; distance >= 1; --distance) {
                const float32x4_t previous = vld1q_f32(
                    output + static_cast<std::ptrdiff_t>(i - distance)
                        * output_row_stride + column);
                const float factor = plan.lower_ld[
                    static_cast<std::size_t>(distance - 1) * factor_row_width
                    + static_cast<std::size_t>(i)];
                const float32x4_t product = vmulq_n_f32(previous, factor);
                sum = vsubq_f32(sum, product);
            }
            sum = vmulq_n_f32(sum, plan.inverse_diagonal[static_cast<std::size_t>(i)]);
            vst1q_f32(output + static_cast<std::ptrdiff_t>(i) * output_row_stride + column,
                       sum);
        }

        for (std::int32_t i = destination_size - 2; i >= 0; --i) {
            float32x4_t sum = vdupq_n_f32(0.0F);
            const std::int32_t available = std::min(
                half_bandwidth, destination_size - i - 1);
            if constexpr (FixedHalfBandwidth == 3) {
                for (std::int32_t distance = 1; distance <= available; ++distance) {
                    const float32x4_t next = vld1q_f32(
                        output + static_cast<std::ptrdiff_t>(i + distance)
                            * output_row_stride + column);
                    const float factor = plan.upper_l[
                        static_cast<std::size_t>(distance - 1) * factor_row_width
                        + static_cast<std::size_t>(i)];
                    const float32x4_t product = vmulq_n_f32(next, factor);
                    sum = vaddq_f32(sum, product);
                }
            } else {
                for (std::int32_t distance = available; distance >= 1; --distance) {
                    const float32x4_t next = vld1q_f32(
                        output + static_cast<std::ptrdiff_t>(i + distance)
                            * output_row_stride + column);
                    const float factor = plan.upper_l[
                        static_cast<std::size_t>(distance - 1) * factor_row_width
                        + static_cast<std::size_t>(i)];
                    const float32x4_t product = vmulq_n_f32(next, factor);
                    sum = vaddq_f32(sum, product);
                }
            }
            float32x4_t current = vld1q_f32(
                output + static_cast<std::ptrdiff_t>(i) * output_row_stride + column);
            current = vsubq_f32(current, sum);
            vst1q_f32(output + static_cast<std::ptrdiff_t>(i) * output_row_stride + column,
                       current);
        }
    }

    for (; column < column_count; ++column) {
        inverse_axis_f32(plan, input + column, input_row_stride,
                         output + column, output_row_stride);
    }
}

} // namespace

void inverse_columns_neon_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) noexcept {
    if (plan.half_bandwidth == 1) {
        inverse_columns_neon_impl<1>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
    } else if (plan.half_bandwidth == 3) {
        inverse_columns_neon_impl<3>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
    } else {
        inverse_columns_neon_impl<0>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
    }
}

} // namespace getnative::detail

#endif

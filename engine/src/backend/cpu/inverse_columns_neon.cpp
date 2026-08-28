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
    for (; column <= column_count - 8; column += 8) {
        for (std::int32_t i = 0; i < destination_size; ++i) {
            float32x4_t sum_low = vdupq_n_f32(0.0F);
            float32x4_t sum_high = vdupq_n_f32(0.0F);
            for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(i)];
                 p < plan.transpose_offsets[static_cast<std::size_t>(i) + 1U]; ++p) {
                const float *values = input
                    + static_cast<std::ptrdiff_t>(plan.transpose_indices[p])
                        * input_row_stride + column;
                const float factor = plan.transpose_weights[p];
                sum_low = vfmaq_n_f32(sum_low, vld1q_f32(values), factor);
                sum_high = vfmaq_n_f32(sum_high, vld1q_f32(values + 4), factor);
            }
            const std::int32_t available = std::min(half_bandwidth, i);
            for (std::int32_t distance = available; distance >= 1; --distance) {
                const float *previous = output
                    + static_cast<std::ptrdiff_t>(i - distance)
                        * output_row_stride + column;
                const float factor = plan.lower_ld[
                    static_cast<std::size_t>(distance - 1) * factor_row_width
                    + static_cast<std::size_t>(i)];
                sum_low = vfmsq_n_f32(sum_low, vld1q_f32(previous), factor);
                sum_high = vfmsq_n_f32(sum_high, vld1q_f32(previous + 4), factor);
            }
            const float inverse_diagonal = plan.inverse_diagonal[static_cast<std::size_t>(i)];
            sum_low = vmulq_n_f32(sum_low, inverse_diagonal);
            sum_high = vmulq_n_f32(sum_high, inverse_diagonal);
            float *current = output
                + static_cast<std::ptrdiff_t>(i) * output_row_stride + column;
            vst1q_f32(current, sum_low);
            vst1q_f32(current + 4, sum_high);
        }

        for (std::int32_t i = destination_size - 2; i >= 0; --i) {
            float32x4_t sum_low = vdupq_n_f32(0.0F);
            float32x4_t sum_high = vdupq_n_f32(0.0F);
            const std::int32_t available = std::min(
                half_bandwidth, destination_size - i - 1);
            if constexpr (FixedHalfBandwidth == 3) {
                for (std::int32_t distance = 1; distance <= available; ++distance) {
                    const float *next = output
                        + static_cast<std::ptrdiff_t>(i + distance)
                            * output_row_stride + column;
                    const float factor = plan.upper_l[
                        static_cast<std::size_t>(distance - 1) * factor_row_width
                        + static_cast<std::size_t>(i)];
                    sum_low = vfmaq_n_f32(sum_low, vld1q_f32(next), factor);
                    sum_high = vfmaq_n_f32(sum_high, vld1q_f32(next + 4), factor);
                }
            } else {
                for (std::int32_t distance = available; distance >= 1; --distance) {
                    const float *next = output
                        + static_cast<std::ptrdiff_t>(i + distance)
                            * output_row_stride + column;
                    const float factor = plan.upper_l[
                        static_cast<std::size_t>(distance - 1) * factor_row_width
                        + static_cast<std::size_t>(i)];
                    sum_low = vfmaq_n_f32(sum_low, vld1q_f32(next), factor);
                    sum_high = vfmaq_n_f32(sum_high, vld1q_f32(next + 4), factor);
                }
            }
            float *current = output
                + static_cast<std::ptrdiff_t>(i) * output_row_stride + column;
            const float32x4_t current_low = vsubq_f32(vld1q_f32(current), sum_low);
            const float32x4_t current_high = vsubq_f32(vld1q_f32(current + 4), sum_high);
            vst1q_f32(current, current_low);
            vst1q_f32(current + 4, current_high);
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

namespace {

// One element from each of four rows packed into lanes. Row streams stay
// contiguous per lane, so the four lane loads behave like four independent
// sequential streams — same access pattern the scalar loop has per row.
inline float32x4_t gather_rows_f32(
    const float *row0, const float *row1, const float *row2, const float *row3,
    std::ptrdiff_t index) noexcept {
    float32x4_t gathered = vld1q_lane_f32(row0 + index, vdupq_n_f32(0.0F), 0);
    gathered = vld1q_lane_f32(row1 + index, gathered, 1);
    gathered = vld1q_lane_f32(row2 + index, gathered, 2);
    return vld1q_lane_f32(row3 + index, gathered, 3);
}

// Mirrors inverse_axis_impl step for step: each lane runs the exact scalar
// FMA sequence of one row, so results are bit-identical to per-row scalar.
// vfmsq_n_f32(sum, x, f) == std::fma(-f, x, sum) (negation is exact), and
// the bandwidth-3 backward sweep keeps its near-to-far ordering.
template <std::int32_t FixedHalfBandwidth>
void inverse_rows_neon_impl(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t row_count) noexcept {
    const std::int32_t n = plan.destination_size;
    const std::int32_t half_bandwidth = FixedHalfBandwidth == 0
        ? plan.half_bandwidth : FixedHalfBandwidth;
    const auto factor_row_width = static_cast<std::size_t>(n);
    alignas(16) float lane_values[4];
    std::int32_t row = 0;
    for (; row <= row_count - 4; row += 4) {
        const float *in0 = input + static_cast<std::ptrdiff_t>(row) * input_row_stride;
        const float *in1 = in0 + input_row_stride;
        const float *in2 = in1 + input_row_stride;
        const float *in3 = in2 + input_row_stride;
        float *out0 = output + static_cast<std::ptrdiff_t>(row) * output_row_stride;
        float *out1 = out0 + output_row_stride;
        float *out2 = out1 + output_row_stride;
        float *out3 = out2 + output_row_stride;
        for (std::int32_t i = 0; i < n; ++i) {
            float32x4_t sum = vdupq_n_f32(0.0F);
            for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(i)];
                 p < plan.transpose_offsets[static_cast<std::size_t>(i) + 1U]; ++p) {
                const auto index =
                    static_cast<std::ptrdiff_t>(plan.transpose_indices[p]);
                sum = vfmaq_n_f32(
                    sum, gather_rows_f32(in0, in1, in2, in3, index),
                    plan.transpose_weights[p]);
            }
            const std::int32_t available = std::min(half_bandwidth, i);
            for (std::int32_t distance = available; distance >= 1; --distance) {
                sum = vfmsq_n_f32(
                    sum, gather_rows_f32(out0, out1, out2, out3, i - distance),
                    plan.lower_ld[static_cast<std::size_t>(distance - 1) * factor_row_width
                                  + static_cast<std::size_t>(i)]);
            }
            sum = vmulq_n_f32(sum, plan.inverse_diagonal[static_cast<std::size_t>(i)]);
            vst1q_f32(lane_values, sum);
            out0[i] = lane_values[0];
            out1[i] = lane_values[1];
            out2[i] = lane_values[2];
            out3[i] = lane_values[3];
        }
        for (std::int32_t i = n - 2; i >= 0; --i) {
            float32x4_t sum = vdupq_n_f32(0.0F);
            const std::int32_t available = std::min(half_bandwidth, n - i - 1);
            if constexpr (FixedHalfBandwidth == 3) {
                for (std::int32_t distance = 1; distance <= available; ++distance) {
                    sum = vfmaq_n_f32(
                        sum, gather_rows_f32(out0, out1, out2, out3, i + distance),
                        plan.upper_l[static_cast<std::size_t>(distance - 1) * factor_row_width
                                     + static_cast<std::size_t>(i)]);
                }
            } else {
                for (std::int32_t distance = available; distance >= 1; --distance) {
                    sum = vfmaq_n_f32(
                        sum, gather_rows_f32(out0, out1, out2, out3, i + distance),
                        plan.upper_l[static_cast<std::size_t>(distance - 1) * factor_row_width
                                     + static_cast<std::size_t>(i)]);
                }
            }
            const float32x4_t current =
                gather_rows_f32(out0, out1, out2, out3, i);
            vst1q_f32(lane_values, vsubq_f32(current, sum));
            out0[i] = lane_values[0];
            out1[i] = lane_values[1];
            out2[i] = lane_values[2];
            out3[i] = lane_values[3];
        }
    }
    for (; row < row_count; ++row) {
        // Row elements are stride-1; input_row_stride is the row pitch.
        inverse_axis_f32(plan,
                         input + static_cast<std::ptrdiff_t>(row) * input_row_stride,
                         1,
                         output + static_cast<std::ptrdiff_t>(row) * output_row_stride,
                         1);
    }
}

} // namespace

void inverse_rows_neon_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t row_count) noexcept {
    if (plan.half_bandwidth == 1) {
        inverse_rows_neon_impl<1>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
    } else if (plan.half_bandwidth == 3) {
        inverse_rows_neon_impl<3>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
    } else {
        inverse_rows_neon_impl<0>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
    }
}

} // namespace getnative::detail

#endif

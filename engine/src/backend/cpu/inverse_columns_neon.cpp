#include "inverse_columns.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

#include <arm_neon.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace getnative::detail {
namespace {

void transpose4(float32x4_t &row0, float32x4_t &row1,
                float32x4_t &row2, float32x4_t &row3) noexcept {
    const float32x4x2_t t0 = vtrnq_f32(row0, row1);
    const float32x4x2_t t1 = vtrnq_f32(row2, row3);
    row0 = vcombine_f32(vget_low_f32(t0.val[0]), vget_low_f32(t1.val[0]));
    row1 = vcombine_f32(vget_low_f32(t0.val[1]), vget_low_f32(t1.val[1]));
    row2 = vcombine_f32(vget_high_f32(t0.val[0]), vget_high_f32(t1.val[0]));
    row3 = vcombine_f32(vget_high_f32(t0.val[1]), vget_high_f32(t1.val[1]));
}

void transpose_four_rows(
    const float *row0, const float *row1, const float *row2, const float *row3,
    std::int32_t width, float *scratch) noexcept {
    std::int32_t column = 0;
    for (; column + 4 <= width; column += 4) {
        float32x4_t x0 = vld1q_f32(row0 + column);
        float32x4_t x1 = vld1q_f32(row1 + column);
        float32x4_t x2 = vld1q_f32(row2 + column);
        float32x4_t x3 = vld1q_f32(row3 + column);
        transpose4(x0, x1, x2, x3);
        vst1q_f32(scratch + static_cast<std::size_t>(column + 0) * 4U, x0);
        vst1q_f32(scratch + static_cast<std::size_t>(column + 1) * 4U, x1);
        vst1q_f32(scratch + static_cast<std::size_t>(column + 2) * 4U, x2);
        vst1q_f32(scratch + static_cast<std::size_t>(column + 3) * 4U, x3);
    }
    if (column < width) {
        alignas(16) float tail[4][4] = {};
        const std::int32_t remain = width - column;
        for (std::int32_t tap = 0; tap < remain; ++tap) {
            tail[0][static_cast<std::size_t>(tap)] = row0[column + tap];
            tail[1][static_cast<std::size_t>(tap)] = row1[column + tap];
            tail[2][static_cast<std::size_t>(tap)] = row2[column + tap];
            tail[3][static_cast<std::size_t>(tap)] = row3[column + tap];
        }
        float32x4_t x0 = vld1q_f32(tail[0]);
        float32x4_t x1 = vld1q_f32(tail[1]);
        float32x4_t x2 = vld1q_f32(tail[2]);
        float32x4_t x3 = vld1q_f32(tail[3]);
        transpose4(x0, x1, x2, x3);
        for (std::int32_t tap = 0; tap < remain; ++tap) {
            const float32x4_t packed = tap == 0 ? x0 : tap == 1 ? x1 : tap == 2 ? x2 : x3;
            vst1q_f32(scratch + static_cast<std::size_t>(column + tap) * 4U, packed);
        }
    }
}

void scatter_four_rows(
    const float *scratch, std::int32_t width,
    float *row0, float *row1, float *row2, float *row3) noexcept {
    for (std::int32_t column = 0; column < width; ++column) {
        const float32x4_t packed =
            vld1q_f32(scratch + static_cast<std::size_t>(column) * 4U);
        row0[column] = vgetq_lane_f32(packed, 0);
        row1[column] = vgetq_lane_f32(packed, 1);
        row2[column] = vgetq_lane_f32(packed, 2);
        row3[column] = vgetq_lane_f32(packed, 3);
    }
}

template <std::int32_t Count>
void shift_lag(float32x4_t *lag, float32x4_t current) noexcept {
    if constexpr (Count >= 2) {
        for (std::int32_t index = Count - 1; index >= 1; --index) {
            lag[index] = lag[index - 1];
        }
    }
    lag[0] = current;
}

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
        float32x4_t lag_low[FixedHalfBandwidth == 0 ? 1 : FixedHalfBandwidth] = {};
        float32x4_t lag_high[FixedHalfBandwidth == 0 ? 1 : FixedHalfBandwidth] = {};
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
            if constexpr (FixedHalfBandwidth != 0) {
                for (std::int32_t distance = FixedHalfBandwidth; distance >= 1; --distance) {
                    if (distance <= available) {
                        const float factor = plan.lower_ld[
                            static_cast<std::size_t>(distance - 1) * factor_row_width
                            + static_cast<std::size_t>(i)];
                        sum_low = vfmsq_n_f32(sum_low, lag_low[distance - 1], factor);
                        sum_high = vfmsq_n_f32(sum_high, lag_high[distance - 1], factor);
                    }
                }
            } else {
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
            }
            const float inverse_diagonal = plan.inverse_diagonal[static_cast<std::size_t>(i)];
            sum_low = vmulq_n_f32(sum_low, inverse_diagonal);
            sum_high = vmulq_n_f32(sum_high, inverse_diagonal);
            float *current = output
                + static_cast<std::ptrdiff_t>(i) * output_row_stride + column;
            vst1q_f32(current, sum_low);
            vst1q_f32(current + 4, sum_high);
            if constexpr (FixedHalfBandwidth != 0) {
                shift_lag<FixedHalfBandwidth>(lag_low, sum_low);
                shift_lag<FixedHalfBandwidth>(lag_high, sum_high);
            }
        }

        float32x4_t rlag_low[FixedHalfBandwidth == 0 ? 1 : FixedHalfBandwidth] = {};
        float32x4_t rlag_high[FixedHalfBandwidth == 0 ? 1 : FixedHalfBandwidth] = {};
        if constexpr (FixedHalfBandwidth != 0) {
            for (std::int32_t k = 0; k < FixedHalfBandwidth; ++k) {
                const std::int32_t index = destination_size - 1 + k;
                if (index < destination_size) {
                    const float *values = output
                        + static_cast<std::ptrdiff_t>(index) * output_row_stride + column;
                    rlag_low[k] = vld1q_f32(values);
                    rlag_high[k] = vld1q_f32(values + 4);
                }
            }
        }
        for (std::int32_t i = destination_size - 2; i >= 0; --i) {
            float32x4_t sum_low = vdupq_n_f32(0.0F);
            float32x4_t sum_high = vdupq_n_f32(0.0F);
            const std::int32_t available = std::min(
                half_bandwidth, destination_size - i - 1);
            if constexpr (FixedHalfBandwidth == 3) {
                for (std::int32_t distance = 1; distance <= FixedHalfBandwidth; ++distance) {
                    if (distance <= available) {
                        const float factor = plan.upper_l[
                            static_cast<std::size_t>(distance - 1) * factor_row_width
                            + static_cast<std::size_t>(i)];
                        sum_low = vfmaq_n_f32(sum_low, rlag_low[distance - 1], factor);
                        sum_high = vfmaq_n_f32(sum_high, rlag_high[distance - 1], factor);
                    }
                }
            } else if constexpr (FixedHalfBandwidth != 0) {
                for (std::int32_t distance = FixedHalfBandwidth; distance >= 1; --distance) {
                    if (distance <= available) {
                        const float factor = plan.upper_l[
                            static_cast<std::size_t>(distance - 1) * factor_row_width
                            + static_cast<std::size_t>(i)];
                        sum_low = vfmaq_n_f32(sum_low, rlag_low[distance - 1], factor);
                        sum_high = vfmaq_n_f32(sum_high, rlag_high[distance - 1], factor);
                    }
                }
            } else if constexpr (FixedHalfBandwidth == 0) {
                if (plan.half_bandwidth == 3) {
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
            }
            float *current = output
                + static_cast<std::ptrdiff_t>(i) * output_row_stride + column;
            const float32x4_t current_low = vsubq_f32(vld1q_f32(current), sum_low);
            const float32x4_t current_high = vsubq_f32(vld1q_f32(current + 4), sum_high);
            vst1q_f32(current, current_low);
            vst1q_f32(current + 4, current_high);
            if constexpr (FixedHalfBandwidth != 0) {
                shift_lag<FixedHalfBandwidth>(rlag_low, current_low);
                shift_lag<FixedHalfBandwidth>(rlag_high, current_high);
            }
        }
    }

    for (; column < column_count; ++column) {
        inverse_axis_f32(plan, input + column, input_row_stride,
                         output + column, output_row_stride);
    }
}

std::vector<float> &row_scratch() {
    thread_local std::vector<float> scratch;
    return scratch;
}

void ensure_scratch(std::vector<float> &scratch, std::size_t count) {
    if (scratch.size() < count) scratch.resize(count);
}

template <std::int32_t FixedHalfBandwidth>
void inverse_four_rows_transposed(
    const AxisPlan &plan, const float *scratch_in, float *scratch_out) noexcept {
    const std::int32_t n = plan.destination_size;
    const std::int32_t half_bandwidth = FixedHalfBandwidth == 0
        ? plan.half_bandwidth : FixedHalfBandwidth;
    const auto factor_row_width = static_cast<std::size_t>(n);
    float32x4_t lag[FixedHalfBandwidth == 0 ? 1 : FixedHalfBandwidth] = {};
    for (std::int32_t i = 0; i < n; ++i) {
        float32x4_t sum = vdupq_n_f32(0.0F);
        for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(i)];
             p < plan.transpose_offsets[static_cast<std::size_t>(i) + 1U]; ++p) {
            sum = vfmaq_n_f32(
                sum,
                vld1q_f32(scratch_in
                          + static_cast<std::size_t>(plan.transpose_indices[p]) * 4U),
                plan.transpose_weights[p]);
        }
        const std::int32_t available = std::min(half_bandwidth, i);
        if constexpr (FixedHalfBandwidth != 0) {
            for (std::int32_t distance = FixedHalfBandwidth; distance >= 1; --distance) {
                if (distance <= available) {
                    sum = vfmsq_n_f32(
                        sum, lag[distance - 1],
                        plan.lower_ld[static_cast<std::size_t>(distance - 1) * factor_row_width
                                      + static_cast<std::size_t>(i)]);
                }
            }
        } else {
            for (std::int32_t distance = available; distance >= 1; --distance) {
                sum = vfmsq_n_f32(
                    sum,
                    vld1q_f32(scratch_out
                              + static_cast<std::size_t>(i - distance) * 4U),
                    plan.lower_ld[static_cast<std::size_t>(distance - 1) * factor_row_width
                                  + static_cast<std::size_t>(i)]);
            }
        }
        sum = vmulq_n_f32(sum, plan.inverse_diagonal[static_cast<std::size_t>(i)]);
        vst1q_f32(scratch_out + static_cast<std::size_t>(i) * 4U, sum);
        if constexpr (FixedHalfBandwidth != 0) {
            shift_lag<FixedHalfBandwidth>(lag, sum);
        }
    }
    if (n < 2) return;
    float32x4_t rlag[FixedHalfBandwidth == 0 ? 1 : FixedHalfBandwidth] = {};
    if constexpr (FixedHalfBandwidth != 0) {
        for (std::int32_t k = 0; k < FixedHalfBandwidth; ++k) {
            const std::int32_t index = n - 1 + k;
            if (index < n) {
                rlag[k] = vld1q_f32(scratch_out + static_cast<std::size_t>(index) * 4U);
            }
        }
    }
    for (std::int32_t i = n - 2; i >= 0; --i) {
        float32x4_t sum = vdupq_n_f32(0.0F);
        const std::int32_t available = std::min(half_bandwidth, n - i - 1);
        auto load_next = [&](std::int32_t distance) noexcept {
            if constexpr (FixedHalfBandwidth != 0) {
                return rlag[distance - 1];
            } else {
                return vld1q_f32(scratch_out
                                 + static_cast<std::size_t>(i + distance) * 4U);
            }
        };
        if constexpr (FixedHalfBandwidth == 3) {
            for (std::int32_t distance = 1; distance <= FixedHalfBandwidth; ++distance) {
                if (distance <= available) {
                    sum = vfmaq_n_f32(
                        sum, load_next(distance),
                        plan.upper_l[static_cast<std::size_t>(distance - 1) * factor_row_width
                                     + static_cast<std::size_t>(i)]);
                }
            }
        } else if constexpr (FixedHalfBandwidth != 0) {
            for (std::int32_t distance = FixedHalfBandwidth; distance >= 1; --distance) {
                if (distance <= available) {
                    sum = vfmaq_n_f32(
                        sum, load_next(distance),
                        plan.upper_l[static_cast<std::size_t>(distance - 1) * factor_row_width
                                     + static_cast<std::size_t>(i)]);
                }
            }
        } else if (plan.half_bandwidth == 3) {
            for (std::int32_t distance = 1; distance <= available; ++distance) {
                sum = vfmaq_n_f32(
                    sum, load_next(distance),
                    plan.upper_l[static_cast<std::size_t>(distance - 1) * factor_row_width
                                 + static_cast<std::size_t>(i)]);
            }
        } else {
            for (std::int32_t distance = available; distance >= 1; --distance) {
                sum = vfmaq_n_f32(
                    sum, load_next(distance),
                    plan.upper_l[static_cast<std::size_t>(distance - 1) * factor_row_width
                                 + static_cast<std::size_t>(i)]);
            }
        }
        const float32x4_t current = vsubq_f32(
            vld1q_f32(scratch_out + static_cast<std::size_t>(i) * 4U), sum);
        vst1q_f32(scratch_out + static_cast<std::size_t>(i) * 4U, current);
        if constexpr (FixedHalfBandwidth != 0) {
            shift_lag<FixedHalfBandwidth>(rlag, current);
        }
    }
}

template <std::int32_t FixedHalfBandwidth>
void inverse_rows_neon_impl(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t row_count) noexcept {
    const std::int32_t source_size = plan.source_size;
    const std::int32_t n = plan.destination_size;
    auto &scratch = row_scratch();
    const std::size_t in_elements = static_cast<std::size_t>(source_size) * 4U;
    const std::size_t out_elements = static_cast<std::size_t>(n) * 4U;
    ensure_scratch(scratch, in_elements + out_elements);
    float *scratch_in = scratch.data();
    float *scratch_out = scratch.data() + in_elements;
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
        transpose_four_rows(in0, in1, in2, in3, source_size, scratch_in);
        inverse_four_rows_transposed<FixedHalfBandwidth>(plan, scratch_in, scratch_out);
        scatter_four_rows(scratch_out, n, out0, out1, out2, out3);
    }
    for (; row < row_count; ++row) {
        inverse_axis_f32(plan,
                         input + static_cast<std::ptrdiff_t>(row) * input_row_stride,
                         1,
                         output + static_cast<std::ptrdiff_t>(row) * output_row_stride,
                         1);
    }
}

template <std::int32_t FixedWidth>
void forward_columns_neon_impl(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) noexcept {
    const std::int32_t width = FixedWidth == 0 ? plan.forward_width : FixedWidth;
    const auto row_stride = static_cast<std::size_t>(plan.forward_width);
    std::int32_t column = 0;
    for (; column <= column_count - 8; column += 8) {
        for (std::int32_t row = 0; row < plan.source_size; ++row) {
            const std::size_t begin = static_cast<std::size_t>(row) * row_stride;
            const std::int32_t left = plan.forward_indices[begin];
            float32x4_t sum_low = vdupq_n_f32(0.0F);
            float32x4_t sum_high = vdupq_n_f32(0.0F);
            for (std::int32_t tap = 0; tap < width; ++tap) {
                const float *values = input
                    + static_cast<std::ptrdiff_t>(left + tap) * input_row_stride + column;
                const float weight = plan.forward_weights[begin + static_cast<std::size_t>(tap)];
                sum_low = vfmaq_n_f32(sum_low, vld1q_f32(values), weight);
                sum_high = vfmaq_n_f32(sum_high, vld1q_f32(values + 4), weight);
            }
            float *destination = output
                + static_cast<std::ptrdiff_t>(row) * output_row_stride + column;
            vst1q_f32(destination, sum_low);
            vst1q_f32(destination + 4, sum_high);
        }
    }
    for (; column < column_count; ++column) {
        forward_axis_f32(plan, input + column, input_row_stride,
                         output + column, output_row_stride);
    }
}

template <std::int32_t FixedWidth>
void forward_rows_neon_impl(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t row_count) noexcept {
    const std::int32_t width = FixedWidth == 0 ? plan.forward_width : FixedWidth;
    const auto row_stride = static_cast<std::size_t>(plan.forward_width);
    const std::int32_t native_width = plan.destination_size;
    auto &scratch = row_scratch();
    ensure_scratch(scratch, static_cast<std::size_t>(native_width) * 4U);
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
        transpose_four_rows(in0, in1, in2, in3, native_width, scratch.data());
        for (std::int32_t out_row = 0; out_row < plan.source_size; ++out_row) {
            const std::size_t begin = static_cast<std::size_t>(out_row) * row_stride;
            const std::int32_t left = plan.forward_indices[begin];
            float32x4_t sum = vdupq_n_f32(0.0F);
            for (std::int32_t tap = 0; tap < width; ++tap) {
                sum = vfmaq_n_f32(
                    sum,
                    vld1q_f32(scratch.data()
                              + static_cast<std::size_t>(left + tap) * 4U),
                    plan.forward_weights[begin + static_cast<std::size_t>(tap)]);
            }
            out0[out_row] = vgetq_lane_f32(sum, 0);
            out1[out_row] = vgetq_lane_f32(sum, 1);
            out2[out_row] = vgetq_lane_f32(sum, 2);
            out3[out_row] = vgetq_lane_f32(sum, 3);
        }
    }
    for (; row < row_count; ++row) {
        forward_axis_f32(plan,
                         input + static_cast<std::ptrdiff_t>(row) * input_row_stride,
                         1,
                         output + static_cast<std::ptrdiff_t>(row) * output_row_stride,
                         1);
    }
}

[[nodiscard]] double add_norm1_lanes(
    float32x4_t difference, float32x4_t threshold, double sum) noexcept {
    const uint32x4_t mask = vcgtq_f32(difference, threshold);
    const float32x4_t masked = vreinterpretq_f32_u32(
        vandq_u32(vreinterpretq_u32_f32(difference), mask));
    sum += static_cast<double>(vgetq_lane_f32(masked, 0));
    sum += static_cast<double>(vgetq_lane_f32(masked, 1));
    sum += static_cast<double>(vgetq_lane_f32(masked, 2));
    sum += static_cast<double>(vgetq_lane_f32(masked, 3));
    return sum;
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

void forward_columns_neon_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) noexcept {
    switch (plan.forward_width) {
    case 2:
        forward_columns_neon_impl<2>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    case 4:
        forward_columns_neon_impl<4>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    case 6:
        forward_columns_neon_impl<6>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    case 8:
        forward_columns_neon_impl<8>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    default:
        forward_columns_neon_impl<0>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    }
}

void forward_rows_neon_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t row_count) noexcept {
    switch (plan.forward_width) {
    case 2:
        forward_rows_neon_impl<2>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
        return;
    case 4:
        forward_rows_neon_impl<4>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
        return;
    case 6:
        forward_rows_neon_impl<6>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
        return;
    case 8:
        forward_rows_neon_impl<8>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
        return;
    default:
        forward_rows_neon_impl<0>(
            plan, input, input_row_stride, output, output_row_stride, row_count);
        return;
    }
}

void absolute_difference_neon_f32(
    const float *source, const float *reconstruction, float *differences) noexcept {
    const float32x4_t difference = vabdq_f32(vld1q_f32(source), vld1q_f32(reconstruction));
    vst1q_f32(differences, difference);
}

double absolute_difference_norm1_neon_f32(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const float32x4_t threshold_values = vdupq_n_f32(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += 4) {
        const float32x4_t difference =
            vabdq_f32(vld1q_f32(source + x), vld1q_f32(reconstruction + x));
        sum = add_norm1_lanes(difference, threshold_values, sum);
    }
    return sum;
}

void vertical_reconstruction_neon_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept {
    float32x4_t reconstructed = vdupq_n_f32(0.0F);
    for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
        reconstructed = vfmaq_n_f32(
            reconstructed,
            vld1q_f32(native + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x),
            plan.forward_weights[begin + static_cast<std::uint32_t>(tap)]);
    }
    vst1q_f32(differences, vabdq_f32(vld1q_f32(source + x), reconstructed));
}

double vertical_reconstruction_norm1_neon_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const float32x4_t threshold_values = vdupq_n_f32(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += 4) {
        float32x4_t reconstructed = vdupq_n_f32(0.0F);
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            reconstructed = vfmaq_n_f32(
                reconstructed,
                vld1q_f32(native
                          + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x),
                plan.forward_weights[begin + static_cast<std::uint32_t>(tap)]);
        }
        const float32x4_t difference = vabdq_f32(vld1q_f32(source + x), reconstructed);
        sum = add_norm1_lanes(difference, threshold_values, sum);
    }
    return sum;
}

} // namespace getnative::detail

#endif

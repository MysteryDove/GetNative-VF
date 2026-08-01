#pragma once

#include "inverse_columns.hpp"

#include <algorithm>
#include <cstddef>

namespace getnative::detail {

template <class Operations, std::int32_t FixedHalfBandwidth,
          std::int32_t TileVectors>
void inverse_columns_x86_tile(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column) noexcept {
    using Vector = typename Operations::Vector;
    constexpr std::int32_t lanes = Operations::lanes;
    static_assert(TileVectors >= 1 && TileVectors <= 3);
    const std::int32_t destination_size = plan.destination_size;
    const std::int32_t half_bandwidth = FixedHalfBandwidth == 0
        ? plan.half_bandwidth : FixedHalfBandwidth;
    const auto factor_row_width = static_cast<std::size_t>(destination_size);
    Vector sum0 = Operations::zero();
    Vector sum1 = Operations::zero();
    Vector sum2 = Operations::zero();

    for (std::int32_t i = 0; i < destination_size; ++i) {
        sum0 = Operations::zero();
        if constexpr (TileVectors >= 2) sum1 = Operations::zero();
        if constexpr (TileVectors >= 3) sum2 = Operations::zero();
        for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(i)];
             p < plan.transpose_offsets[static_cast<std::size_t>(i) + 1U]; ++p) {
            const float *values = input
                + static_cast<std::ptrdiff_t>(plan.transpose_indices[p])
                    * input_row_stride + column;
            const Vector factor = Operations::broadcast(plan.transpose_weights[p]);
            sum0 = Operations::multiply_add(
                sum0, Operations::load(values), factor);
            if constexpr (TileVectors >= 2) {
                sum1 = Operations::multiply_add(
                    sum1, Operations::load(values + lanes), factor);
            }
            if constexpr (TileVectors >= 3) {
                sum2 = Operations::multiply_add(
                    sum2, Operations::load(values + 2 * lanes), factor);
            }
        }

        const std::int32_t available = std::min(half_bandwidth, i);
        if constexpr (FixedHalfBandwidth == 3) {
            if (available == 3) {
                {
                    const float *previous = output
                        + static_cast<std::ptrdiff_t>(i - 3)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.lower_ld[
                        2U * factor_row_width + static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_sub(
                        sum0, Operations::load(previous), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_sub(
                            sum1, Operations::load(previous + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_sub(
                            sum2, Operations::load(previous + 2 * lanes), factor);
                    }
                }
                {
                    const float *previous = output
                        + static_cast<std::ptrdiff_t>(i - 2)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.lower_ld[
                        factor_row_width + static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_sub(
                        sum0, Operations::load(previous), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_sub(
                            sum1, Operations::load(previous + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_sub(
                            sum2, Operations::load(previous + 2 * lanes), factor);
                    }
                }
                {
                    const float *previous = output
                        + static_cast<std::ptrdiff_t>(i - 1)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.lower_ld[
                        static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_sub(
                        sum0, Operations::load(previous), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_sub(
                            sum1, Operations::load(previous + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_sub(
                            sum2, Operations::load(previous + 2 * lanes), factor);
                    }
                }
            } else {
                for (std::int32_t distance = available; distance >= 1; --distance) {
                    const float *previous = output
                        + static_cast<std::ptrdiff_t>(i - distance)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.lower_ld[
                        static_cast<std::size_t>(distance - 1) * factor_row_width
                        + static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_sub(
                        sum0, Operations::load(previous), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_sub(
                            sum1, Operations::load(previous + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_sub(
                            sum2, Operations::load(previous + 2 * lanes), factor);
                    }
                }
            }
        } else {
            for (std::int32_t distance = available; distance >= 1; --distance) {
                const float *previous = output
                    + static_cast<std::ptrdiff_t>(i - distance)
                        * output_row_stride + column;
                const Vector factor = Operations::broadcast(plan.lower_ld[
                    static_cast<std::size_t>(distance - 1) * factor_row_width
                    + static_cast<std::size_t>(i)]);
                sum0 = Operations::multiply_sub(
                    sum0, Operations::load(previous), factor);
                if constexpr (TileVectors >= 2) {
                    sum1 = Operations::multiply_sub(
                        sum1, Operations::load(previous + lanes), factor);
                }
                if constexpr (TileVectors >= 3) {
                    sum2 = Operations::multiply_sub(
                        sum2, Operations::load(previous + 2 * lanes), factor);
                }
            }
        }
        const Vector diagonal = Operations::broadcast(
            plan.inverse_diagonal[static_cast<std::size_t>(i)]);
        float *current = output
            + static_cast<std::ptrdiff_t>(i) * output_row_stride + column;
        sum0 = Operations::multiply(sum0, diagonal);
        Operations::store(current, sum0);
        if constexpr (TileVectors >= 2) {
            sum1 = Operations::multiply(sum1, diagonal);
            Operations::store(current + lanes, sum1);
        }
        if constexpr (TileVectors >= 3) {
            sum2 = Operations::multiply(sum2, diagonal);
            Operations::store(current + 2 * lanes, sum2);
        }
    }

    for (std::int32_t i = destination_size - 2; i >= 0; --i) {
        sum0 = Operations::zero();
        if constexpr (TileVectors >= 2) sum1 = Operations::zero();
        if constexpr (TileVectors >= 3) sum2 = Operations::zero();
        const std::int32_t available = std::min(
            half_bandwidth, destination_size - i - 1);
        if constexpr (FixedHalfBandwidth == 3) {
            if (available == 3) {
                {
                    const float *next = output
                        + static_cast<std::ptrdiff_t>(i + 1)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.upper_l[
                        static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_add(
                        sum0, Operations::load(next), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_add(
                            sum1, Operations::load(next + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_add(
                            sum2, Operations::load(next + 2 * lanes), factor);
                    }
                }
                {
                    const float *next = output
                        + static_cast<std::ptrdiff_t>(i + 2)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.upper_l[
                        factor_row_width + static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_add(
                        sum0, Operations::load(next), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_add(
                            sum1, Operations::load(next + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_add(
                            sum2, Operations::load(next + 2 * lanes), factor);
                    }
                }
                {
                    const float *next = output
                        + static_cast<std::ptrdiff_t>(i + 3)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.upper_l[
                        2U * factor_row_width + static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_add(
                        sum0, Operations::load(next), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_add(
                            sum1, Operations::load(next + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_add(
                            sum2, Operations::load(next + 2 * lanes), factor);
                    }
                }
            } else {
                for (std::int32_t distance = 1; distance <= available; ++distance) {
                    const float *next = output
                        + static_cast<std::ptrdiff_t>(i + distance)
                            * output_row_stride + column;
                    const Vector factor = Operations::broadcast(plan.upper_l[
                        static_cast<std::size_t>(distance - 1) * factor_row_width
                        + static_cast<std::size_t>(i)]);
                    sum0 = Operations::multiply_add(
                        sum0, Operations::load(next), factor);
                    if constexpr (TileVectors >= 2) {
                        sum1 = Operations::multiply_add(
                            sum1, Operations::load(next + lanes), factor);
                    }
                    if constexpr (TileVectors >= 3) {
                        sum2 = Operations::multiply_add(
                            sum2, Operations::load(next + 2 * lanes), factor);
                    }
                }
            }
        } else {
            for (std::int32_t distance = available; distance >= 1; --distance) {
                const float *next = output
                    + static_cast<std::ptrdiff_t>(i + distance)
                        * output_row_stride + column;
                const Vector factor = Operations::broadcast(plan.upper_l[
                    static_cast<std::size_t>(distance - 1) * factor_row_width
                    + static_cast<std::size_t>(i)]);
                sum0 = Operations::multiply_add(
                    sum0, Operations::load(next), factor);
                if constexpr (TileVectors >= 2) {
                    sum1 = Operations::multiply_add(
                        sum1, Operations::load(next + lanes), factor);
                }
                if constexpr (TileVectors >= 3) {
                    sum2 = Operations::multiply_add(
                        sum2, Operations::load(next + 2 * lanes), factor);
                }
            }
        }
        float *current = output
            + static_cast<std::ptrdiff_t>(i) * output_row_stride + column;
        Operations::store(
            current, Operations::subtract(Operations::load(current), sum0));
        if constexpr (TileVectors >= 2) {
            Operations::store(
                current + lanes,
                Operations::subtract(Operations::load(current + lanes), sum1));
        }
        if constexpr (TileVectors >= 3) {
            Operations::store(
                current + 2 * lanes,
                Operations::subtract(Operations::load(current + 2 * lanes), sum2));
        }
    }
}

template <class Operations, std::int32_t FixedHalfBandwidth,
          std::int32_t TileVectors = Operations::tile_vectors>
void inverse_columns_x86_impl(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept {
    constexpr std::int32_t lanes = Operations::lanes;
    constexpr std::int32_t tile_vectors = TileVectors;
    static_assert(tile_vectors >= 1 && tile_vectors <= 3);
    constexpr std::int32_t tile_columns = lanes * tile_vectors;

    std::int32_t column = 0;
    for (; column <= column_count - tile_columns; column += tile_columns) {
        inverse_columns_x86_tile<Operations, FixedHalfBandwidth, tile_vectors>(
            plan, input, input_row_stride, output, output_row_stride, column);
    }
    if constexpr (tile_vectors > 1) {
        for (; column <= column_count - lanes; column += lanes) {
            inverse_columns_x86_tile<Operations, FixedHalfBandwidth, 1>(
                plan, input, input_row_stride, output, output_row_stride, column);
        }
    }

    for (; column < column_count; ++column) {
        inverse_axis_f32(plan, input + column, input_row_stride,
                         output + column, output_row_stride);
    }
}

template <class Operations>
void inverse_columns_x86(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept {
    if (plan.half_bandwidth == 1) {
        inverse_columns_x86_impl<Operations, 1>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
    } else if (plan.half_bandwidth == 3) {
        inverse_columns_x86_impl<Operations, 3>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
    } else {
        inverse_columns_x86_impl<Operations, 0>(
            plan, input, input_row_stride, output, output_row_stride, column_count);
    }
}

} // namespace getnative::detail

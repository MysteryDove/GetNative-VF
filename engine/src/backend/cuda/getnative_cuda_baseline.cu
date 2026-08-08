#include "cuda_baseline.hpp"

#include <cuda_runtime.h>

#include <cstdint>

namespace baseline = getnative::cuda_baseline;

namespace {

__device__ __forceinline__ std::uint32_t minimum(std::uint32_t left,
                                                 std::uint32_t right) {
    return left < right ? left : right;
}

template <std::uint32_t Bandwidth, bool UpperAscending>
__device__ __forceinline__ void inverse_axis_ring(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input,
    std::uint32_t input_stride,
    float *__restrict__ output,
    std::uint32_t output_stride) {
    const std::uint32_t size = plan.destination_size;
    float recent[Bandwidth]{};
    for (std::uint32_t index = 0U; index < size; ++index) {
        float sum = 0.0F;
        const std::uint32_t begin = transpose_offsets[
            plan.transpose_offsets_base + index];
        const std::uint32_t end = transpose_offsets[
            plan.transpose_offsets_base + index + 1U];
        for (std::uint32_t position = begin; position < end; ++position) {
            const std::uint32_t input_index = static_cast<std::uint32_t>(
                transpose_indices[plan.transpose_indices_base + position]);
            sum = fmaf(
                transpose_weights[plan.transpose_weights_base + position],
                input[input_index * input_stride], sum);
        }
        const std::uint32_t available = minimum(Bandwidth, index);
#pragma unroll
        for (std::uint32_t reverse = Bandwidth; reverse > 0U; --reverse) {
            if (reverse <= available) {
                const std::uint32_t factor = plan.lower_ld_base
                    + (reverse - 1U) * size + index;
                sum = fmaf(-lower_ld[factor], recent[reverse - 1U], sum);
            }
        }
        const float current =
            sum * inverse_diagonal[plan.inverse_diagonal_base + index];
        output[index * output_stride] = current;
#pragma unroll
        for (std::uint32_t distance = Bandwidth - 1U; distance > 0U; --distance) {
            recent[distance] = recent[distance - 1U];
        }
        recent[0] = current;
    }

    if (size < 2U) return;
    recent[0] = output[(size - 1U) * output_stride];
#pragma unroll
    for (std::uint32_t distance = 1U; distance < Bandwidth; ++distance) {
        recent[distance] = 0.0F;
    }
    for (std::uint32_t reverse_index = size - 1U;
         reverse_index > 0U; --reverse_index) {
        const std::uint32_t index = reverse_index - 1U;
        const std::uint32_t available = minimum(
            Bandwidth, size - index - 1U);
        float sum = 0.0F;
        if constexpr (UpperAscending) {
#pragma unroll
            for (std::uint32_t distance = 1U;
                 distance <= Bandwidth; ++distance) {
                if (distance <= available) {
                    const std::uint32_t factor = plan.upper_l_base
                        + (distance - 1U) * size + index;
                    sum = fmaf(upper_l[factor], recent[distance - 1U], sum);
                }
            }
        } else {
#pragma unroll
            for (std::uint32_t distance = Bandwidth;
                 distance > 0U; --distance) {
                if (distance <= available) {
                    const std::uint32_t factor = plan.upper_l_base
                        + (distance - 1U) * size + index;
                    sum = fmaf(upper_l[factor], recent[distance - 1U], sum);
                }
            }
        }
        const float current = output[index * output_stride] - sum;
        output[index * output_stride] = current;
#pragma unroll
        for (std::uint32_t distance = Bandwidth - 1U; distance > 0U; --distance) {
            recent[distance] = recent[distance - 1U];
        }
        recent[0] = current;
    }
}

template <std::uint32_t Bandwidth, bool UpperAscending>
__device__ __forceinline__ void inverse_axis_ring_pair(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input0,
    const float *__restrict__ input1,
    std::uint32_t input_stride,
    float *__restrict__ output0,
    float *__restrict__ output1,
    std::uint32_t output_stride,
    bool second_active) {
    const std::uint32_t size = plan.destination_size;
    float recent0[Bandwidth]{};
    float recent1[Bandwidth]{};
    for (std::uint32_t index = 0U; index < size; ++index) {
        float sum0 = 0.0F;
        float sum1 = 0.0F;
        const std::uint32_t begin = transpose_offsets[
            plan.transpose_offsets_base + index];
        const std::uint32_t end = transpose_offsets[
            plan.transpose_offsets_base + index + 1U];
        for (std::uint32_t position = begin; position < end; ++position) {
            const std::uint32_t input_index = static_cast<std::uint32_t>(
                transpose_indices[plan.transpose_indices_base + position]);
            const float weight =
                transpose_weights[plan.transpose_weights_base + position];
            sum0 = fmaf(weight, input0[input_index * input_stride], sum0);
            if (second_active) {
                sum1 = fmaf(weight, input1[input_index * input_stride], sum1);
            }
        }
        const std::uint32_t available = minimum(Bandwidth, index);
#pragma unroll
        for (std::uint32_t reverse = Bandwidth; reverse > 0U; --reverse) {
            if (reverse <= available) {
                const std::uint32_t factor_index = plan.lower_ld_base
                    + (reverse - 1U) * size + index;
                const float factor = lower_ld[factor_index];
                sum0 = fmaf(-factor, recent0[reverse - 1U], sum0);
                if (second_active) {
                    sum1 = fmaf(-factor, recent1[reverse - 1U], sum1);
                }
            }
        }
        const float diagonal =
            inverse_diagonal[plan.inverse_diagonal_base + index];
        const float current0 = sum0 * diagonal;
        const float current1 = second_active ? sum1 * diagonal : 0.0F;
        output0[index * output_stride] = current0;
        if (second_active) output1[index * output_stride] = current1;
#pragma unroll
        for (std::uint32_t distance = Bandwidth - 1U; distance > 0U; --distance) {
            recent0[distance] = recent0[distance - 1U];
            recent1[distance] = recent1[distance - 1U];
        }
        recent0[0] = current0;
        recent1[0] = current1;
    }

    if (size < 2U) return;
    recent0[0] = output0[(size - 1U) * output_stride];
    recent1[0] = second_active
        ? output1[(size - 1U) * output_stride] : 0.0F;
#pragma unroll
    for (std::uint32_t distance = 1U; distance < Bandwidth; ++distance) {
        recent0[distance] = 0.0F;
        recent1[distance] = 0.0F;
    }
    for (std::uint32_t reverse_index = size - 1U;
         reverse_index > 0U; --reverse_index) {
        const std::uint32_t index = reverse_index - 1U;
        const std::uint32_t available = minimum(
            Bandwidth, size - index - 1U);
        float sum0 = 0.0F;
        float sum1 = 0.0F;
        if constexpr (UpperAscending) {
#pragma unroll
            for (std::uint32_t distance = 1U;
                 distance <= Bandwidth; ++distance) {
                if (distance <= available) {
                    const std::uint32_t factor_index = plan.upper_l_base
                        + (distance - 1U) * size + index;
                    const float factor = upper_l[factor_index];
                    sum0 = fmaf(factor, recent0[distance - 1U], sum0);
                    if (second_active) {
                        sum1 = fmaf(factor, recent1[distance - 1U], sum1);
                    }
                }
            }
        } else {
#pragma unroll
            for (std::uint32_t distance = Bandwidth;
                 distance > 0U; --distance) {
                if (distance <= available) {
                    const std::uint32_t factor_index = plan.upper_l_base
                        + (distance - 1U) * size + index;
                    const float factor = upper_l[factor_index];
                    sum0 = fmaf(factor, recent0[distance - 1U], sum0);
                    if (second_active) {
                        sum1 = fmaf(factor, recent1[distance - 1U], sum1);
                    }
                }
            }
        }
        const float current0 = output0[index * output_stride] - sum0;
        const float current1 = second_active
            ? output1[index * output_stride] - sum1 : 0.0F;
        output0[index * output_stride] = current0;
        if (second_active) output1[index * output_stride] = current1;
#pragma unroll
        for (std::uint32_t distance = Bandwidth - 1U; distance > 0U; --distance) {
            recent0[distance] = recent0[distance - 1U];
            recent1[distance] = recent1[distance - 1U];
        }
        recent0[0] = current0;
        recent1[0] = current1;
    }
}

// Register-ring inverse for runtime half-bandwidths (up to 15). Identical
// operation order to the generic output-array recurrence in inverse_axis
// below, but the recurrence state lives in registers instead of re-reading
// output[] from global memory at every step — the wide-taps stall measured
// in docs/performance/e3-kernel-increments-20260808.md (inverse_horizontal
// at 9.1% SM, 0.32 waves/SM on lanczos8). The fixed 15-deep unrolled loops
// with runtime guards keep the window in registers for any band <= 15;
// half-bandwidths 1, 3, 5, and 7 keep their tighter specializations above.
constexpr std::uint32_t kRegisterRingMaxBandwidth = 15U;

__device__ __forceinline__ void inverse_axis_ring_dynamic(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input,
    std::uint32_t input_stride,
    float *__restrict__ output,
    std::uint32_t output_stride) {
    const std::uint32_t size = plan.destination_size;
    const std::uint32_t band = plan.half_bandwidth;
    float recent[kRegisterRingMaxBandwidth]{};
    for (std::uint32_t index = 0U; index < size; ++index) {
        float sum = 0.0F;
        const std::uint32_t begin = transpose_offsets[
            plan.transpose_offsets_base + index];
        const std::uint32_t end = transpose_offsets[
            plan.transpose_offsets_base + index + 1U];
        for (std::uint32_t position = begin; position < end; ++position) {
            const std::uint32_t input_index = static_cast<std::uint32_t>(
                transpose_indices[plan.transpose_indices_base + position]);
            sum = fmaf(
                transpose_weights[plan.transpose_weights_base + position],
                input[input_index * input_stride], sum);
        }
        const std::uint32_t available = minimum(band, index);
#pragma unroll
        for (std::uint32_t reverse = kRegisterRingMaxBandwidth;
             reverse > 0U; --reverse) {
            if (reverse <= available) {
                const std::uint32_t factor = plan.lower_ld_base
                    + (reverse - 1U) * size + index;
                sum = fmaf(-lower_ld[factor], recent[reverse - 1U], sum);
            }
        }
        const float current =
            sum * inverse_diagonal[plan.inverse_diagonal_base + index];
        output[index * output_stride] = current;
#pragma unroll
        for (std::uint32_t distance = kRegisterRingMaxBandwidth - 1U;
             distance > 0U; --distance) {
            recent[distance] = recent[distance - 1U];
        }
        recent[0] = current;
    }

    if (size < 2U) return;
    recent[0] = output[(size - 1U) * output_stride];
#pragma unroll
    for (std::uint32_t distance = 1U; distance < kRegisterRingMaxBandwidth;
         ++distance) {
        recent[distance] = 0.0F;
    }
    for (std::uint32_t reverse_index = size - 1U;
         reverse_index > 0U; --reverse_index) {
        const std::uint32_t index = reverse_index - 1U;
        const std::uint32_t available = minimum(band, size - index - 1U);
        float sum = 0.0F;
#pragma unroll
        for (std::uint32_t distance = kRegisterRingMaxBandwidth;
             distance > 0U; --distance) {
            if (distance <= available) {
                const std::uint32_t factor = plan.upper_l_base
                    + (distance - 1U) * size + index;
                sum = fmaf(upper_l[factor], recent[distance - 1U], sum);
            }
        }
        const float current = output[index * output_stride] - sum;
        output[index * output_stride] = current;
#pragma unroll
        for (std::uint32_t distance = kRegisterRingMaxBandwidth - 1U;
             distance > 0U; --distance) {
            recent[distance] = recent[distance - 1U];
        }
        recent[0] = current;
    }
}

__device__ __forceinline__ void inverse_axis_ring_pair_dynamic(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input0,
    const float *__restrict__ input1,
    std::uint32_t input_stride,
    float *__restrict__ output0,
    float *__restrict__ output1,
    std::uint32_t output_stride,
    bool second_active) {
    const std::uint32_t size = plan.destination_size;
    const std::uint32_t band = plan.half_bandwidth;
    float recent0[kRegisterRingMaxBandwidth]{};
    float recent1[kRegisterRingMaxBandwidth]{};
    for (std::uint32_t index = 0U; index < size; ++index) {
        float sum0 = 0.0F;
        float sum1 = 0.0F;
        const std::uint32_t begin = transpose_offsets[
            plan.transpose_offsets_base + index];
        const std::uint32_t end = transpose_offsets[
            plan.transpose_offsets_base + index + 1U];
        for (std::uint32_t position = begin; position < end; ++position) {
            const std::uint32_t input_index = static_cast<std::uint32_t>(
                transpose_indices[plan.transpose_indices_base + position]);
            const float weight =
                transpose_weights[plan.transpose_weights_base + position];
            sum0 = fmaf(weight, input0[input_index * input_stride], sum0);
            if (second_active) {
                sum1 = fmaf(weight, input1[input_index * input_stride], sum1);
            }
        }
        const std::uint32_t available = minimum(band, index);
#pragma unroll
        for (std::uint32_t reverse = kRegisterRingMaxBandwidth;
             reverse > 0U; --reverse) {
            if (reverse <= available) {
                const std::uint32_t factor_index = plan.lower_ld_base
                    + (reverse - 1U) * size + index;
                const float factor = lower_ld[factor_index];
                sum0 = fmaf(-factor, recent0[reverse - 1U], sum0);
                if (second_active) {
                    sum1 = fmaf(-factor, recent1[reverse - 1U], sum1);
                }
            }
        }
        const float diagonal =
            inverse_diagonal[plan.inverse_diagonal_base + index];
        const float current0 = sum0 * diagonal;
        const float current1 = second_active ? sum1 * diagonal : 0.0F;
        output0[index * output_stride] = current0;
        if (second_active) output1[index * output_stride] = current1;
#pragma unroll
        for (std::uint32_t distance = kRegisterRingMaxBandwidth - 1U;
             distance > 0U; --distance) {
            recent0[distance] = recent0[distance - 1U];
            recent1[distance] = recent1[distance - 1U];
        }
        recent0[0] = current0;
        recent1[0] = current1;
    }

    if (size < 2U) return;
    recent0[0] = output0[(size - 1U) * output_stride];
    recent1[0] = second_active
        ? output1[(size - 1U) * output_stride] : 0.0F;
#pragma unroll
    for (std::uint32_t distance = 1U; distance < kRegisterRingMaxBandwidth;
         ++distance) {
        recent0[distance] = 0.0F;
        recent1[distance] = 0.0F;
    }
    for (std::uint32_t reverse_index = size - 1U;
         reverse_index > 0U; --reverse_index) {
        const std::uint32_t index = reverse_index - 1U;
        const std::uint32_t available = minimum(band, size - index - 1U);
        float sum0 = 0.0F;
        float sum1 = 0.0F;
#pragma unroll
        for (std::uint32_t distance = kRegisterRingMaxBandwidth;
             distance > 0U; --distance) {
            if (distance <= available) {
                const std::uint32_t factor_index = plan.upper_l_base
                    + (distance - 1U) * size + index;
                const float factor = upper_l[factor_index];
                sum0 = fmaf(factor, recent0[distance - 1U], sum0);
                if (second_active) {
                    sum1 = fmaf(factor, recent1[distance - 1U], sum1);
                }
            }
        }
        const float current0 = output0[index * output_stride] - sum0;
        const float current1 = second_active
            ? output1[index * output_stride] - sum1 : 0.0F;
        output0[index * output_stride] = current0;
        if (second_active) output1[index * output_stride] = current1;
#pragma unroll
        for (std::uint32_t distance = kRegisterRingMaxBandwidth - 1U;
             distance > 0U; --distance) {
            recent0[distance] = recent0[distance - 1U];
            recent1[distance] = recent1[distance - 1U];
        }
        recent0[0] = current0;
        recent1[0] = current1;
    }
}

template <std::uint32_t Bandwidth, bool UpperAscending>
__device__ __forceinline__ void inverse_axis_ring_row_major_tiled(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input,
    std::uint32_t input_stride,
    float *__restrict__ output,
    std::uint32_t vector_index,
    std::uint32_t vector_count,
    float *tile) {
    constexpr std::uint32_t tile_width = 32U;
    constexpr std::uint32_t tile_pitch = tile_width + 1U;
    const std::uint32_t size = plan.destination_size;
    const bool active = vector_index < vector_count;
    float recent[Bandwidth]{};
    float last = 0.0F;

    for (std::uint32_t tile_base = 0U; tile_base < size;
         tile_base += tile_width) {
#pragma unroll
        for (std::uint32_t offset = 0U; offset < tile_width; ++offset) {
            const std::uint32_t index = tile_base + offset;
            if (active && index < size) {
                float sum = 0.0F;
                const std::uint32_t begin = transpose_offsets[
                    plan.transpose_offsets_base + index];
                const std::uint32_t end = transpose_offsets[
                    plan.transpose_offsets_base + index + 1U];
                for (std::uint32_t position = begin; position < end; ++position) {
                    const std::uint32_t input_index = static_cast<std::uint32_t>(
                        transpose_indices[plan.transpose_indices_base + position]);
                    sum = fmaf(
                        transpose_weights[plan.transpose_weights_base + position],
                        input[input_index * input_stride], sum);
                }
                const std::uint32_t available = minimum(Bandwidth, index);
#pragma unroll
                for (std::uint32_t reverse = Bandwidth; reverse > 0U; --reverse) {
                    if (reverse <= available) {
                        const std::uint32_t factor = plan.lower_ld_base
                            + (reverse - 1U) * size + index;
                        sum = fmaf(-lower_ld[factor], recent[reverse - 1U], sum);
                    }
                }
                const float current =
                    sum * inverse_diagonal[plan.inverse_diagonal_base + index];
                tile[threadIdx.x * tile_pitch + offset] = current;
                last = current;
#pragma unroll
                for (std::uint32_t distance = Bandwidth - 1U;
                     distance > 0U; --distance) {
                    recent[distance] = recent[distance - 1U];
                }
                recent[0] = current;
            }
        }
        __syncthreads();

        const std::uint32_t tile_elements = blockDim.x * tile_width;
        for (std::uint32_t linear = threadIdx.x; linear < tile_elements;
             linear += blockDim.x) {
            const std::uint32_t local_vector = linear / tile_width;
            const std::uint32_t offset = linear - local_vector * tile_width;
            const std::uint32_t destination_index = tile_base + offset;
            const std::uint32_t destination_vector =
                blockIdx.x * blockDim.x + local_vector;
            if (destination_vector < vector_count && destination_index < size) {
                output[destination_vector * size + destination_index] =
                    tile[local_vector * tile_pitch + offset];
            }
        }
        __syncthreads();
    }

    if (size < 2U) return;
    recent[0] = last;
#pragma unroll
    for (std::uint32_t distance = 1U; distance < Bandwidth; ++distance) {
        recent[distance] = 0.0F;
    }

    for (std::uint32_t tile_end = size - 1U; tile_end > 0U;) {
        const std::uint32_t tile_count = minimum(tile_width, tile_end);
        const std::uint32_t tile_begin = tile_end - tile_count;
        const std::uint32_t tile_elements = blockDim.x * tile_width;
        for (std::uint32_t linear = threadIdx.x; linear < tile_elements;
             linear += blockDim.x) {
            const std::uint32_t local_vector = linear / tile_width;
            const std::uint32_t offset = linear - local_vector * tile_width;
            const std::uint32_t source_vector =
                blockIdx.x * blockDim.x + local_vector;
            if (source_vector < vector_count && offset < tile_count) {
                tile[local_vector * tile_pitch + offset] =
                    output[source_vector * size + tile_begin + offset];
            }
        }
        __syncthreads();

        if (active) {
            for (std::uint32_t reverse = tile_end; reverse > tile_begin; --reverse) {
                const std::uint32_t index = reverse - 1U;
                const std::uint32_t available = minimum(
                    Bandwidth, size - index - 1U);
                float sum = 0.0F;
                if constexpr (UpperAscending) {
#pragma unroll
                    for (std::uint32_t distance = 1U;
                         distance <= Bandwidth; ++distance) {
                        if (distance <= available) {
                            const std::uint32_t factor = plan.upper_l_base
                                + (distance - 1U) * size + index;
                            sum = fmaf(
                                upper_l[factor], recent[distance - 1U], sum);
                        }
                    }
                } else {
#pragma unroll
                    for (std::uint32_t distance = Bandwidth;
                         distance > 0U; --distance) {
                        if (distance <= available) {
                            const std::uint32_t factor = plan.upper_l_base
                                + (distance - 1U) * size + index;
                            sum = fmaf(
                                upper_l[factor], recent[distance - 1U], sum);
                        }
                    }
                }
                const std::uint32_t offset = index - tile_begin;
                const float current =
                    tile[threadIdx.x * tile_pitch + offset] - sum;
                tile[threadIdx.x * tile_pitch + offset] = current;
#pragma unroll
                for (std::uint32_t distance = Bandwidth - 1U;
                     distance > 0U; --distance) {
                    recent[distance] = recent[distance - 1U];
                }
                recent[0] = current;
            }
        }
        __syncthreads();

        for (std::uint32_t linear = threadIdx.x; linear < tile_elements;
             linear += blockDim.x) {
            const std::uint32_t local_vector = linear / tile_width;
            const std::uint32_t offset = linear - local_vector * tile_width;
            const std::uint32_t destination_vector =
                blockIdx.x * blockDim.x + local_vector;
            if (destination_vector < vector_count && offset < tile_count) {
                output[destination_vector * size + tile_begin + offset] =
                    tile[local_vector * tile_pitch + offset];
            }
        }
        __syncthreads();
        tile_end = tile_begin;
    }
}

__device__ __forceinline__ void inverse_axis(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input,
    std::uint32_t input_stride,
    float *__restrict__ output,
    std::uint32_t output_stride) {
    if (plan.half_bandwidth == 5U) {
        inverse_axis_ring<5U, false>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input, input_stride, output, output_stride);
        return;
    }
    if (plan.half_bandwidth == 3U) {
        inverse_axis_ring<3U, true>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input, input_stride, output, output_stride);
        return;
    }
    // Exact-fit tiers for the other common bands: the dynamic 15-deep ring
    // pays 15-band guards per step, which made bilinear (band 1) measurably
    // slower per candidate than exact-fit bicubic (band 3) despite less
    // work. Band 7 covers spline64/lanczos4; band 1 is order-neutral
    // (single recurrence term). Both use the generic loop's descending
    // back-substitution order, so results stay bit-identical.
    if (plan.half_bandwidth == 7U) {
        inverse_axis_ring<7U, false>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input, input_stride, output, output_stride);
        return;
    }
    if (plan.half_bandwidth == 1U) {
        inverse_axis_ring<1U, false>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input, input_stride, output, output_stride);
        return;
    }
    if (plan.half_bandwidth <= kRegisterRingMaxBandwidth) {
        inverse_axis_ring_dynamic(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input, input_stride, output, output_stride);
        return;
    }
    const std::uint32_t size = plan.destination_size;
    for (std::uint32_t index = 0U; index < size; ++index) {
        float sum = 0.0F;
        const std::uint32_t begin = transpose_offsets[
            plan.transpose_offsets_base + index];
        const std::uint32_t end = transpose_offsets[
            plan.transpose_offsets_base + index + 1U];
        for (std::uint32_t position = begin; position < end; ++position) {
            const std::uint32_t input_index = static_cast<std::uint32_t>(
                transpose_indices[plan.transpose_indices_base + position]);
            sum = fmaf(
                transpose_weights[plan.transpose_weights_base + position],
                input[input_index * input_stride], sum);
        }
        const std::uint32_t available = minimum(plan.half_bandwidth, index);
        for (std::uint32_t distance = available; distance > 0U; --distance) {
            const std::uint32_t factor = plan.lower_ld_base
                + (distance - 1U) * size + index;
            sum = fmaf(-lower_ld[factor],
                       output[(index - distance) * output_stride], sum);
        }
        output[index * output_stride] =
            sum * inverse_diagonal[plan.inverse_diagonal_base + index];
    }

    if (size < 2U) return;
    for (std::uint32_t reverse = size - 1U; reverse > 0U; --reverse) {
        const std::uint32_t index = reverse - 1U;
        float sum = 0.0F;
        const std::uint32_t available = minimum(
            plan.half_bandwidth, size - index - 1U);
        if (plan.half_bandwidth == 3U) {
            for (std::uint32_t distance = 1U; distance <= available; ++distance) {
                const std::uint32_t factor = plan.upper_l_base
                    + (distance - 1U) * size + index;
                sum = fmaf(upper_l[factor],
                           output[(index + distance) * output_stride], sum);
            }
        } else {
            for (std::uint32_t distance = available; distance > 0U; --distance) {
                const std::uint32_t factor = plan.upper_l_base
                    + (distance - 1U) * size + index;
                sum = fmaf(upper_l[factor],
                           output[(index + distance) * output_stride], sum);
            }
        }
        output[index * output_stride] -= sum;
    }
}

__device__ __forceinline__ void inverse_axis_pair(
    const baseline::AxisPlanDescriptor &plan,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    const float *__restrict__ input0,
    const float *__restrict__ input1,
    std::uint32_t input_stride,
    float *__restrict__ output0,
    float *__restrict__ output1,
    std::uint32_t output_stride,
    bool second_active) {
    if (plan.half_bandwidth == 5U) {
        inverse_axis_ring_pair<5U, false>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input0, input1, input_stride,
            output0, output1, output_stride, second_active);
        return;
    }
    if (plan.half_bandwidth == 3U) {
        inverse_axis_ring_pair<3U, true>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input0, input1, input_stride,
            output0, output1, output_stride, second_active);
        return;
    }
    if (plan.half_bandwidth == 7U) {
        inverse_axis_ring_pair<7U, false>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input0, input1, input_stride,
            output0, output1, output_stride, second_active);
        return;
    }
    if (plan.half_bandwidth == 1U) {
        inverse_axis_ring_pair<1U, false>(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input0, input1, input_stride,
            output0, output1, output_stride, second_active);
        return;
    }
    if (plan.half_bandwidth <= kRegisterRingMaxBandwidth) {
        inverse_axis_ring_pair_dynamic(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input0, input1, input_stride,
            output0, output1, output_stride, second_active);
        return;
    }
    inverse_axis(
        plan, transpose_offsets, transpose_indices, transpose_weights,
        lower_ld, upper_l, inverse_diagonal,
        input0, input_stride, output0, output_stride);
    if (second_active) {
        inverse_axis(
            plan, transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            input1, input_stride, output1, output_stride);
    }
}

__device__ __forceinline__ float forward_point(
    const baseline::AxisPlanDescriptor &plan,
    std::uint32_t row,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    const float *__restrict__ input,
    std::uint32_t input_stride) {
    const std::int32_t left = forward_left[plan.forward_left_base + row];
    float sum = 0.0F;
    for (std::uint32_t tap = 0U; tap < plan.forward_width; ++tap) {
        sum = fmaf(
            forward_weights[
                plan.forward_weights_base + tap * plan.source_size + row],
            input[static_cast<std::uint32_t>(left + static_cast<std::int32_t>(tap))
                  * input_stride],
            sum);
    }
    return sum;
}

template <std::uint32_t Width>
__device__ __forceinline__ float forward_point_prefetched(
    const baseline::AxisPlanDescriptor &plan,
    std::uint32_t row,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    const float *__restrict__ input,
    std::uint32_t input_stride) {
    const std::int32_t left = forward_left[plan.forward_left_base + row];
    float weights[Width];
    float values[Width];
#pragma unroll
    for (std::uint32_t tap = 0U; tap < Width; ++tap) {
        weights[tap] = forward_weights[
            plan.forward_weights_base + tap * plan.source_size + row];
        values[tap] = input[
            static_cast<std::uint32_t>(left + static_cast<std::int32_t>(tap))
            * input_stride];
    }
    float sum = 0.0F;
#pragma unroll
    for (std::uint32_t tap = 0U; tap < Width; ++tap) {
        sum = fmaf(weights[tap], values[tap], sum);
    }
    return sum;
}

} // namespace

extern "C" __global__ void getnative_cuda_transpose_source(
    const float *__restrict__ source,
    std::uint32_t source_width,
    std::uint32_t source_height,
    float *__restrict__ transposed) {
    __shared__ float tile[32][33];
    const std::uint32_t x = blockIdx.x * 32U + threadIdx.x;
    const std::uint32_t y = blockIdx.y * 32U + threadIdx.y;
#pragma unroll
    for (std::uint32_t offset = 0U; offset < 32U; offset += 8U) {
        if (x < source_width && y + offset < source_height) {
            tile[threadIdx.y + offset][threadIdx.x] =
                source[(y + offset) * source_width + x];
        }
    }
    __syncthreads();

    const std::uint32_t output_x = blockIdx.y * 32U + threadIdx.x;
    const std::uint32_t output_y = blockIdx.x * 32U + threadIdx.y;
#pragma unroll
    for (std::uint32_t offset = 0U; offset < 32U; offset += 8U) {
        if (output_x < source_height && output_y + offset < source_width) {
            transposed[(output_y + offset) * source_height + output_x] =
                tile[threadIdx.x][threadIdx.y + offset];
        }
    }
}

extern "C" __global__ void getnative_cuda_inverse_horizontal(
    const float *__restrict__ transposed_source,
    std::uint32_t source_height,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    float *__restrict__ workspace) {
    const std::uint32_t candidate_index = blockIdx.y;
    const std::uint32_t row = blockIdx.x * blockDim.x + threadIdx.x;
    if (candidate_index >= candidate_count) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    if (candidate.axes == baseline::vertical_axes) return;

    const bool both = candidate.axes == baseline::both_axes;
    float *intermediate = workspace + candidate.workspace_base;
    if (both && candidate.horizontal.half_bandwidth == 5U) {
        extern __shared__ float tile[];
        inverse_axis_ring_row_major_tiled<5U, false>(
            candidate.horizontal,
            transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            transposed_source + (row < source_height ? row : 0U), source_height,
            intermediate, row, source_height, tile);
        return;
    }
    if (both && candidate.horizontal.half_bandwidth == 3U) {
        extern __shared__ float tile[];
        inverse_axis_ring_row_major_tiled<3U, true>(
            candidate.horizontal,
            transpose_offsets, transpose_indices, transpose_weights,
            lower_ld, upper_l, inverse_diagonal,
            transposed_source + (row < source_height ? row : 0U), source_height,
            intermediate, row, source_height, tile);
        return;
    }
    if (row >= source_height) return;
    inverse_axis(
        candidate.horizontal,
        transpose_offsets, transpose_indices, transpose_weights,
        lower_ld, upper_l, inverse_diagonal,
        transposed_source + row, source_height,
        both
            ? intermediate + row * candidate.horizontal.destination_size
            : intermediate + row,
        both ? 1U : source_height);
}

extern "C" __global__ void getnative_cuda_horizontal_fused(
    const float *__restrict__ transposed_source,
    std::uint32_t source_height,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    float *__restrict__ workspace,
    std::uint32_t crop_left,
    std::uint32_t crop_right,
    std::uint32_t crop_top,
    std::uint32_t crop_bottom,
    float threshold,
    double *__restrict__ row_sums) {
    const std::uint32_t candidate_index = blockIdx.y;
    const std::uint32_t row = blockIdx.x * blockDim.x + threadIdx.x;
    if (candidate_index >= candidate_count || row >= source_height) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    float *intermediate = workspace + candidate.workspace_base;
    inverse_axis(
        candidate.horizontal,
        transpose_offsets, transpose_indices, transpose_weights,
        lower_ld, upper_l, inverse_diagonal,
        transposed_source + row, source_height,
        intermediate + row, source_height);

    double sum = 0.0;
    if (row >= crop_top && row < source_height - crop_bottom) {
        const std::uint32_t column_end =
            candidate.horizontal.source_size - crop_right;
        for (std::uint32_t column = crop_left; column < column_end; ++column) {
            const float reconstructed =
                candidate.horizontal.forward_width == 6U
                ? forward_point_prefetched<6U>(
                    candidate.horizontal, column,
                    forward_left, forward_weights,
                    intermediate + row, source_height)
                : forward_point(
                    candidate.horizontal, column,
                    forward_left, forward_weights,
                    intermediate + row, source_height);
            const float difference = fabsf(
                transposed_source[column * source_height + row]
                - reconstructed);
            if (difference > threshold) sum += static_cast<double>(difference);
        }
    }
    row_sums[candidate_index * source_height + row] = sum;
}

extern "C" __global__ void getnative_cuda_inverse_vertical(
    const float *__restrict__ source,
    std::uint32_t source_width,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    float *__restrict__ workspace) {
    const std::uint32_t candidate_index = blockIdx.y;
    const std::uint32_t column = blockIdx.x * blockDim.x + threadIdx.x;
    if (candidate_index >= candidate_count) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    if (candidate.axes == baseline::horizontal_axes) return;

    const bool both = candidate.axes == baseline::both_axes;
    const std::uint32_t stride = both
        ? candidate.horizontal.destination_size : source_width;
    if (column >= stride) return;
    const float *input = both
        ? workspace + candidate.workspace_base
        : source;
    float *native = workspace + candidate.workspace_base
        + candidate.intermediate_elements;
    inverse_axis(
        candidate.vertical,
        transpose_offsets, transpose_indices, transpose_weights,
        lower_ld, upper_l, inverse_diagonal,
        input + column, stride, native + column, stride);
}

extern "C" __global__ void getnative_cuda_inverse_vertical_pair(
    const float *__restrict__ source,
    std::uint32_t source_width,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::int32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld,
    const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal,
    float *__restrict__ workspace) {
    const std::uint32_t candidate_index = blockIdx.y;
    constexpr std::uint32_t columns_per_thread = 2U;
    const std::uint32_t column0 =
        blockIdx.x * blockDim.x * columns_per_thread + threadIdx.x;
    const std::uint32_t column1 = column0 + blockDim.x;
    if (candidate_index >= candidate_count) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    if (candidate.axes == baseline::horizontal_axes) return;

    const bool both = candidate.axes == baseline::both_axes;
    const std::uint32_t stride = both
        ? candidate.horizontal.destination_size : source_width;
    if (column0 >= stride) return;
    const bool second_active = column1 < stride;
    const float *input = both
        ? workspace + candidate.workspace_base
        : source;
    float *native = workspace + candidate.workspace_base
        + candidate.intermediate_elements;
    inverse_axis_pair(
        candidate.vertical,
        transpose_offsets, transpose_indices, transpose_weights,
        lower_ld, upper_l, inverse_diagonal,
        input + column0, input + (second_active ? column1 : column0), stride,
        native + column0, native + (second_active ? column1 : column0), stride,
        second_active);
}

extern "C" __global__ void getnative_cuda_forward_intermediate(
    std::uint32_t source_width,
    std::uint32_t source_height,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    float *__restrict__ workspace) {
    const std::uint32_t candidate_index = blockIdx.y;
    const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
    if (candidate_index >= candidate_count) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    if (candidate.axes != baseline::both_axes) return;

    const std::uint32_t native_width = candidate.horizontal.destination_size;
    const std::uint32_t native_height = candidate.vertical.destination_size;
    float *intermediate = workspace + candidate.workspace_base;
    const float *native = intermediate + candidate.intermediate_elements;
    if (candidate.forward_order == baseline::vertical_first) {
        const std::uint32_t elements = source_height * native_width;
        if (index >= elements) return;
        const std::uint32_t row = index / native_width;
        const std::uint32_t column = index - row * native_width;
        intermediate[index] = forward_point(
            candidate.vertical, row, forward_left, forward_weights,
            native + column, native_width);
    } else {
        const std::uint32_t elements = native_height * source_width;
        if (index >= elements) return;
        const std::uint32_t row = index / source_width;
        const std::uint32_t column = index - row * source_width;
        intermediate[index] = forward_point(
            candidate.horizontal, column, forward_left, forward_weights,
            native + row * native_width, 1U);
    }
}

extern "C" __global__ void getnative_cuda_both_fused_metric(
    const float *__restrict__ source,
    std::uint32_t source_width,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    const float *__restrict__ workspace,
    std::uint32_t crop_left,
    std::uint32_t crop_right,
    std::uint32_t crop_top,
    float threshold,
    double *__restrict__ partials) {
    extern __shared__ float staged_vertical[];
    __shared__ double reduction[256];
    const std::uint32_t candidate_index = blockIdx.y;
    const std::uint32_t lane = threadIdx.x;
    if (candidate_index >= candidate_count) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    if (candidate.axes != baseline::both_axes
        || candidate.forward_order != baseline::vertical_first) return;

    constexpr std::uint32_t outputs_per_thread = 8U;
    const std::uint32_t row = crop_top + blockIdx.z;
    const std::uint32_t column_begin = crop_left
        + blockIdx.x * blockDim.x * outputs_per_thread;
    const std::uint32_t column_end = minimum(
        source_width - crop_right,
        column_begin + blockDim.x * outputs_per_thread);
    const baseline::AxisPlanDescriptor &horizontal = candidate.horizontal;
    const baseline::AxisPlanDescriptor &vertical = candidate.vertical;
    const std::int32_t native_begin = forward_left[
        horizontal.forward_left_base + column_begin];
    const std::int32_t native_end = forward_left[
        horizontal.forward_left_base + column_end - 1U]
        + static_cast<std::int32_t>(horizontal.forward_width);
    const std::uint32_t native_span = static_cast<std::uint32_t>(
        native_end - native_begin);
    const std::uint32_t native_width = horizontal.destination_size;
    const float *native = workspace + candidate.workspace_base
        + candidate.intermediate_elements;

    const std::int32_t vertical_left = forward_left[
        vertical.forward_left_base + row];
    constexpr std::uint32_t staging_vectors_per_thread = 3U;
    for (std::uint32_t offset0 = lane; offset0 < native_span;
         offset0 += blockDim.x * staging_vectors_per_thread) {
        const std::uint32_t offset1 = offset0 + blockDim.x;
        const std::uint32_t offset2 = offset1 + blockDim.x;
        float sum0 = 0.0F;
        float sum1 = 0.0F;
        float sum2 = 0.0F;
        for (std::uint32_t tap = 0U; tap < vertical.forward_width; ++tap) {
            const float weight = forward_weights[
                vertical.forward_weights_base
                + tap * vertical.source_size + row];
            const std::uint32_t input_row = static_cast<std::uint32_t>(
                vertical_left + static_cast<std::int32_t>(tap));
            const float *input = native + input_row * native_width
                + static_cast<std::uint32_t>(native_begin);
            sum0 = fmaf(weight, input[offset0], sum0);
            if (offset1 < native_span) {
                sum1 = fmaf(weight, input[offset1], sum1);
            }
            if (offset2 < native_span) {
                sum2 = fmaf(weight, input[offset2], sum2);
            }
        }
        staged_vertical[offset0] = sum0;
        if (offset1 < native_span) staged_vertical[offset1] = sum1;
        if (offset2 < native_span) staged_vertical[offset2] = sum2;
    }
    __syncthreads();

    double sum = 0.0;
    constexpr std::uint32_t output_vectors_per_thread = 2U;
    for (std::uint32_t column0 = column_begin + lane;
         column0 < column_end;
         column0 += blockDim.x * output_vectors_per_thread) {
        const std::uint32_t column1 = column0 + blockDim.x;
        const std::int32_t left0 = forward_left[
            horizontal.forward_left_base + column0];
        const std::int32_t left1 = column1 < column_end
            ? forward_left[horizontal.forward_left_base + column1] : 0;
        float reconstructed0 = 0.0F;
        float reconstructed1 = 0.0F;
        for (std::uint32_t tap = 0U; tap < horizontal.forward_width; ++tap) {
            reconstructed0 = fmaf(
                forward_weights[
                    horizontal.forward_weights_base
                    + tap * horizontal.source_size + column0],
                staged_vertical[
                    static_cast<std::uint32_t>(left0 - native_begin) + tap],
                reconstructed0);
            if (column1 < column_end) {
                reconstructed1 = fmaf(
                    forward_weights[
                        horizontal.forward_weights_base
                        + tap * horizontal.source_size + column1],
                    staged_vertical[
                        static_cast<std::uint32_t>(left1 - native_begin) + tap],
                    reconstructed1);
            }
        }
        const float difference0 = fabsf(
            source[row * source_width + column0] - reconstructed0);
        if (difference0 > threshold) sum += static_cast<double>(difference0);
        if (column1 < column_end) {
            const float difference1 = fabsf(
                source[row * source_width + column1] - reconstructed1);
            if (difference1 > threshold) sum += static_cast<double>(difference1);
        }
    }

    reduction[lane] = sum;
    __syncthreads();
    for (std::uint32_t offset = blockDim.x / 2U; offset > 0U; offset /= 2U) {
        if (lane < offset) reduction[lane] += reduction[lane + offset];
        __syncthreads();
    }
    if (lane == 0U) {
        const std::uint32_t partial_stride = gridDim.x * gridDim.z;
        const std::uint32_t partial_index = blockIdx.z * gridDim.x + blockIdx.x;
        partials[candidate_index * partial_stride + partial_index] = reduction[0];
    }
}

extern "C" __global__ void getnative_cuda_metric_partials(
    const float *__restrict__ source,
    const float *__restrict__ transposed_source,
    std::uint32_t source_width,
    std::uint32_t source_height,
    const baseline::CandidateDescriptor *__restrict__ candidates,
    std::uint32_t candidate_count,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    const float *__restrict__ workspace,
    std::uint32_t crop_left,
    std::uint32_t crop_right,
    std::uint32_t crop_top,
    std::uint32_t crop_bottom,
    float threshold,
    double *__restrict__ partials) {
    extern __shared__ double reduction[];
    const std::uint32_t candidate_index = blockIdx.y;
    const std::uint32_t lane = threadIdx.x;
    if (candidate_index >= candidate_count) return;
    const baseline::CandidateDescriptor &candidate = candidates[candidate_index];
    const float *intermediate = workspace + candidate.workspace_base;
    const float *native = intermediate + candidate.intermediate_elements;
    const std::uint32_t crop_width = source_width - crop_left - crop_right;
    const std::uint32_t crop_height = source_height - crop_top - crop_bottom;
    const std::uint32_t pixel_count = crop_width * crop_height;

    double sum = 0.0;
    if (candidate.axes == baseline::horizontal_axes) {
        const std::uint32_t local_row = blockIdx.x * blockDim.x + lane;
        if (local_row < crop_height) {
            const std::uint32_t row = crop_top + local_row;
            for (std::uint32_t column = crop_left;
                 column < source_width - crop_right; ++column) {
                const float reconstructed = forward_point(
                    candidate.horizontal, column,
                    forward_left, forward_weights,
                    intermediate + row, source_height);
                const float difference = fabsf(
                    transposed_source[column * source_height + row]
                    - reconstructed);
                if (difference > threshold) {
                    sum += static_cast<double>(difference);
                }
            }
        }
        reduction[lane] = sum;
        __syncthreads();
        for (std::uint32_t offset = blockDim.x / 2U;
             offset > 0U; offset /= 2U) {
            if (lane < offset) reduction[lane] += reduction[lane + offset];
            __syncthreads();
        }
        if (lane == 0U) {
            partials[candidate_index * gridDim.x + blockIdx.x] = reduction[0];
        }
        return;
    }

    for (std::uint32_t linear = blockIdx.x * blockDim.x + lane;
         linear < pixel_count;
         linear += gridDim.x * blockDim.x) {
        const std::uint32_t local_row = linear / crop_width;
        const std::uint32_t local_column = linear - local_row * crop_width;
        const std::uint32_t row = crop_top + local_row;
        const std::uint32_t column = crop_left + local_column;
        float reconstructed = 0.0F;
        if (candidate.axes == baseline::vertical_axes) {
            reconstructed = candidate.vertical.forward_width == 6U
                ? forward_point_prefetched<6U>(
                    candidate.vertical, row, forward_left, forward_weights,
                    native + column, source_width)
                : forward_point(
                    candidate.vertical, row, forward_left, forward_weights,
                    native + column, source_width);
        } else if (candidate.axes == baseline::horizontal_axes) {
            const std::uint32_t native_width = candidate.horizontal.destination_size;
            reconstructed = forward_point(
                candidate.horizontal, column, forward_left, forward_weights,
                intermediate + row * native_width, 1U);
        } else if (candidate.forward_order == baseline::vertical_first) {
            const std::uint32_t native_width = candidate.horizontal.destination_size;
            reconstructed = forward_point(
                candidate.horizontal, column, forward_left, forward_weights,
                intermediate + row * native_width, 1U);
        } else {
            reconstructed = forward_point(
                candidate.vertical, row, forward_left, forward_weights,
                intermediate + column, source_width);
        }
        const float difference = fabsf(
            source[row * source_width + column] - reconstructed);
        if (difference > threshold) sum += static_cast<double>(difference);
    }

    reduction[lane] = sum;
    __syncthreads();
    for (std::uint32_t offset = blockDim.x / 2U; offset > 0U; offset /= 2U) {
        if (lane < offset) reduction[lane] += reduction[lane + offset];
        __syncthreads();
    }
    if (lane == 0U) {
        partials[candidate_index * gridDim.x + blockIdx.x] = reduction[0];
    }
}

extern "C" __global__ void getnative_cuda_metric_finalize(
    const double *__restrict__ partials,
    std::uint32_t partial_count,
    std::uint32_t candidate_count,
    std::uint32_t pixel_count,
    double *__restrict__ results) {
    extern __shared__ double reduction[];
    const std::uint32_t candidate_index = blockIdx.x;
    const std::uint32_t lane = threadIdx.x;
    if (candidate_index >= candidate_count) return;
    double sum = 0.0;
    for (std::uint32_t index = lane; index < partial_count; index += blockDim.x) {
        sum += partials[candidate_index * partial_count + index];
    }
    reduction[lane] = sum;
    __syncthreads();
    for (std::uint32_t offset = blockDim.x / 2U; offset > 0U; offset /= 2U) {
        if (lane < offset) reduction[lane] += reduction[lane + offset];
        __syncthreads();
    }
    if (lane == 0U) {
        results[candidate_index] = reduction[0] / static_cast<double>(pixel_count);
    }
}

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace getnative_cuda_detail {

inline constexpr unsigned int horizontal_axis = 0U;
inline constexpr unsigned int reduction_width = 256U;

template <class Value>
__device__ __forceinline__ Value device_load(const Value *base,
                                              unsigned int index) {
    const auto address = reinterpret_cast<std::uintptr_t>(base)
        + static_cast<unsigned long long>(index) * sizeof(Value);
    return *reinterpret_cast<const Value *>(address);
}

template <class Value>
__device__ __forceinline__ void device_store(Value *base, unsigned int index,
                                              Value value) {
    const auto address = reinterpret_cast<std::uintptr_t>(base)
        + static_cast<unsigned long long>(index) * sizeof(Value);
    *reinterpret_cast<Value *>(address) = value;
}

struct __align__(16) AxisPlanDescriptor {
    std::uint32_t source_size;
    std::uint32_t destination_size;
    std::uint32_t half_bandwidth;
    std::uint32_t forward_width;
    std::uint32_t transpose_offsets_base;
    std::uint32_t transpose_entries_base;
    std::uint32_t lower_ld_base;
    std::uint32_t upper_l_base;
    std::uint32_t inverse_diagonal_base;
    std::uint32_t forward_left_base;
    std::uint32_t forward_weights_base;
    std::uint32_t workspace_base;
    std::uint32_t direction;
    std::uint32_t vector_count;
    std::uint32_t reserved_0;
    std::uint32_t reserved_1;
};

struct AnalysisJob {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t crop_left;
    std::uint32_t crop_right;
    std::uint32_t crop_top;
    std::uint32_t crop_bottom;
    float threshold;
    std::uint32_t groups_per_candidate;
    std::uint32_t candidate_count;
    std::uint32_t maximum_vector_count;
};

static_assert(sizeof(AxisPlanDescriptor) == 64);
static_assert(alignof(AxisPlanDescriptor) == 16);
static_assert(offsetof(AxisPlanDescriptor, workspace_base) == 44);
static_assert(offsetof(AxisPlanDescriptor, reserved_1) == 60);
static_assert(sizeof(AnalysisJob) == 40);
static_assert(offsetof(AnalysisJob, threshold) == 24);
static_assert(offsetof(AnalysisJob, maximum_vector_count) == 36);

__device__ __forceinline__ unsigned int minimum(unsigned int left,
                                                 unsigned int right) {
    return left < right ? left : right;
}

__device__ __forceinline__ unsigned int image_index(
    unsigned int direction, unsigned int vector, unsigned int axis_index,
    unsigned int width) {
    return direction == horizontal_axis ? vector * width + axis_index
                                        : axis_index * width + vector;
}

// Production path allows FP32 contraction/FMA. Do not force RN mul+add pairs.
__device__ __forceinline__ float math_multiply(float left, float right) {
    return left * right;
}

__device__ __forceinline__ float math_add(float left, float right) {
    return left + right;
}

__device__ __forceinline__ float math_subtract(float left, float right) {
    return left - right;
}

template <unsigned int FixedHalfBandwidth, bool Bandwidth7Order>
__device__ __forceinline__ void inverse_axis(
    const float *__restrict__ source, const AnalysisJob &job,
    const AxisPlanDescriptor *__restrict__ plans,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::uint32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld, const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal, float *__restrict__ workspace,
    unsigned int candidate, unsigned int vector) {
    if (candidate >= job.candidate_count) return;
    const AxisPlanDescriptor plan = device_load(plans, candidate);
    if (vector >= plan.vector_count) return;

    const bool horizontal = plan.direction == horizontal_axis;
    const unsigned int output_stride = horizontal ? 1U : plan.vector_count;
    const unsigned int output_base = plan.workspace_base
        + (horizontal ? vector * plan.destination_size : vector);
    const unsigned int half_bandwidth = FixedHalfBandwidth == 0U
        ? plan.half_bandwidth : FixedHalfBandwidth;

    for (unsigned int i = 0U; i < plan.destination_size; ++i) {
        float sum = 0.0F;
        const unsigned int begin = plan.transpose_entries_base
            + device_load(transpose_offsets, plan.transpose_offsets_base + i);
        const unsigned int end = plan.transpose_entries_base
            + device_load(transpose_offsets, plan.transpose_offsets_base + i + 1U);
        for (unsigned int entry = begin; entry < end; ++entry) {
            const float product = math_multiply(
                device_load(transpose_weights, entry),
                device_load(source, image_index(
                    plan.direction, vector,
                    device_load(transpose_indices, entry), job.width)));
            sum = math_add(sum, product);
        }
        const unsigned int available = minimum(half_bandwidth, i);
        for (unsigned int distance = available; distance >= 1U; --distance) {
            const float product = math_multiply(
                device_load(lower_ld, plan.lower_ld_base
                    + (distance - 1U) * plan.destination_size + i),
                device_load(workspace, output_base
                    + (i - distance) * output_stride));
            sum = math_subtract(sum, product);
        }
        device_store(workspace, output_base + i * output_stride, math_multiply(
            sum, device_load(inverse_diagonal, plan.inverse_diagonal_base + i)));
    }

    if (plan.destination_size < 2U) return;
    for (unsigned int i = plan.destination_size - 1U; i-- > 0U;) {
        float sum = 0.0F;
        const unsigned int available = minimum(
            half_bandwidth, plan.destination_size - i - 1U);
        if constexpr (Bandwidth7Order) {
            for (unsigned int distance = 1U; distance <= available; ++distance) {
                const float product = math_multiply(
                    device_load(upper_l, plan.upper_l_base
                        + (distance - 1U) * plan.destination_size + i),
                    device_load(workspace, output_base
                        + (i + distance) * output_stride));
                sum = math_add(sum, product);
            }
        } else if constexpr (FixedHalfBandwidth == 0U) {
            if (plan.half_bandwidth == 3U) {
                for (unsigned int distance = 1U; distance <= available;
                     ++distance) {
                    const float product = math_multiply(
                        device_load(upper_l, plan.upper_l_base
                            + (distance - 1U) * plan.destination_size + i),
                        device_load(workspace, output_base
                            + (i + distance) * output_stride));
                    sum = math_add(sum, product);
                }
            } else {
                for (unsigned int distance = available; distance >= 1U;
                     --distance) {
                    const float product = math_multiply(
                        device_load(upper_l, plan.upper_l_base
                            + (distance - 1U) * plan.destination_size + i),
                        device_load(workspace, output_base
                            + (i + distance) * output_stride));
                    sum = math_add(sum, product);
                }
            }
        } else {
            for (unsigned int distance = available; distance >= 1U;
                 --distance) {
                const float product = math_multiply(
                    device_load(upper_l, plan.upper_l_base
                        + (distance - 1U) * plan.destination_size + i),
                    device_load(workspace, output_base
                        + (i + distance) * output_stride));
                sum = math_add(sum, product);
            }
        }
        const unsigned int output_index = output_base + i * output_stride;
        device_store(workspace, output_index,
                     math_subtract(device_load(workspace, output_index), sum));
    }
}

template <unsigned int FixedHalfBandwidth, bool Bandwidth7Order>
__device__ __forceinline__ void inverse_axis_matrix(
    const AnalysisJob &job, const AxisPlanDescriptor *__restrict__ plans,
    const std::uint32_t *__restrict__ transpose_offsets,
    const std::uint32_t *__restrict__ transpose_indices,
    const float *__restrict__ transpose_weights,
    const float *__restrict__ lower_ld, const float *__restrict__ upper_l,
    const float *__restrict__ inverse_diagonal, float *__restrict__ workspace,
    unsigned int candidate, unsigned int vector) {
    if (candidate >= job.candidate_count) return;
    const AxisPlanDescriptor plan = device_load(plans, candidate);
    if (vector >= plan.vector_count) return;

    const unsigned int input_base = plan.reserved_0;
    const unsigned int output_base = plan.workspace_base + vector;
    const unsigned int stride = plan.vector_count;
    const unsigned int half_bandwidth = FixedHalfBandwidth == 0U
        ? plan.half_bandwidth : FixedHalfBandwidth;

    for (unsigned int i = 0U; i < plan.destination_size; ++i) {
        float sum = 0.0F;
        const unsigned int begin = plan.transpose_entries_base
            + device_load(transpose_offsets, plan.transpose_offsets_base + i);
        const unsigned int end = plan.transpose_entries_base
            + device_load(transpose_offsets, plan.transpose_offsets_base + i + 1U);
        for (unsigned int entry = begin; entry < end; ++entry) {
            const float product = math_multiply(
                device_load(transpose_weights, entry),
                device_load(workspace, input_base
                    + device_load(transpose_indices, entry) * stride + vector));
            sum = math_add(sum, product);
        }
        const unsigned int available = minimum(half_bandwidth, i);
        for (unsigned int distance = available; distance >= 1U; --distance) {
            const float product = math_multiply(
                device_load(lower_ld, plan.lower_ld_base
                    + (distance - 1U) * plan.destination_size + i),
                device_load(workspace, output_base + (i - distance) * stride));
            sum = math_subtract(sum, product);
        }
        device_store(workspace, output_base + i * stride, math_multiply(
            sum, device_load(inverse_diagonal, plan.inverse_diagonal_base + i)));
    }

    if (plan.destination_size < 2U) return;
    for (unsigned int i = plan.destination_size - 1U; i-- > 0U;) {
        float sum = 0.0F;
        const unsigned int available = minimum(
            half_bandwidth, plan.destination_size - i - 1U);
        if constexpr (Bandwidth7Order) {
            for (unsigned int distance = 1U; distance <= available;
                 ++distance) {
                const float product = math_multiply(
                    device_load(upper_l, plan.upper_l_base
                        + (distance - 1U) * plan.destination_size + i),
                    device_load(workspace, output_base + (i + distance) * stride));
                sum = math_add(sum, product);
            }
        } else if constexpr (FixedHalfBandwidth == 0U) {
            if (plan.half_bandwidth == 3U) {
                for (unsigned int distance = 1U; distance <= available;
                     ++distance) {
                    const float product = math_multiply(
                        device_load(upper_l, plan.upper_l_base
                            + (distance - 1U) * plan.destination_size + i),
                        device_load(workspace, output_base
                            + (i + distance) * stride));
                    sum = math_add(sum, product);
                }
            } else {
                for (unsigned int distance = available; distance >= 1U;
                     --distance) {
                    const float product = math_multiply(
                        device_load(upper_l, plan.upper_l_base
                            + (distance - 1U) * plan.destination_size + i),
                        device_load(workspace, output_base
                            + (i + distance) * stride));
                    sum = math_add(sum, product);
                }
            }
        } else {
            for (unsigned int distance = available; distance >= 1U;
                 --distance) {
                const float product = math_multiply(
                    device_load(upper_l, plan.upper_l_base
                        + (distance - 1U) * plan.destination_size + i),
                    device_load(workspace, output_base
                        + (i + distance) * stride));
                sum = math_add(sum, product);
            }
        }
        const unsigned int output_index = output_base + i * stride;
        device_store(workspace, output_index,
                     math_subtract(device_load(workspace, output_index), sum));
    }
}

template <unsigned int FixedForwardWidth>
__device__ __forceinline__ void forward_axis_matrix(
    const AnalysisJob &job, const AxisPlanDescriptor *__restrict__ plans,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights, float *__restrict__ workspace,
    unsigned int candidate, unsigned int vector) {
    if (candidate >= job.candidate_count) return;
    const AxisPlanDescriptor plan = device_load(plans, candidate);
    const bool horizontal = plan.direction == horizontal_axis;
    const unsigned int vector_count = horizontal ? plan.reserved_1 : plan.vector_count;
    if (vector >= vector_count) return;
    const unsigned int width = FixedForwardWidth == 0U
        ? plan.forward_width : FixedForwardWidth;
    const unsigned int stride = horizontal ? 1U : plan.vector_count;
    const unsigned int input_base =
        + (horizontal ? plan.reserved_0 + vector * plan.destination_size
                      : plan.workspace_base + vector);
    const unsigned int output_base =
        + (horizontal ? plan.workspace_base + vector * plan.source_size
                      : plan.reserved_0 + vector);

    for (unsigned int row = 0U; row < plan.source_size; ++row) {
        const unsigned int forward = plan.forward_weights_base
            + row * plan.forward_width;
        const unsigned int left = static_cast<unsigned int>(
            device_load(forward_left, plan.forward_left_base + row));
        float sum = 0.0F;
        for (unsigned int tap = 0U; tap < width; ++tap) {
            const float product = math_multiply(
                device_load(forward_weights, forward + tap),
                device_load(workspace, input_base + (left + tap) * stride));
            sum = math_add(sum, product);
        }
        device_store(workspace, output_base + row * stride, sum);
    }
}

template <unsigned int FixedForwardWidth, bool HorizontalFirstTwoAxis>
__device__ __forceinline__ void metric_axis_p1(
    const float *__restrict__ source, const AnalysisJob &job,
    const AxisPlanDescriptor *__restrict__ plans,
    const std::int32_t *__restrict__ forward_left,
    const float *__restrict__ forward_weights,
    const float *__restrict__ workspace, float *__restrict__ partials,
    float *reduction, unsigned int thread_index, unsigned int candidate,
    unsigned int part) {
    if (candidate >= job.candidate_count) return;
    const AxisPlanDescriptor plan = device_load(plans, candidate);
    const unsigned int cropped_width = job.width - job.crop_left - job.crop_right;
    const unsigned int cropped_height = job.height - job.crop_top - job.crop_bottom;
    const unsigned int lane = part * reduction_width + thread_index;
    const unsigned int raster_stride = job.groups_per_candidate * reduction_width;
    unsigned int stride_y = 0U;
    unsigned int stride_x = raster_stride;
    while (stride_x >= cropped_width) {
        stride_x -= cropped_width;
        ++stride_y;
    }
    unsigned int y_offset = 0U;
    unsigned int x_offset = lane;
    while (x_offset >= cropped_width) {
        x_offset -= cropped_width;
        ++y_offset;
    }
    const bool horizontal = plan.direction == horizontal_axis;
    float sum = 0.0F;

    while (y_offset < cropped_height) {
        const unsigned int x = job.crop_left + x_offset;
        const unsigned int y = job.crop_top + y_offset;
        const unsigned int axis_index = horizontal ? x : y;
        const unsigned int vector = horizontal ? y : x;
        const unsigned int forward = plan.forward_weights_base
            + axis_index * plan.forward_width;
        const unsigned int left = static_cast<unsigned int>(
            device_load(forward_left, plan.forward_left_base + axis_index));
        const unsigned int native_stride = horizontal ? 1U
            : (HorizontalFirstTwoAxis ? job.width : plan.vector_count);
        const unsigned int native_base = HorizontalFirstTwoAxis
            ? plan.reserved_0 : plan.workspace_base;
        const unsigned int native = native_base
            + (horizontal ? vector * plan.destination_size + left
                          : left * native_stride + vector);
        float reconstructed = 0.0F;
        const unsigned int width = FixedForwardWidth == 0U
            ? plan.forward_width : FixedForwardWidth;
        for (unsigned int tap = 0U; tap < width; ++tap) {
            const float product = math_multiply(
                device_load(forward_weights, forward + tap),
                device_load(workspace, native + tap * native_stride));
            reconstructed = math_add(reconstructed, product);
        }
        const float difference = fabsf(math_subtract(
            device_load(source, y * job.width + x), reconstructed));
        if (difference > job.threshold) sum = math_add(sum, difference);

        const unsigned int next_x = x_offset + stride_x;
        const bool carry = next_x >= cropped_width;
        x_offset = carry ? next_x - cropped_width : next_x;
        y_offset += stride_y + static_cast<unsigned int>(carry);
    }

    reduction[thread_index] = sum;
    __syncthreads();
    for (unsigned int offset = blockDim.x >> 1U; offset > 0U; offset >>= 1U) {
        if (thread_index < offset) {
            reduction[thread_index] = math_add(
                reduction[thread_index], reduction[thread_index + offset]);
        }
        __syncthreads();
    }
    if (thread_index == 0U) {
        device_store(partials, candidate * job.groups_per_candidate + part,
                     reduction[0U]);
    }
}

} // namespace getnative_cuda_detail

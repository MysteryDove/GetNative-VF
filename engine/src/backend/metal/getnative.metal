#include <metal_stdlib>

using namespace metal;

enum : uint {
    horizontal_axis = 0,
    reduction_width = 256,
};

struct AxisPlanDescriptor {
    uint source_size;
    uint destination_size;
    uint half_bandwidth;
    uint forward_width;
    uint transpose_offsets_base;
    uint transpose_entries_base;
    uint lower_ld_base;
    uint upper_l_base;
    uint inverse_diagonal_base;
    uint forward_left_base;
    uint forward_weights_base;
    uint workspace_base;
    uint direction;
    uint vector_count;
    uint reserved_0;
    uint reserved_1;
};

struct AnalysisJob {
    uint width;
    uint height;
    uint crop_left;
    uint crop_right;
    uint crop_top;
    uint crop_bottom;
    float threshold;
    uint groups_per_candidate;
    uint candidate_count;
    uint maximum_vector_count;
    uint norm;
};

struct LumaNormalizeJob {
    uint width;
    uint height;
    uint bit_depth;
    uint full_range;
};

kernel void normalize_luma_r8(
    texture2d<float, access::read> source [[texture(0)]],
    device float *destination [[buffer(0)]],
    constant LumaNormalizeJob &job [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= job.width || gid.y >= job.height) return;
    float value = source.read(gid).r;
    if (job.full_range == 0u) value = max(0.0f, (value * 255.0f - 16.0f) / 219.0f);
    destination[gid.y * job.width + gid.x] = clamp(value, 0.0f, 1.0f);
}

kernel void normalize_luma_r16(
    texture2d<float, access::read> source [[texture(0)]],
    device float *destination [[buffer(0)]],
    constant LumaNormalizeJob &job [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= job.width || gid.y >= job.height) return;
    // VideoToolbox 10-bit bi-planar samples are stored left-aligned in a
    // 16-bit plane. R16Unorm has already divided by 65535, so multiplying by
    // 1023 reconstructs the logical 10-bit code without a host conversion.
    float code = source.read(gid).r * 1023.0f;
    float value = code / 1023.0f;
    if (job.full_range == 0u) value = max(0.0f, (value * 1023.0f - 64.0f) / 876.0f);
    destination[gid.y * job.width + gid.x] = clamp(value, 0.0f, 1.0f);
}

static inline uint image_index(uint direction, uint vector, uint axis_index, uint width) {
    return direction == horizontal_axis ? vector * width + axis_index
                                        : axis_index * width + vector;
}

// fixed_half_bandwidth and bandwidth7_order are template constants so each
// specialized kernel (b3/b7/b11/b15) gets a lag window sized exactly to its
// bandwidth: substitution sweeps read back only the last fixed_half_bandwidth
// outputs, and a register window of that size replaces the device-memory
// reloads with zero excess shifting. Iteration order matches the memory-loop
// version exactly, keeping results bit-identical. The generic kernel
// instantiates <0, false> and keeps memory-loop substitution.
template <uint fixed_half_bandwidth, bool bandwidth7_order>
static inline void inverse_axis_impl(
    device const float *source,
    constant AnalysisJob &job,
    device const AxisPlanDescriptor *plans,
    device const uint *transpose_offsets,
    device const uint *transpose_indices,
    device const float *transpose_weights,
    device const float *lower_ld,
    device const float *upper_l,
    device const float *inverse_diagonal,
    device float *workspace, uint gid) {
    const uint candidate = gid / job.maximum_vector_count;
    const uint vector = gid - candidate * job.maximum_vector_count;
    if (candidate >= job.candidate_count) {
        return;
    }
    const AxisPlanDescriptor plan = plans[candidate];
    if (vector >= plan.vector_count) {
        return;
    }

    const bool horizontal = plan.direction == horizontal_axis;
    const uint output_stride = horizontal ? 1u : plan.vector_count;
    device float *output = workspace + plan.workspace_base
        + (horizontal ? vector * plan.destination_size : vector);
    const uint half_bandwidth = fixed_half_bandwidth == 0u
        ? plan.half_bandwidth : fixed_half_bandwidth;

    if constexpr (fixed_half_bandwidth != 0u) {
        // Register lag window sized to the compile-time bandwidth.
        float lag[fixed_half_bandwidth] = {};
        for (uint i = 0; i < plan.destination_size; ++i) {
            float sum = 0.0f;
            const uint begin = plan.transpose_entries_base
                + transpose_offsets[plan.transpose_offsets_base + i];
            const uint end = plan.transpose_entries_base
                + transpose_offsets[plan.transpose_offsets_base + i + 1];
            for (uint p = begin; p < end; ++p) {
                sum += transpose_weights[p]
                    * source[image_index(plan.direction, vector,
                                         transpose_indices[p], job.width)];
            }
            const uint available = min(fixed_half_bandwidth, i);
            #pragma unroll
            for (uint distance = fixed_half_bandwidth; distance >= 1; --distance) {
                if (distance <= available) {
                    sum -= lower_ld[plan.lower_ld_base
                                    + (distance - 1) * plan.destination_size + i]
                        * lag[distance - 1];
                }
            }
            const float current = sum
                * inverse_diagonal[plan.inverse_diagonal_base + i];
            output[i * output_stride] = current;
            #pragma unroll
            for (uint k = fixed_half_bandwidth - 1; k >= 1; --k) lag[k] = lag[k - 1];
            lag[0] = current;
        }

        if (plan.destination_size < 2) {
            return;
        }
        // Reverse window seeded with the forward sweep's final outputs;
        // entries past the end are never read (`available` guards them).
        float rlag[fixed_half_bandwidth];
        #pragma unroll
        for (uint k = 0; k < fixed_half_bandwidth; ++k) {
            const uint index = plan.destination_size - 1 + k;
            rlag[k] = index < plan.destination_size
                ? output[index * output_stride] : 0.0f;
        }
        for (uint i = plan.destination_size - 1; i-- > 0;) {
            float sum = 0.0f;
            const uint available = min(fixed_half_bandwidth, plan.destination_size - i - 1);
            if (bandwidth7_order) {
                // Descale's bandwidth-7 path accumulates backward near-to-far.
                #pragma unroll
                for (uint distance = 1; distance <= fixed_half_bandwidth; ++distance) {
                    if (distance <= available) {
                        sum += upper_l[plan.upper_l_base
                                       + (distance - 1) * plan.destination_size + i]
                            * rlag[distance - 1];
                    }
                }
            } else {
                // Bandwidth-3 and generic scalar paths accumulate far-to-near.
                #pragma unroll
                for (uint distance = fixed_half_bandwidth; distance >= 1; --distance) {
                    if (distance <= available) {
                        sum += upper_l[plan.upper_l_base
                                       + (distance - 1) * plan.destination_size + i]
                            * rlag[distance - 1];
                    }
                }
            }
            const float current = output[i * output_stride] - sum;
            output[i * output_stride] = current;
            #pragma unroll
            for (uint k = fixed_half_bandwidth - 1; k >= 1; --k) rlag[k] = rlag[k - 1];
            rlag[0] = current;
        }
        return;
    }

    for (uint i = 0; i < plan.destination_size; ++i) {
        float sum = 0.0f;
        const uint begin = plan.transpose_entries_base
            + transpose_offsets[plan.transpose_offsets_base + i];
        const uint end = plan.transpose_entries_base
            + transpose_offsets[plan.transpose_offsets_base + i + 1];
        for (uint p = begin; p < end; ++p) {
            sum += transpose_weights[p]
                * source[image_index(plan.direction, vector,
                                     transpose_indices[p], job.width)];
        }
        const uint available = min(half_bandwidth, i);
        for (uint distance = available; distance >= 1; --distance) {
            sum -= lower_ld[plan.lower_ld_base
                            + (distance - 1) * plan.destination_size + i]
                * output[(i - distance) * output_stride];
        }
        output[i * output_stride] = sum
            * inverse_diagonal[plan.inverse_diagonal_base + i];
    }

    if (plan.destination_size < 2) {
        return;
    }
    for (uint i = plan.destination_size - 1; i-- > 0;) {
        float sum = 0.0f;
        const uint available = min(half_bandwidth, plan.destination_size - i - 1);
        if (bandwidth7_order
            || (fixed_half_bandwidth == 0u && plan.half_bandwidth == 3u)) {
            // Descale's bandwidth-7 path accumulates backward near-to-far.
            for (uint distance = 1; distance <= available; ++distance) {
                sum += upper_l[plan.upper_l_base
                               + (distance - 1) * plan.destination_size + i]
                    * output[(i + distance) * output_stride];
            }
        } else {
            // Bandwidth-3 and generic scalar paths accumulate far-to-near.
            for (uint distance = available; distance >= 1; --distance) {
                sum += upper_l[plan.upper_l_base
                               + (distance - 1) * plan.destination_size + i]
                    * output[(i + distance) * output_stride];
            }
        }
        output[i * output_stride] -= sum;
    }
}

kernel void inverse_axis_b3(
    device const float *source [[buffer(0)]],
    constant AnalysisJob &job [[buffer(1)]],
    device const AxisPlanDescriptor *plans [[buffer(2)]],
    device const uint *transpose_offsets [[buffer(3)]],
    device const uint *transpose_indices [[buffer(4)]],
    device const float *transpose_weights [[buffer(5)]],
    device const float *lower_ld [[buffer(6)]],
    device const float *upper_l [[buffer(7)]],
    device const float *inverse_diagonal [[buffer(8)]],
    device float *workspace [[buffer(9)]],
    uint gid [[thread_position_in_grid]]) {
    inverse_axis_impl<1u, false>(source, job, plans, transpose_offsets, transpose_indices,
                      transpose_weights, lower_ld, upper_l, inverse_diagonal,
                      workspace, gid);
}

kernel void inverse_axis_b7(
    device const float *source [[buffer(0)]],
    constant AnalysisJob &job [[buffer(1)]],
    device const AxisPlanDescriptor *plans [[buffer(2)]],
    device const uint *transpose_offsets [[buffer(3)]],
    device const uint *transpose_indices [[buffer(4)]],
    device const float *transpose_weights [[buffer(5)]],
    device const float *lower_ld [[buffer(6)]],
    device const float *upper_l [[buffer(7)]],
    device const float *inverse_diagonal [[buffer(8)]],
    device float *workspace [[buffer(9)]],
    uint gid [[thread_position_in_grid]]) {
    inverse_axis_impl<3u, true>(source, job, plans, transpose_offsets, transpose_indices,
                      transpose_weights, lower_ld, upper_l, inverse_diagonal,
                      workspace, gid);
}

#define DEFINE_IMAGE_INVERSE_FIXED(NAME, HALF) \
kernel void NAME( \
    device const float *source [[buffer(0)]], \
    constant AnalysisJob &job [[buffer(1)]], \
    device const AxisPlanDescriptor *plans [[buffer(2)]], \
    device const uint *transpose_offsets [[buffer(3)]], \
    device const uint *transpose_indices [[buffer(4)]], \
    device const float *transpose_weights [[buffer(5)]], \
    device const float *lower_ld [[buffer(6)]], \
    device const float *upper_l [[buffer(7)]], \
    device const float *inverse_diagonal [[buffer(8)]], \
    device float *workspace [[buffer(9)]], \
    uint gid [[thread_position_in_grid]]) { \
    inverse_axis_impl<HALF, false>(source, job, plans, transpose_offsets, transpose_indices, \
        transpose_weights, lower_ld, upper_l, inverse_diagonal, workspace, gid); \
}

DEFINE_IMAGE_INVERSE_FIXED(inverse_axis_b11, 5u)
DEFINE_IMAGE_INVERSE_FIXED(inverse_axis_b15, 7u)

kernel void inverse_axis_generic(
    device const float *source [[buffer(0)]],
    constant AnalysisJob &job [[buffer(1)]],
    device const AxisPlanDescriptor *plans [[buffer(2)]],
    device const uint *transpose_offsets [[buffer(3)]],
    device const uint *transpose_indices [[buffer(4)]],
    device const float *transpose_weights [[buffer(5)]],
    device const float *lower_ld [[buffer(6)]],
    device const float *upper_l [[buffer(7)]],
    device const float *inverse_diagonal [[buffer(8)]],
    device float *workspace [[buffer(9)]],
    uint gid [[thread_position_in_grid]]) {
    inverse_axis_impl<0u, false>(source, job, plans, transpose_offsets, transpose_indices,
                      transpose_weights, lower_ld, upper_l, inverse_diagonal,
                      workspace, gid);
}

template <uint fixed_half_bandwidth, bool bandwidth7_order>
static inline void inverse_axis_matrix_impl(
    constant AnalysisJob &job,
    device const AxisPlanDescriptor *plans,
    device const uint *transpose_offsets,
    device const uint *transpose_indices,
    device const float *transpose_weights,
    device const float *lower_ld,
    device const float *upper_l,
    device const float *inverse_diagonal,
    device float *workspace, uint gid) {
    const uint candidate = gid / job.maximum_vector_count;
    const uint vector = gid - candidate * job.maximum_vector_count;
    if (candidate >= job.candidate_count) return;
    const AxisPlanDescriptor plan = plans[candidate];
    if (vector >= plan.vector_count) return;

    device const float *input = workspace + plan.reserved_0;
    device float *output = workspace + plan.workspace_base + vector;
    const uint stride = plan.vector_count;
    const uint half_bandwidth = fixed_half_bandwidth == 0u
        ? plan.half_bandwidth : fixed_half_bandwidth;

    if constexpr (fixed_half_bandwidth != 0u) {
        // Register lag window, mirroring the image inverse path: identical
        // iteration order, no output reloads from device memory.
        float lag[fixed_half_bandwidth] = {};
        for (uint i = 0; i < plan.destination_size; ++i) {
            float sum = 0.0f;
            const uint begin = plan.transpose_entries_base
                + transpose_offsets[plan.transpose_offsets_base + i];
            const uint end = plan.transpose_entries_base
                + transpose_offsets[plan.transpose_offsets_base + i + 1];
            for (uint p = begin; p < end; ++p) {
                sum += transpose_weights[p]
                    * input[transpose_indices[p] * stride + vector];
            }
            const uint available = min(fixed_half_bandwidth, i);
            #pragma unroll
            for (uint distance = fixed_half_bandwidth; distance >= 1; --distance) {
                if (distance <= available) {
                    sum -= lower_ld[plan.lower_ld_base
                                    + (distance - 1) * plan.destination_size + i]
                        * lag[distance - 1];
                }
            }
            const float current = sum
                * inverse_diagonal[plan.inverse_diagonal_base + i];
            output[i * stride] = current;
            #pragma unroll
            for (uint k = fixed_half_bandwidth - 1; k >= 1; --k) lag[k] = lag[k - 1];
            lag[0] = current;
        }
        if (plan.destination_size < 2) return;
        float rlag[fixed_half_bandwidth];
        #pragma unroll
        for (uint k = 0; k < fixed_half_bandwidth; ++k) {
            const uint index = plan.destination_size - 1 + k;
            rlag[k] = index < plan.destination_size
                ? output[index * stride] : 0.0f;
        }
        for (uint i = plan.destination_size - 1; i-- > 0;) {
            float sum = 0.0f;
            const uint available = min(fixed_half_bandwidth, plan.destination_size - i - 1);
            if (bandwidth7_order) {
                #pragma unroll
                for (uint distance = 1; distance <= fixed_half_bandwidth; ++distance) {
                    if (distance <= available) {
                        sum += upper_l[plan.upper_l_base
                                       + (distance - 1) * plan.destination_size + i]
                            * rlag[distance - 1];
                    }
                }
            } else {
                #pragma unroll
                for (uint distance = fixed_half_bandwidth; distance >= 1; --distance) {
                    if (distance <= available) {
                        sum += upper_l[plan.upper_l_base
                                       + (distance - 1) * plan.destination_size + i]
                            * rlag[distance - 1];
                    }
                }
            }
            const float current = output[i * stride] - sum;
            output[i * stride] = current;
            #pragma unroll
            for (uint k = fixed_half_bandwidth - 1; k >= 1; --k) rlag[k] = rlag[k - 1];
            rlag[0] = current;
        }
        return;
    }

    for (uint i = 0; i < plan.destination_size; ++i) {
        float sum = 0.0f;
        const uint begin = plan.transpose_entries_base
            + transpose_offsets[plan.transpose_offsets_base + i];
        const uint end = plan.transpose_entries_base
            + transpose_offsets[plan.transpose_offsets_base + i + 1];
        for (uint p = begin; p < end; ++p) {
            sum += transpose_weights[p]
                * input[transpose_indices[p] * stride + vector];
        }
        const uint available = min(half_bandwidth, i);
        for (uint distance = available; distance >= 1; --distance) {
            sum -= lower_ld[plan.lower_ld_base
                            + (distance - 1) * plan.destination_size + i]
                * output[(i - distance) * stride];
        }
        output[i * stride] = sum
            * inverse_diagonal[plan.inverse_diagonal_base + i];
    }
    if (plan.destination_size < 2) return;
    for (uint i = plan.destination_size - 1; i-- > 0;) {
        float sum = 0.0f;
        const uint available = min(half_bandwidth, plan.destination_size - i - 1);
        if (bandwidth7_order
            || (fixed_half_bandwidth == 0u && plan.half_bandwidth == 3u)) {
            for (uint distance = 1; distance <= available; ++distance) {
                sum += upper_l[plan.upper_l_base
                               + (distance - 1) * plan.destination_size + i]
                    * output[(i + distance) * stride];
            }
        } else {
            for (uint distance = available; distance >= 1; --distance) {
                sum += upper_l[plan.upper_l_base
                               + (distance - 1) * plan.destination_size + i]
                    * output[(i + distance) * stride];
            }
        }
        output[i * stride] -= sum;
    }
}

#define DEFINE_MATRIX_INVERSE(NAME, HALF, B7_ORDER) \
kernel void NAME( \
    constant AnalysisJob &job [[buffer(0)]], \
    device const AxisPlanDescriptor *plans [[buffer(1)]], \
    device const uint *transpose_offsets [[buffer(2)]], \
    device const uint *transpose_indices [[buffer(3)]], \
    device const float *transpose_weights [[buffer(4)]], \
    device const float *lower_ld [[buffer(5)]], \
    device const float *upper_l [[buffer(6)]], \
    device const float *inverse_diagonal [[buffer(7)]], \
    device float *workspace [[buffer(8)]], \
    uint gid [[thread_position_in_grid]]) { \
    inverse_axis_matrix_impl<HALF, B7_ORDER>(job, plans, transpose_offsets, transpose_indices, \
        transpose_weights, lower_ld, upper_l, inverse_diagonal, workspace, gid); \
}

DEFINE_MATRIX_INVERSE(inverse_axis_matrix_b3, 1u, false)
DEFINE_MATRIX_INVERSE(inverse_axis_matrix_b7, 3u, true)
DEFINE_MATRIX_INVERSE(inverse_axis_matrix_b11, 5u, false)
DEFINE_MATRIX_INVERSE(inverse_axis_matrix_b15, 7u, false)
DEFINE_MATRIX_INVERSE(inverse_axis_matrix_generic, 0u, false)

static inline void forward_axis_matrix_impl(
    constant AnalysisJob &job,
    device const AxisPlanDescriptor *plans,
    device const int *forward_left,
    device const float *forward_weights,
    device float *workspace, uint gid, uint fixed_forward_width) {
    const uint candidate = gid / job.maximum_vector_count;
    const uint vector = gid - candidate * job.maximum_vector_count;
    if (candidate >= job.candidate_count) return;
    const AxisPlanDescriptor plan = plans[candidate];
    const bool horizontal = plan.direction == horizontal_axis;
    const uint vector_count = horizontal ? plan.reserved_1 : plan.vector_count;
    if (vector >= vector_count) return;
    const uint width = fixed_forward_width == 0u ? plan.forward_width : fixed_forward_width;
    const uint input_stride = horizontal ? 1u : plan.vector_count;
    const uint output_stride = horizontal ? 1u : plan.vector_count;
    device const float *input = workspace
        + (horizontal ? plan.reserved_0 + vector * plan.destination_size
                      : plan.workspace_base + vector);
    device float *output = workspace
        + (horizontal ? plan.workspace_base + vector * plan.source_size
                      : plan.reserved_0 + vector);
    for (uint row = 0; row < plan.source_size; ++row) {
        const uint forward = plan.forward_weights_base + row * plan.forward_width;
        const int left = forward_left[plan.forward_left_base + row];
        float sum = 0.0f;
        for (uint tap = 0; tap < width; ++tap) {
            sum += forward_weights[forward + tap]
                * input[(uint(left) + tap) * input_stride];
        }
        output[row * output_stride] = sum;
    }
}

#define DEFINE_MATRIX_FORWARD(NAME, WIDTH) \
kernel void NAME( \
    constant AnalysisJob &job [[buffer(0)]], \
    device const AxisPlanDescriptor *plans [[buffer(1)]], \
    device const int *forward_left [[buffer(2)]], \
    device const float *forward_weights [[buffer(3)]], \
    device float *workspace [[buffer(4)]], \
    uint gid [[thread_position_in_grid]]) { \
    forward_axis_matrix_impl(job, plans, forward_left, forward_weights, workspace, gid, WIDTH); \
}

DEFINE_MATRIX_FORWARD(forward_axis_matrix_b3, 2u)
DEFINE_MATRIX_FORWARD(forward_axis_matrix_b7, 4u)
DEFINE_MATRIX_FORWARD(forward_axis_matrix_b11, 6u)
DEFINE_MATRIX_FORWARD(forward_axis_matrix_b15, 8u)
DEFINE_MATRIX_FORWARD(forward_axis_matrix_generic, 0u)

static inline void metric_axis_impl(
    device const float *source,
    constant AnalysisJob &job,
    device const AxisPlanDescriptor *plans,
    device const int *forward_left,
    device const float *forward_weights,
    device const float *workspace,
    device float *partials, threadgroup float *reduction,
    uint thread_index, uint group_index, uint fixed_forward_width,
    bool horizontal_first_two_axis) {
    const uint candidate = group_index / job.groups_per_candidate;
    const uint part = group_index - candidate * job.groups_per_candidate;
    if (candidate >= job.candidate_count) {
        return;
    }
    const AxisPlanDescriptor plan = plans[candidate];
    const uint cropped_width = job.width - job.crop_left - job.crop_right;
    const uint cropped_height = job.height - job.crop_top - job.crop_bottom;
    const uint lane = part * reduction_width + thread_index;
    const uint stride = job.groups_per_candidate * reduction_width;
    const uint stride_y = stride / cropped_width;
    const uint stride_x = stride - stride_y * cropped_width;
    uint y_offset = lane / cropped_width;
    uint x_offset = lane - y_offset * cropped_width;
        const bool horizontal = plan.direction == horizontal_axis;
    float sum = 0.0f;

    while (y_offset < cropped_height) {
        const uint x = job.crop_left + x_offset;
        const uint y = job.crop_top + y_offset;
        const uint axis_index = horizontal ? x : y;
        const uint vector = horizontal ? y : x;
        const uint forward = plan.forward_weights_base
            + axis_index * plan.forward_width;
        const int left = forward_left[plan.forward_left_base + axis_index];
        const uint native_stride = horizontal ? 1u
            : (horizontal_first_two_axis ? job.width : plan.vector_count);
        const uint native_base = horizontal_first_two_axis
            ? plan.reserved_0 : plan.workspace_base;
        const uint native = native_base
            + (horizontal ? vector * plan.destination_size + uint(left)
                          : uint(left) * native_stride + vector);
        float reconstructed = 0.0f;
        const uint forward_width = fixed_forward_width == 0u
            ? plan.forward_width : fixed_forward_width;
        for (uint tap = 0; tap < forward_width; ++tap) {
            reconstructed += forward_weights[forward + tap]
                * workspace[native + tap * native_stride];
        }
        const float difference = abs(source[y * job.width + x] - reconstructed);
        if (difference > job.threshold) {
            float moment = difference;
            if (job.norm == 2u) moment = difference * difference;
            if (job.norm == 3u) moment = difference * difference * difference;
            if (job.norm == 4u) {
                const float square = difference * difference;
                moment = square * square;
            }
            sum += moment;
        }

        const uint next_x = x_offset + stride_x;
        const bool carry = next_x >= cropped_width;
        x_offset = carry ? next_x - cropped_width : next_x;
        y_offset += stride_y + uint(carry);
    }

    reduction[thread_index] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint offset = reduction_width / 2; offset > 0; offset /= 2) {
        if (thread_index < offset) {
            reduction[thread_index] += reduction[thread_index + offset];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (thread_index == 0) {
        partials[candidate * job.groups_per_candidate + part] = reduction[0];
    }
}

kernel void metric_axis_b3(
    device const float *source [[buffer(0)]],
    constant AnalysisJob &job [[buffer(1)]],
    device const AxisPlanDescriptor *plans [[buffer(2)]],
    device const int *forward_left [[buffer(3)]],
    device const float *forward_weights [[buffer(4)]],
    device const float *workspace [[buffer(5)]],
    device float *partials [[buffer(6)]],
    uint thread_index [[thread_index_in_threadgroup]],
    uint group_index [[threadgroup_position_in_grid]]) {
    threadgroup float reduction[reduction_width];
    metric_axis_impl(source, job, plans, forward_left, forward_weights,
                        workspace, partials, reduction, thread_index, group_index, 2u, false);
}

kernel void metric_axis_b7(
    device const float *source [[buffer(0)]],
    constant AnalysisJob &job [[buffer(1)]],
    device const AxisPlanDescriptor *plans [[buffer(2)]],
    device const int *forward_left [[buffer(3)]],
    device const float *forward_weights [[buffer(4)]],
    device const float *workspace [[buffer(5)]],
    device float *partials [[buffer(6)]],
    uint thread_index [[thread_index_in_threadgroup]],
    uint group_index [[threadgroup_position_in_grid]]) {
    threadgroup float reduction[reduction_width];
    metric_axis_impl(source, job, plans, forward_left, forward_weights,
                        workspace, partials, reduction, thread_index, group_index, 4u, false);
}

#define DEFINE_AXIS_METRIC_FIXED(NAME, WIDTH) \
kernel void NAME( \
    device const float *source [[buffer(0)]], \
    constant AnalysisJob &job [[buffer(1)]], \
    device const AxisPlanDescriptor *plans [[buffer(2)]], \
    device const int *forward_left [[buffer(3)]], \
    device const float *forward_weights [[buffer(4)]], \
    device const float *workspace [[buffer(5)]], \
    device float *partials [[buffer(6)]], \
    uint thread_index [[thread_index_in_threadgroup]], \
    uint group_index [[threadgroup_position_in_grid]]) { \
    threadgroup float reduction[reduction_width]; \
    metric_axis_impl(source, job, plans, forward_left, forward_weights, \
        workspace, partials, reduction, thread_index, group_index, WIDTH, false); \
}

DEFINE_AXIS_METRIC_FIXED(metric_axis_b11, 6u)
DEFINE_AXIS_METRIC_FIXED(metric_axis_b15, 8u)

kernel void metric_axis_generic(
    device const float *source [[buffer(0)]],
    constant AnalysisJob &job [[buffer(1)]],
    device const AxisPlanDescriptor *plans [[buffer(2)]],
    device const int *forward_left [[buffer(3)]],
    device const float *forward_weights [[buffer(4)]],
    device const float *workspace [[buffer(5)]],
    device float *partials [[buffer(6)]],
    uint thread_index [[thread_index_in_threadgroup]],
    uint group_index [[threadgroup_position_in_grid]]) {
    threadgroup float reduction[reduction_width];
    metric_axis_impl(source, job, plans, forward_left, forward_weights,
                        workspace, partials, reduction, thread_index, group_index, 0u, false);
}

#define DEFINE_HORIZONTAL_FIRST_METRIC(NAME, WIDTH) \
kernel void NAME( \
    device const float *source [[buffer(0)]], \
    constant AnalysisJob &job [[buffer(1)]], \
    device const AxisPlanDescriptor *plans [[buffer(2)]], \
    device const int *forward_left [[buffer(3)]], \
    device const float *forward_weights [[buffer(4)]], \
    device const float *workspace [[buffer(5)]], \
    device float *partials [[buffer(6)]], \
    uint thread_index [[thread_index_in_threadgroup]], \
    uint group_index [[threadgroup_position_in_grid]]) { \
    threadgroup float reduction[reduction_width]; \
    metric_axis_impl(source, job, plans, forward_left, forward_weights, \
        workspace, partials, reduction, thread_index, group_index, WIDTH, true); \
}

DEFINE_HORIZONTAL_FIRST_METRIC(metric_axis_horizontal_first_b3, 2u)
DEFINE_HORIZONTAL_FIRST_METRIC(metric_axis_horizontal_first_b7, 4u)
DEFINE_HORIZONTAL_FIRST_METRIC(metric_axis_horizontal_first_b11, 6u)
DEFINE_HORIZONTAL_FIRST_METRIC(metric_axis_horizontal_first_b15, 8u)
DEFINE_HORIZONTAL_FIRST_METRIC(metric_axis_horizontal_first_generic, 0u)

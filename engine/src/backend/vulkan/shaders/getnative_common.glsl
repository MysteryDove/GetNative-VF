#ifndef GETNATIVE_VULKAN_COMMON_GLSL
#define GETNATIVE_VULKAN_COMMON_GLSL

// Production math path: ordinary Float32 ops; compiler contraction/FMA allowed.
// Do not reintroduce GLSL `precise` / NoContraction dual math modes.

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
    uint intermediate_or_native_base;
    uint native_height;
};

layout(push_constant, std430) uniform AnalysisJobBlock {
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
} job;

layout(set = 0, binding = 0, std430) readonly buffer SourceBuffer {
    float source_data[];
};
layout(set = 0, binding = 1, std430) readonly buffer DescriptorBuffer {
    AxisPlanDescriptor plans[];
};
layout(set = 0, binding = 2, std430) readonly buffer TransposeOffsetBuffer {
    uint transpose_offsets[];
};
layout(set = 0, binding = 3, std430) readonly buffer TransposeIndexBuffer {
    uint transpose_indices[];
};
layout(set = 0, binding = 4, std430) readonly buffer TransposeWeightBuffer {
    float transpose_weights[];
};
layout(set = 0, binding = 5, std430) readonly buffer LowerBuffer {
    float lower_ld[];
};
layout(set = 0, binding = 6, std430) readonly buffer UpperBuffer {
    float upper_l[];
};
layout(set = 0, binding = 7, std430) readonly buffer DiagonalBuffer {
    float inverse_diagonal[];
};
layout(set = 0, binding = 8, std430) readonly buffer ForwardLeftBuffer {
    int forward_left[];
};
layout(set = 0, binding = 9, std430) readonly buffer ForwardWeightBuffer {
    float forward_weights[];
};
layout(set = 0, binding = 10, std430) buffer WorkspaceBuffer {
    float workspace[];
};
layout(set = 0, binding = 11, std430) buffer PartialBuffer {
    float partials[];
};

const uint horizontal_axis = 0u;

uint source_image_index(uint direction, uint vector, uint axis_index) {
    return direction == horizontal_axis
        ? vector * job.width + axis_index
        : axis_index * job.width + vector;
}

#endif

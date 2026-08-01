#include "getnative_cuda_kernels.cuh"

using namespace getnative_cuda_detail;

#define GETNATIVE_IMAGE_INVERSE_KERNEL(NAME, HALF, B7_ORDER) \
extern "C" __global__ void NAME( \
    const float *source, AnalysisJob job, const AxisPlanDescriptor *plans, \
    const std::uint32_t *transpose_offsets, const std::uint32_t *transpose_indices, \
    const float *transpose_weights, const float *lower_ld, const float *upper_l, \
    const float *inverse_diagonal, float *workspace) { \
    const unsigned int candidate = blockIdx.y; \
    const unsigned int vector = blockIdx.x * blockDim.x + threadIdx.x; \
    inverse_axis<HALF, B7_ORDER>(source, job, plans, transpose_offsets, \
        transpose_indices, transpose_weights, lower_ld, upper_l, \
        inverse_diagonal, workspace, candidate, vector); \
}

GETNATIVE_IMAGE_INVERSE_KERNEL(inverse_axis_generic, 0U, false)
GETNATIVE_IMAGE_INVERSE_KERNEL(inverse_axis_b3, 1U, false)
GETNATIVE_IMAGE_INVERSE_KERNEL(inverse_axis_b7, 3U, true)
GETNATIVE_IMAGE_INVERSE_KERNEL(inverse_axis_b11, 5U, false)
GETNATIVE_IMAGE_INVERSE_KERNEL(inverse_axis_b15, 7U, false)

#define GETNATIVE_MATRIX_INVERSE_KERNEL(NAME, HALF, B7_ORDER) \
extern "C" __global__ void NAME( \
    AnalysisJob job, const AxisPlanDescriptor *plans, \
    const std::uint32_t *transpose_offsets, const std::uint32_t *transpose_indices, \
    const float *transpose_weights, const float *lower_ld, const float *upper_l, \
    const float *inverse_diagonal, float *workspace) { \
    const unsigned int candidate = blockIdx.y; \
    const unsigned int vector = blockIdx.x * blockDim.x + threadIdx.x; \
    inverse_axis_matrix<HALF, B7_ORDER>(job, plans, transpose_offsets, \
        transpose_indices, transpose_weights, lower_ld, upper_l, \
        inverse_diagonal, workspace, candidate, vector); \
}

GETNATIVE_MATRIX_INVERSE_KERNEL(inverse_axis_matrix_generic, 0U, false)
GETNATIVE_MATRIX_INVERSE_KERNEL(inverse_axis_matrix_b3, 1U, false)
GETNATIVE_MATRIX_INVERSE_KERNEL(inverse_axis_matrix_b7, 3U, true)
GETNATIVE_MATRIX_INVERSE_KERNEL(inverse_axis_matrix_b11, 5U, false)
GETNATIVE_MATRIX_INVERSE_KERNEL(inverse_axis_matrix_b15, 7U, false)

#define GETNATIVE_MATRIX_FORWARD_KERNEL(NAME, WIDTH) \
extern "C" __global__ void NAME( \
    AnalysisJob job, const AxisPlanDescriptor *plans, \
    const std::int32_t *forward_left, const float *forward_weights, \
    float *workspace) { \
    const unsigned int candidate = blockIdx.y; \
    const unsigned int vector = blockIdx.x * blockDim.x + threadIdx.x; \
    forward_axis_matrix<WIDTH>(job, plans, forward_left, forward_weights, workspace, \
        candidate, vector); \
}

GETNATIVE_MATRIX_FORWARD_KERNEL(forward_axis_matrix_generic, 0U)
GETNATIVE_MATRIX_FORWARD_KERNEL(forward_axis_matrix_b3, 2U)
GETNATIVE_MATRIX_FORWARD_KERNEL(forward_axis_matrix_b7, 4U)
GETNATIVE_MATRIX_FORWARD_KERNEL(forward_axis_matrix_b11, 6U)
GETNATIVE_MATRIX_FORWARD_KERNEL(forward_axis_matrix_b15, 8U)

#define GETNATIVE_METRIC_KERNEL(NAME, WIDTH, HORIZONTAL_FIRST) \
extern "C" __global__ void NAME( \
    const float *source, AnalysisJob job, const AxisPlanDescriptor *plans, \
    const std::int32_t *forward_left, const float *forward_weights, \
    const float *workspace, float *partials) { \
    __shared__ float reduction[reduction_width]; \
    metric_axis_p1<WIDTH, HORIZONTAL_FIRST>(source, job, plans, forward_left, \
        forward_weights, workspace, partials, reduction, threadIdx.x, blockIdx.y, \
        blockIdx.x); \
}

GETNATIVE_METRIC_KERNEL(metric_axis_p1_generic, 0U, false)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_b3, 2U, false)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_b7, 4U, false)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_b11, 6U, false)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_b15, 8U, false)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_horizontal_first_generic, 0U, true)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_horizontal_first_b3, 2U, true)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_horizontal_first_b7, 4U, true)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_horizontal_first_b11, 6U, true)
GETNATIVE_METRIC_KERNEL(metric_axis_p1_horizontal_first_b15, 8U, true)

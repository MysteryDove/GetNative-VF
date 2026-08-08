#include "inverse_columns.hpp"

#include <stdexcept>

namespace getnative::detail {

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
void inverse_columns_neon_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride, std::int32_t column_count) noexcept;
#endif
#if GETNATIVE_X86_SSE2_COMPILED
void inverse_columns_sse2_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept;
void absolute_difference_sse2_f32(
    const float *source, const float *reconstruction, float *differences) noexcept;
double absolute_difference_norm1_sse2_f32(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
void vertical_reconstruction_sse2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept;
double vertical_reconstruction_norm1_sse2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
#endif
#if GETNATIVE_X86_AVX2_COMPILED
void inverse_columns_avx2_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept;
void absolute_difference_avx2_f32(
    const float *source, const float *reconstruction, float *differences) noexcept;
double absolute_difference_norm1_avx2_f32(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
void vertical_reconstruction_avx2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept;
double vertical_reconstruction_norm1_avx2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
#endif
#if GETNATIVE_X86_AVX512_COMPILED
void inverse_columns_avx512_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept;
void absolute_difference_avx512_f32(
    const float *source, const float *reconstruction, float *differences) noexcept;
double absolute_difference_norm1_avx512_f32(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
void vertical_reconstruction_avx512_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept;
double vertical_reconstruction_norm1_avx512_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept;
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

[[nodiscard]] CpuIsaRequest policy_request(ColumnDispatchPolicy policy) noexcept {
    switch (policy) {
    case ColumnDispatchPolicy::automatic:
    case ColumnDispatchPolicy::required_simd:
        return CpuIsaRequest::automatic;
    case ColumnDispatchPolicy::scalar_only:
        return CpuIsaRequest::scalar;
    case ColumnDispatchPolicy::sse2_strict:
        return CpuIsaRequest::sse2;
    case ColumnDispatchPolicy::avx2_strict:
        return CpuIsaRequest::avx2;
    case ColumnDispatchPolicy::avx512_strict:
        return CpuIsaRequest::avx512;
    }
    return CpuIsaRequest::scalar;
}

[[nodiscard]] CpuIsa resolve_x86_policy(ColumnDispatchPolicy policy) {
    const CpuIsa selected = require_cpu_isa(policy_request(policy));
    if (policy == ColumnDispatchPolicy::required_simd && selected == CpuIsa::scalar) {
        throw std::runtime_error("required adjacent-column SIMD is unavailable");
    }
    return selected;
}

} // namespace

bool column_simd_available() noexcept {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return true;
#else
    return cpu_dispatch_info().selected != CpuIsa::scalar;
#endif
}

std::string_view column_simd_name() noexcept {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return "neon-f32x8";
#else
    return cpu_isa_name(cpu_dispatch_info().selected);
#endif
}

ColumnDispatchPolicy column_dispatch_policy(CpuIsaRequest request) noexcept {
    switch (request) {
    case CpuIsaRequest::automatic: return ColumnDispatchPolicy::automatic;
    case CpuIsaRequest::scalar: return ColumnDispatchPolicy::scalar_only;
    case CpuIsaRequest::sse2: return ColumnDispatchPolicy::sse2_strict;
    case CpuIsaRequest::avx2: return ColumnDispatchPolicy::avx2_strict;
    case CpuIsaRequest::avx512: return ColumnDispatchPolicy::avx512_strict;
    }
    return ColumnDispatchPolicy::scalar_only;
}

void validate_column_dispatch_policy(ColumnDispatchPolicy policy) {
    if (policy == ColumnDispatchPolicy::scalar_only) return;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (policy == ColumnDispatchPolicy::automatic
        || policy == ColumnDispatchPolicy::required_simd) {
        return;
    }
#endif
    (void)resolve_x86_policy(policy);
}

AnalysisRowDispatch analysis_row_dispatch(ColumnDispatchPolicy policy) {
    if (policy == ColumnDispatchPolicy::scalar_only) return {};
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (policy == ColumnDispatchPolicy::automatic
        || policy == ColumnDispatchPolicy::required_simd) {
        return {};
    }
#endif
    switch (resolve_x86_policy(policy)) {
    case CpuIsa::scalar:
        return {};
    case CpuIsa::sse2:
#if GETNATIVE_X86_SSE2_COMPILED
        return {4, absolute_difference_sse2_f32, absolute_difference_norm1_sse2_f32,
                vertical_reconstruction_sse2_f32, vertical_reconstruction_norm1_sse2_f32};
#else
        break;
#endif
    case CpuIsa::avx2:
#if GETNATIVE_X86_AVX2_COMPILED
        return {8, absolute_difference_avx2_f32, absolute_difference_norm1_avx2_f32,
                vertical_reconstruction_avx2_f32, vertical_reconstruction_norm1_avx2_f32};
#else
        break;
#endif
    case CpuIsa::avx512:
#if GETNATIVE_X86_AVX512_COMPILED
        return {16, absolute_difference_avx512_f32, absolute_difference_norm1_avx512_f32,
                vertical_reconstruction_avx512_f32, vertical_reconstruction_norm1_avx512_f32};
#else
        break;
#endif
    }
    throw std::runtime_error("selected CPU ISA has no compiled row kernel");
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
    if (policy == ColumnDispatchPolicy::scalar_only) {
        inverse_columns_scalar_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (policy == ColumnDispatchPolicy::automatic
        || policy == ColumnDispatchPolicy::required_simd) {
        inverse_columns_neon_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    }
#endif
    switch (resolve_x86_policy(policy)) {
    case CpuIsa::scalar:
        inverse_columns_scalar_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
    case CpuIsa::sse2:
#if GETNATIVE_X86_SSE2_COMPILED
        inverse_columns_sse2_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
#else
        break;
#endif
    case CpuIsa::avx2:
#if GETNATIVE_X86_AVX2_COMPILED
        inverse_columns_avx2_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
#else
        break;
#endif
    case CpuIsa::avx512:
#if GETNATIVE_X86_AVX512_COMPILED
        inverse_columns_avx512_f32(
            plan, input, input_row_stride, output, output_row_stride, column_count);
        return;
#else
        break;
#endif
    }
    throw std::runtime_error("selected CPU ISA has no compiled column kernel");
}

} // namespace getnative::detail

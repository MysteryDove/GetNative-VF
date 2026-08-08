#include "inverse_columns_x86_impl.hpp"

#include <immintrin.h>

namespace getnative::detail {
namespace {

struct Sse2Operations {
    using Vector = __m128;
    static constexpr std::int32_t lanes = 4;
    static constexpr std::int32_t tile_vectors = 1;

    [[nodiscard]] static Vector zero() noexcept { return _mm_setzero_ps(); }
    [[nodiscard]] static Vector broadcast(float value) noexcept {
        return _mm_set1_ps(value);
    }
    [[nodiscard]] static Vector load(const float *value) noexcept {
        return _mm_loadu_ps(value);
    }
    static void store(float *destination, Vector value) noexcept {
        _mm_storeu_ps(destination, value);
    }
    [[nodiscard]] static Vector multiply(Vector value, Vector factor) noexcept {
        return _mm_mul_ps(value, factor);
    }
    [[nodiscard]] static Vector add(Vector left, Vector right) noexcept {
        return _mm_add_ps(left, right);
    }
    [[nodiscard]] static Vector subtract(Vector left, Vector right) noexcept {
        return _mm_sub_ps(left, right);
    }
    // SSE2 has no hardware FMA; keep fused math as mul+add for this tier only.
    [[nodiscard]] static Vector multiply_add(
        Vector acc, Vector value, Vector factor) noexcept {
        return add(acc, multiply(value, factor));
    }
    [[nodiscard]] static Vector multiply_sub(
        Vector acc, Vector value, Vector factor) noexcept {
        return subtract(acc, multiply(value, factor));
    }
};

[[nodiscard]] double add_norm1_lanes(
    __m128 difference, __m128 threshold, double sum) noexcept {
    difference = _mm_and_ps(difference, _mm_cmpgt_ps(difference, threshold));
    sum += static_cast<double>(_mm_cvtss_f32(difference));
    difference = _mm_shuffle_ps(difference, difference, _MM_SHUFFLE(0, 3, 2, 1));
    sum += static_cast<double>(_mm_cvtss_f32(difference));
    difference = _mm_shuffle_ps(difference, difference, _MM_SHUFFLE(0, 3, 2, 1));
    sum += static_cast<double>(_mm_cvtss_f32(difference));
    difference = _mm_shuffle_ps(difference, difference, _MM_SHUFFLE(0, 3, 2, 1));
    sum += static_cast<double>(_mm_cvtss_f32(difference));
    return sum;
}

} // namespace

void inverse_columns_sse2_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept {
    inverse_columns_x86<Sse2Operations>(
        plan, input, input_row_stride, output, output_row_stride, column_count);
}

void absolute_difference_sse2_f32(
    const float *source, const float *reconstruction, float *differences) noexcept {
    const __m128 difference = _mm_sub_ps(
        _mm_loadu_ps(source), _mm_loadu_ps(reconstruction));
    _mm_storeu_ps(
        differences, _mm_andnot_ps(_mm_set1_ps(-0.0F), difference));
}

// Bit-exact with the scalar raster-order accumulator (masked lanes add an
// exact +0.0; survivors keep their order in one double chain).
double absolute_difference_norm1_sse2_f32(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const __m128 sign = _mm_set1_ps(-0.0F);
    const __m128 threshold_values = _mm_set1_ps(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += Sse2Operations::lanes) {
        const __m128 difference = _mm_andnot_ps(
            sign, _mm_sub_ps(_mm_loadu_ps(source + x),
                             _mm_loadu_ps(reconstruction + x)));
        sum = add_norm1_lanes(difference, threshold_values, sum);
    }
    return sum;
}

void vertical_reconstruction_sse2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept {
    __m128 reconstructed = _mm_setzero_ps();
    for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
        const __m128 values = _mm_loadu_ps(
            native + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x);
        reconstructed = _mm_add_ps(
            reconstructed,
            _mm_mul_ps(
                values, _mm_set1_ps(plan.forward_weights[
                    begin + static_cast<std::uint32_t>(tap)])));
    }
    const __m128 difference = _mm_sub_ps(_mm_loadu_ps(source + x), reconstructed);
    _mm_storeu_ps(
        differences, _mm_andnot_ps(_mm_set1_ps(-0.0F), difference));
}

double vertical_reconstruction_norm1_sse2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const __m128 sign = _mm_set1_ps(-0.0F);
    const __m128 threshold_values = _mm_set1_ps(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += Sse2Operations::lanes) {
        __m128 reconstructed = _mm_setzero_ps();
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            const __m128 values = _mm_loadu_ps(
                native + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x);
            reconstructed = _mm_add_ps(
                reconstructed,
                _mm_mul_ps(values, _mm_set1_ps(plan.forward_weights[
                    begin + static_cast<std::uint32_t>(tap)])));
        }
        const __m128 difference = _mm_andnot_ps(
            sign, _mm_sub_ps(_mm_loadu_ps(source + x), reconstructed));
        sum = add_norm1_lanes(difference, threshold_values, sum);
    }
    return sum;
}

} // namespace getnative::detail

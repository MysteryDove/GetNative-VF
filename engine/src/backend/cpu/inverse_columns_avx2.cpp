#include "inverse_columns_x86_impl.hpp"

#include <immintrin.h>

namespace getnative::detail {
namespace {

struct Avx2Operations {
    using Vector = __m256;
    static constexpr std::int32_t lanes = 8;

    [[nodiscard]] static Vector zero() noexcept { return _mm256_setzero_ps(); }
    [[nodiscard]] static Vector load(const float *value) noexcept {
        return _mm256_loadu_ps(value);
    }
    static void store(float *destination, Vector value) noexcept {
        _mm256_storeu_ps(destination, value);
    }
    [[nodiscard]] static Vector multiply(Vector value, float factor) noexcept {
        return _mm256_mul_ps(value, _mm256_set1_ps(factor));
    }
    [[nodiscard]] static Vector add(Vector left, Vector right) noexcept {
        return _mm256_add_ps(left, right);
    }
    [[nodiscard]] static Vector subtract(Vector left, Vector right) noexcept {
        return _mm256_sub_ps(left, right);
    }
    [[nodiscard]] static Vector multiply_add(
        Vector acc, Vector value, float factor) noexcept {
        return _mm256_fmadd_ps(value, _mm256_set1_ps(factor), acc);
    }
    [[nodiscard]] static Vector multiply_sub(
        Vector acc, Vector value, float factor) noexcept {
        return _mm256_fnmadd_ps(value, _mm256_set1_ps(factor), acc);
    }
};

} // namespace

void inverse_columns_avx2_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept {
    inverse_columns_x86<Avx2Operations>(
        plan, input, input_row_stride, output, output_row_stride, column_count);
}

void absolute_difference_avx2_f32(
    const float *source, const float *reconstruction, float *differences) noexcept {
    const __m256 difference = _mm256_sub_ps(
        _mm256_loadu_ps(source), _mm256_loadu_ps(reconstruction));
    _mm256_storeu_ps(
        differences, _mm256_andnot_ps(_mm256_set1_ps(-0.0F), difference));
}

void vertical_reconstruction_avx2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept {
    __m256 reconstructed = _mm256_setzero_ps();
    for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
        const __m256 values = _mm256_loadu_ps(
            native + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x);
        reconstructed = _mm256_fmadd_ps(
            values,
            _mm256_set1_ps(
                plan.forward_weights[begin + static_cast<std::uint32_t>(tap)]),
            reconstructed);
    }
    const __m256 difference = _mm256_sub_ps(_mm256_loadu_ps(source + x), reconstructed);
    _mm256_storeu_ps(
        differences, _mm256_andnot_ps(_mm256_set1_ps(-0.0F), difference));
}

} // namespace getnative::detail

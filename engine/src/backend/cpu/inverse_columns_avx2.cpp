#include "inverse_columns_x86_impl.hpp"

#include <immintrin.h>

namespace getnative::detail {
namespace {

struct Avx2Operations {
    using Vector = __m256;
    static constexpr std::int32_t lanes = 8;
    static constexpr std::int32_t tile_vectors = 3;

    [[nodiscard]] static Vector zero() noexcept { return _mm256_setzero_ps(); }
    [[nodiscard]] static Vector broadcast(float value) noexcept {
        return _mm256_set1_ps(value);
    }
    [[nodiscard]] static Vector load(const float *value) noexcept {
        return _mm256_loadu_ps(value);
    }
    static void store(float *destination, Vector value) noexcept {
        _mm256_storeu_ps(destination, value);
    }
    [[nodiscard]] static Vector multiply(Vector value, Vector factor) noexcept {
        return _mm256_mul_ps(value, factor);
    }
    [[nodiscard]] static Vector add(Vector left, Vector right) noexcept {
        return _mm256_add_ps(left, right);
    }
    [[nodiscard]] static Vector subtract(Vector left, Vector right) noexcept {
        return _mm256_sub_ps(left, right);
    }
    [[nodiscard]] static Vector multiply_add(
        Vector acc, Vector value, Vector factor) noexcept {
        return _mm256_fmadd_ps(value, factor, acc);
    }
    [[nodiscard]] static Vector multiply_sub(
        Vector acc, Vector value, Vector factor) noexcept {
        return _mm256_fnmadd_ps(value, factor, acc);
    }
};

[[nodiscard]] double add_norm1_lanes(
    __m128 difference, double sum) noexcept {
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

double vertical_reconstruction_norm1_avx2_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const __m256 sign = _mm256_set1_ps(-0.0F);
    const __m256 threshold_values = _mm256_set1_ps(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += Avx2Operations::lanes) {
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
        __m256 difference = _mm256_andnot_ps(
            sign, _mm256_sub_ps(_mm256_loadu_ps(source + x), reconstructed));
        difference = _mm256_and_ps(
            difference, _mm256_cmp_ps(difference, threshold_values, _CMP_GT_OQ));
        sum = add_norm1_lanes(_mm256_castps256_ps128(difference), sum);
        sum = add_norm1_lanes(_mm256_extractf128_ps(difference, 1), sum);
    }
    return sum;
}

} // namespace getnative::detail

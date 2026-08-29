#include "inverse_columns_x86_impl.hpp"

#include <immintrin.h>

namespace getnative::detail {
namespace {

struct Avx512Operations {
    using Vector = __m512;
    static constexpr std::int32_t lanes = 16;
    static constexpr std::int32_t tile_vectors = 1;

    [[nodiscard]] static Vector zero() noexcept { return _mm512_setzero_ps(); }
    [[nodiscard]] static Vector broadcast(float value) noexcept {
        return _mm512_set1_ps(value);
    }
    [[nodiscard]] static Vector load(const float *value) noexcept {
        return _mm512_loadu_ps(value);
    }
    static void store(float *destination, Vector value) noexcept {
        _mm512_storeu_ps(destination, value);
    }
    [[nodiscard]] static Vector multiply(Vector value, Vector factor) noexcept {
        return _mm512_mul_ps(value, factor);
    }
    [[nodiscard]] static Vector add(Vector left, Vector right) noexcept {
        return _mm512_add_ps(left, right);
    }
    [[nodiscard]] static Vector subtract(Vector left, Vector right) noexcept {
        return _mm512_sub_ps(left, right);
    }
    [[nodiscard]] static Vector multiply_add(
        Vector acc, Vector value, Vector factor) noexcept {
        return _mm512_fmadd_ps(value, factor, acc);
    }
    [[nodiscard]] static Vector multiply_sub(
        Vector acc, Vector value, Vector factor) noexcept {
        return _mm512_fnmadd_ps(value, factor, acc);
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

void inverse_columns_avx512_f32(
    const AxisPlan &plan, const float *input, std::ptrdiff_t input_row_stride,
    float *output, std::ptrdiff_t output_row_stride,
    std::int32_t column_count) noexcept {
    inverse_columns_x86<Avx512Operations>(
        plan, input, input_row_stride, output, output_row_stride, column_count);
}

void absolute_difference_avx512_f32(
    const float *source, const float *reconstruction, float *differences) noexcept {
    const __m512 difference = _mm512_sub_ps(
        _mm512_loadu_ps(source), _mm512_loadu_ps(reconstruction));
    const __m512 absolute = _mm512_castsi512_ps(_mm512_andnot_si512(
        _mm512_set1_epi32(static_cast<int>(0x80000000U)),
        _mm512_castps_si512(difference)));
    _mm512_storeu_ps(differences, absolute);
}

// Bit-exact with the scalar raster-order accumulator (masked lanes add an
// exact +0.0; survivors keep their order in one double chain).
double absolute_difference_norm1_avx512_f32(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const __m512i sign = _mm512_set1_epi32(static_cast<int>(0x80000000U));
    const __m512 threshold_values = _mm512_set1_ps(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += Avx512Operations::lanes) {
        const __m512 difference = _mm512_castsi512_ps(_mm512_andnot_si512(
            sign, _mm512_castps_si512(
                _mm512_sub_ps(_mm512_loadu_ps(source + x),
                              _mm512_loadu_ps(reconstruction + x)))));
        const __m512 filtered = _mm512_maskz_mov_ps(
            _mm512_cmp_ps_mask(difference, threshold_values, _CMP_GT_OQ), difference);
        sum = add_norm1_lanes(_mm512_castps512_ps128(filtered), sum);
        sum = add_norm1_lanes(_mm512_extractf32x4_ps(filtered, 1), sum);
        sum = add_norm1_lanes(_mm512_extractf32x4_ps(filtered, 2), sum);
        sum = add_norm1_lanes(_mm512_extractf32x4_ps(filtered, 3), sum);
    }
    return sum;
}

void vertical_reconstruction_avx512_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x, float *differences) noexcept {
    __m512 reconstructed = _mm512_setzero_ps();
    for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
        const __m512 values = _mm512_loadu_ps(
            native + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x);
        reconstructed = _mm512_fmadd_ps(
            values,
            _mm512_set1_ps(
                plan.forward_weights[begin + static_cast<std::uint32_t>(tap)]),
            reconstructed);
    }
    const __m512 difference = _mm512_sub_ps(_mm512_loadu_ps(source + x), reconstructed);
    const __m512 absolute = _mm512_castsi512_ps(_mm512_andnot_si512(
        _mm512_set1_epi32(static_cast<int>(0x80000000U)),
        _mm512_castps_si512(difference)));
    _mm512_storeu_ps(differences, absolute);
}

double vertical_reconstruction_norm1_avx512_f32(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end, float threshold,
    double sum) noexcept {
    const __m512i sign = _mm512_set1_epi32(static_cast<int>(0x80000000U));
    const __m512 threshold_values = _mm512_set1_ps(threshold);
    for (std::int32_t x = x_begin; x < x_end; x += Avx512Operations::lanes) {
        __m512 reconstructed = _mm512_setzero_ps();
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            const __m512 values = _mm512_loadu_ps(
                native + static_cast<std::ptrdiff_t>(left + tap) * native_stride + x);
            reconstructed = _mm512_fmadd_ps(
                values,
                _mm512_set1_ps(
                    plan.forward_weights[begin + static_cast<std::uint32_t>(tap)]),
                reconstructed);
        }
        const __m512 difference = _mm512_castsi512_ps(_mm512_andnot_si512(
            sign, _mm512_castps_si512(
                _mm512_sub_ps(_mm512_loadu_ps(source + x), reconstructed))));
        const __m512 filtered = _mm512_maskz_mov_ps(
            _mm512_cmp_ps_mask(difference, threshold_values, _CMP_GT_OQ), difference);
        sum = add_norm1_lanes(_mm512_castps512_ps128(filtered), sum);
        sum = add_norm1_lanes(_mm512_extractf32x4_ps(filtered, 1), sum);
        sum = add_norm1_lanes(_mm512_extractf32x4_ps(filtered, 2), sum);
        sum = add_norm1_lanes(_mm512_extractf32x4_ps(filtered, 3), sum);
    }
    return sum;
}

} // namespace getnative::detail

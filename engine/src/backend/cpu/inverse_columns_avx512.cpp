#include "inverse_columns_x86_impl.hpp"

#include <immintrin.h>

namespace getnative::detail {
namespace {

struct Avx512Operations {
    using Vector = __m512;
    static constexpr std::int32_t lanes = 16;

    [[nodiscard]] static Vector zero() noexcept { return _mm512_setzero_ps(); }
    [[nodiscard]] static Vector load(const float *value) noexcept {
        return _mm512_loadu_ps(value);
    }
    static void store(float *destination, Vector value) noexcept {
        _mm512_storeu_ps(destination, value);
    }
    [[nodiscard]] static Vector multiply(Vector value, float factor) noexcept {
        return _mm512_mul_ps(value, _mm512_set1_ps(factor));
    }
    [[nodiscard]] static Vector add(Vector left, Vector right) noexcept {
        return _mm512_add_ps(left, right);
    }
    [[nodiscard]] static Vector subtract(Vector left, Vector right) noexcept {
        return _mm512_sub_ps(left, right);
    }
    [[nodiscard]] static Vector multiply_add(
        Vector acc, Vector value, float factor) noexcept {
        return _mm512_fmadd_ps(value, _mm512_set1_ps(factor), acc);
    }
    [[nodiscard]] static Vector multiply_sub(
        Vector acc, Vector value, float factor) noexcept {
        return _mm512_fnmadd_ps(value, _mm512_set1_ps(factor), acc);
    }
};

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

} // namespace getnative::detail

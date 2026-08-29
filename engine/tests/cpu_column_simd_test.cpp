#include "inverse_columns.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <class Exception = std::exception, class Function>
void expect_throws(Function &&function, std::string_view message) {
    try {
        function();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

using NamedFilter = std::pair<std::string, getnative::Filter>;

[[nodiscard]] std::vector<NamedFilter> filters() {
    std::vector<NamedFilter> values{
        {"bilinear", getnative::Filter::bilinear()},
        {"bicubic-catrom", getnative::Filter::bicubic(0.0, 0.5)},
        {"bicubic-mitchell", getnative::Filter::bicubic(1.0 / 3.0, 1.0 / 3.0)},
        {"bicubic-bspline", getnative::Filter::bicubic(1.0, 0.0)},
        {"bicubic-arbitrary", getnative::Filter::bicubic(-0.25, 0.75)},
        {"spline16", getnative::Filter::spline16()},
        {"spline36", getnative::Filter::spline36()},
        {"spline64", getnative::Filter::spline64()},
        {"bicubic-catrom-blur125", getnative::Filter::bicubic(0.0, 0.5, 1.25)},
        {"spline64-blur125", getnative::Filter::spline64(1.25)},
        {"spline64-blur150", getnative::Filter::spline64(1.5)},
    };
    for (std::int32_t taps = 1; taps <= 15; ++taps) {
        values.emplace_back(
            "lanczos" + std::to_string(taps), getnative::Filter::lanczos(taps));
    }
    return values;
}

[[nodiscard]] float source_value(std::int32_t row, std::int32_t column) {
    const int selector = (row * 17 + column * 29) % 19;
    if (selector == 0) return 0.0F;
    if (selector == 1) return -0.0F;
    if (selector == 2) return std::numeric_limits<float>::denorm_min();
    if (selector == 3) return -std::numeric_limits<float>::denorm_min();
    if (selector == 4) return std::numeric_limits<float>::min();
    if (selector == 5) return -std::numeric_limits<float>::min();
    if (selector == 6) return std::numeric_limits<float>::max() / 2048.0F;
    if (selector == 7) return -std::numeric_limits<float>::max() / 2048.0F;
    return static_cast<float>(
        0.37 + 0.21 * std::sin(0.073 * static_cast<double>(row))
        + 0.17 * std::cos(0.091 * static_cast<double>(column))
        + 0.05 * std::sin(0.037 * static_cast<double>(row + column)));
}

// Production CPU path uses FMA. SSE2 emulates FMA with mul+add, so SIMD tiers
// may differ from scalar. Intermediate buffers use a looser relative bound;
// metrics use the shared production tolerance max(1e-7, 5e-4*|ref|).
[[nodiscard]] bool close_float(float actual, float expected) noexcept {
    if (std::bit_cast<std::uint32_t>(actual)
        == std::bit_cast<std::uint32_t>(expected)) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) {
        return false;
    }
    const float scale = std::max(std::abs(actual), std::abs(expected));
    return std::abs(actual - expected) <= std::max(1e-5F, 1e-3F * scale);
}

[[nodiscard]] bool close_metric(double actual, double expected) noexcept {
    if (std::bit_cast<std::uint64_t>(actual)
        == std::bit_cast<std::uint64_t>(expected)) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) {
        return false;
    }
    const double scale = std::max(std::abs(actual), std::abs(expected));
    return std::abs(actual - expected) <= std::max(1e-7, 5e-4 * scale);
}

[[nodiscard]] bool close_double(double actual, double expected) noexcept {
    return close_metric(actual, expected);
}

void expect_same_float(float actual, float expected, const std::string &message) {
    if (!close_float(actual, expected)) {
        throw std::runtime_error(message);
    }
}

void expect_same_double(double actual, double expected, const std::string &message) {
    if (!close_double(actual, expected)) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] std::int32_t lane_count(getnative::CpuIsa isa) noexcept {
    switch (isa) {
    case getnative::CpuIsa::scalar: return 1;
    case getnative::CpuIsa::sse2: return 4;
    case getnative::CpuIsa::avx2: return 8;
    case getnative::CpuIsa::avx512: return 16;
    }
    return 1;
}

[[nodiscard]] std::vector<float> make_source(
    std::int32_t width, std::int32_t height, std::int32_t stride);

[[nodiscard]] getnative::CpuFeatureSnapshot full_avx512_snapshot() {
    getnative::CpuFeatureSnapshot snapshot{};
    snapshot.x86 = true;
    snapshot.maximum_basic_leaf = 7;
    snapshot.leaf1.eax = 0x000A0671U;
    snapshot.leaf1.ecx = (std::uint32_t{1} << 27U) | (std::uint32_t{1} << 28U);
    snapshot.leaf1.edx = std::uint32_t{1} << 26U;
    snapshot.leaf7_subleaf0.ebx =
        (std::uint32_t{1} << 5U) | (std::uint32_t{1} << 16U);
    snapshot.xcr0 = 0xE6U;
    snapshot.xcr0_valid = true;
    snapshot.vendor = {'G', 'e', 'n', 'u', 'i', 'n', 'e', 'I', 'n', 't', 'e', 'l', '\0'};
    snapshot.family = 6;
    snapshot.model = 167;
    snapshot.stepping = 1;
    snapshot.logical_processor_count = 16;
    return snapshot;
}

void test_feature_evaluator() {
    const getnative::CpuIsaSet all_compiled{true, true, true, true};
    auto snapshot = full_avx512_snapshot();

    auto info = getnative::evaluate_cpu_dispatch(
        snapshot, all_compiled, getnative::CpuIsaRequest::automatic, false);
    expect(info.available.sse2 && info.available.avx2 && info.available.avx512,
           "full snapshot exposes every compiled tier");
    expect(info.selected == getnative::CpuIsa::avx2,
           "AVX-512 remains force-only without benchmark approval");
    expect(info.selection_reason == "avx512 not benchmark-approved",
           "unapproved AVX-512 selection reason is stable");

    info = getnative::evaluate_cpu_dispatch(
        snapshot, all_compiled, getnative::CpuIsaRequest::automatic, true);
    expect(info.selected == getnative::CpuIsa::avx512,
           "approved AVX-512 may win automatic dispatch");

    snapshot.xcr0 = 0x6U;
    info = getnative::evaluate_cpu_dispatch(snapshot, all_compiled);
    expect(info.available.avx2 && !info.available.avx512,
           "AVX512F without complete ZMM state is unavailable");

    snapshot = full_avx512_snapshot();
    snapshot.xcr0_valid = false;
    info = getnative::evaluate_cpu_dispatch(snapshot, all_compiled);
    expect(!info.available.avx2 && !info.available.avx512,
           "XCR0 bits are ignored unless OSXSAVE authorized the read");

    snapshot = full_avx512_snapshot();
    snapshot.leaf1.ecx &= ~(std::uint32_t{1} << 27U);
    info = getnative::evaluate_cpu_dispatch(snapshot, all_compiled);
    expect(!info.available.avx2 && !info.available.avx512,
           "missing OSXSAVE rejects AVX tiers");

    snapshot = full_avx512_snapshot();
    snapshot.maximum_basic_leaf = 1;
    info = getnative::evaluate_cpu_dispatch(snapshot, all_compiled);
    expect(info.available.sse2 && !info.available.avx2 && !info.available.avx512,
           "missing leaf 7 retains only SSE2");

    snapshot = full_avx512_snapshot();
    info = getnative::evaluate_cpu_dispatch(
        snapshot, {true, true, false, false}, getnative::CpuIsaRequest::avx2);
    expect(info.forced && !info.request_available
               && info.selected == getnative::CpuIsa::avx2,
           "compiled mask rejects a forced omitted tier");

    snapshot = {};
    info = getnative::evaluate_cpu_dispatch(snapshot, all_compiled);
    expect(info.selected == getnative::CpuIsa::scalar
               && !info.available.sse2 && !info.available.avx2
               && !info.available.avx512,
           "non-x86 snapshot safely selects scalar");

    expect(getnative::parse_cpu_isa_request("auto")
               == getnative::CpuIsaRequest::automatic,
           "auto request parses");
    expect(getnative::parse_cpu_isa_request("avx-512")
               == getnative::CpuIsaRequest::avx512,
           "AVX-512 alias parses");
    expect(!getnative::parse_cpu_isa_request("native"),
           "unknown request is rejected");
}

void test_materialized_inverse_columns(
    getnative::detail::ColumnDispatchPolicy policy,
    getnative::CpuIsa selected_isa) {
    constexpr std::int32_t source_size = 79;
    constexpr std::int32_t destination_size = 43;
    constexpr float padding = -913.25F;
    const std::vector<getnative::BorderMode> borders{
        getnative::BorderMode::zero,
        getnative::BorderMode::repeat,
        getnative::BorderMode::mirror,
    };
    const std::vector<std::int32_t> column_counts{
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63,
    };
    const std::int32_t lanes = lane_count(selected_isa);

    for (const auto &[name, filter] : filters()) {
        for (const getnative::BorderMode border : borders) {
            const auto plan = getnative::build_axis_plan({
                source_size, destination_size, 43.25, -0.1875, filter, border,
            });
            for (const std::int32_t columns : column_counts) {
                for (std::int32_t base_offset = 0; base_offset < lanes; ++base_offset) {
                    const std::ptrdiff_t input_stride = columns + 3;
                    const std::ptrdiff_t output_stride = columns + 5;
                    const std::size_t input_count = static_cast<std::size_t>(
                        source_size * input_stride + base_offset + 7);
                    const std::size_t output_count = static_cast<std::size_t>(
                        destination_size * output_stride + base_offset + 7);
                    std::vector<float> input(input_count, padding);
                    for (std::int32_t row = 0; row < source_size; ++row) {
                        for (std::int32_t column = 0; column < columns; ++column) {
                            input[static_cast<std::size_t>(base_offset)
                                  + static_cast<std::size_t>(row * input_stride + column)] =
                                source_value(row, column);
                        }
                    }
                    const std::vector<float> original_input = input;
                    std::vector<float> scalar(output_count, padding);
                    std::vector<float> actual(output_count, padding);
                    getnative::detail::inverse_columns_f32(
                        plan, input.data() + base_offset, input_stride,
                        scalar.data() + base_offset, output_stride, columns,
                        getnative::detail::ColumnDispatchPolicy::scalar_only);
                    getnative::detail::inverse_columns_f32(
                        plan, input.data() + base_offset, input_stride,
                        actual.data() + base_offset, output_stride, columns, policy);
                    expect(input == original_input, "inverse kernel modified its input");
                    for (std::size_t index = 0; index < actual.size(); ++index) {
                        expect_same_float(
                            actual[index], scalar[index],
                            name + " inverse differs from scalar at storage index "
                                + std::to_string(index));
                    }
                }
            }
        }
    }
}

void test_materialized_inverse_rows(getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t source_size = 79;
    constexpr std::int32_t destination_size = 43;
    constexpr float padding = -913.25F;
    const std::vector<getnative::BorderMode> borders{
        getnative::BorderMode::zero,
        getnative::BorderMode::repeat,
        getnative::BorderMode::mirror,
    };
    const std::vector<std::int32_t> row_counts{
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
    };

    for (const auto &[name, filter] : filters()) {
        for (const getnative::BorderMode border : borders) {
            const auto plan = getnative::build_axis_plan({
                source_size, destination_size, 43.25, -0.1875, filter, border,
            });
            for (const std::int32_t rows : row_counts) {
                const std::ptrdiff_t input_stride = source_size + 3;
                const std::ptrdiff_t output_stride = destination_size + 5;
                const std::size_t input_count = static_cast<std::size_t>(
                    rows > 0 ? (rows - 1) * input_stride + source_size + 7 : 8);
                const std::size_t output_count = static_cast<std::size_t>(
                    rows > 0 ? (rows - 1) * output_stride + destination_size + 7 : 8);
                std::vector<float> input(input_count, padding);
                for (std::int32_t row = 0; row < rows; ++row) {
                    for (std::int32_t x = 0; x < source_size; ++x) {
                        input[static_cast<std::size_t>(row) * input_stride
                              + static_cast<std::size_t>(x)] = source_value(row, x);
                    }
                }
                const std::vector<float> original_input = input;
                std::vector<float> scalar(output_count, padding);
                std::vector<float> actual(output_count, padding);
                for (std::int32_t row = 0; row < rows; ++row) {
                    getnative::inverse_axis_f32(
                        plan, input.data() + static_cast<std::ptrdiff_t>(row) * input_stride,
                        1,
                        scalar.data() + static_cast<std::ptrdiff_t>(row) * output_stride,
                        1);
                }
                getnative::detail::inverse_rows_f32(
                    plan, input.data(), input_stride,
                    actual.data(), output_stride, rows, policy);
                expect(input == original_input, "row inverse kernel modified its input");
                for (std::size_t index = 0; index < actual.size(); ++index) {
                    expect_same_float(
                        actual[index], scalar[index],
                        name + " row inverse differs from scalar at storage index "
                            + std::to_string(index));
                }
            }
        }
    }
}

void test_materialized_forward_rows(getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t source_size = 79;
    constexpr std::int32_t destination_size = 43;
    constexpr float padding = -913.25F;
    const std::vector<getnative::BorderMode> borders{
        getnative::BorderMode::zero,
        getnative::BorderMode::repeat,
        getnative::BorderMode::mirror,
    };
    const std::vector<std::int32_t> row_counts{
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
    };

    for (const auto &[name, filter] : filters()) {
        for (const getnative::BorderMode border : borders) {
            const auto plan = getnative::build_axis_plan({
                source_size, destination_size, 43.25, -0.1875, filter, border,
            });
            for (const std::int32_t rows : row_counts) {
                const std::ptrdiff_t input_stride = destination_size + 3;
                const std::ptrdiff_t output_stride = source_size + 5;
                const std::size_t input_count = static_cast<std::size_t>(
                    rows > 0 ? (rows - 1) * input_stride + destination_size + 7 : 8);
                const std::size_t output_count = static_cast<std::size_t>(
                    rows > 0 ? (rows - 1) * output_stride + source_size + 7 : 8);
                std::vector<float> input(input_count, padding);
                for (std::int32_t row = 0; row < rows; ++row) {
                    for (std::int32_t x = 0; x < destination_size; ++x) {
                        input[static_cast<std::size_t>(row) * input_stride
                              + static_cast<std::size_t>(x)] = source_value(row, x);
                    }
                }
                const std::vector<float> original_input = input;
                std::vector<float> scalar(output_count, padding);
                std::vector<float> actual(output_count, padding);
                for (std::int32_t row = 0; row < rows; ++row) {
                    getnative::forward_axis_f32(
                        plan, input.data() + static_cast<std::ptrdiff_t>(row) * input_stride,
                        1,
                        scalar.data() + static_cast<std::ptrdiff_t>(row) * output_stride,
                        1);
                }
                getnative::detail::forward_rows_f32(
                    plan, input.data(), input_stride,
                    actual.data(), output_stride, rows, policy);
                expect(input == original_input, "row forward kernel modified its input");
                for (std::size_t index = 0; index < actual.size(); ++index) {
                    expect_same_float(
                        actual[index], scalar[index],
                        name + " row forward differs from scalar at storage index "
                            + std::to_string(index));
                }
            }
        }
    }
}

void test_materialized_forward_columns(
    getnative::detail::ColumnDispatchPolicy policy,
    getnative::CpuIsa selected_isa) {
    constexpr std::int32_t source_size = 79;
    constexpr std::int32_t destination_size = 43;
    constexpr float padding = -913.25F;
    const std::vector<getnative::BorderMode> borders{
        getnative::BorderMode::zero,
        getnative::BorderMode::repeat,
        getnative::BorderMode::mirror,
    };
    const std::vector<std::int32_t> column_counts{
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63,
    };
    const std::int32_t lanes = lane_count(selected_isa);

    for (const auto &[name, filter] : filters()) {
        for (const getnative::BorderMode border : borders) {
            const auto plan = getnative::build_axis_plan({
                source_size, destination_size, 43.25, -0.1875, filter, border,
            });
            for (const std::int32_t columns : column_counts) {
                for (std::int32_t base_offset = 0; base_offset < lanes; ++base_offset) {
                    const std::ptrdiff_t input_stride = columns + 3;
                    const std::ptrdiff_t output_stride = columns + 5;
                    const std::size_t input_count = static_cast<std::size_t>(
                        destination_size * input_stride + base_offset + 7);
                    const std::size_t output_count = static_cast<std::size_t>(
                        source_size * output_stride + base_offset + 7);
                    std::vector<float> input(input_count, padding);
                    for (std::int32_t row = 0; row < destination_size; ++row) {
                        for (std::int32_t column = 0; column < columns; ++column) {
                            input[static_cast<std::size_t>(base_offset)
                                  + static_cast<std::size_t>(row * input_stride + column)] =
                                source_value(row, column);
                        }
                    }
                    const std::vector<float> original_input = input;
                    std::vector<float> scalar(output_count, padding);
                    std::vector<float> actual(output_count, padding);
                    getnative::detail::forward_columns_f32(
                        plan, input.data() + base_offset, input_stride,
                        scalar.data() + base_offset, output_stride, columns,
                        getnative::detail::ColumnDispatchPolicy::scalar_only);
                    getnative::detail::forward_columns_f32(
                        plan, input.data() + base_offset, input_stride,
                        actual.data() + base_offset, output_stride, columns, policy);
                    expect(input == original_input, "forward column kernel modified its input");
                    for (std::size_t index = 0; index < actual.size(); ++index) {
                        expect_same_float(
                            actual[index], scalar[index],
                            name + " column forward differs from scalar at storage index "
                                + std::to_string(index));
                    }
                }
            }
        }
    }
}

void test_cropped_inverse_column_ranges(
    getnative::detail::ColumnDispatchPolicy policy) {    constexpr std::int32_t source_size = 53;
    constexpr std::int32_t destination_size = 31;
    constexpr std::int32_t columns = 41;
    constexpr std::ptrdiff_t input_stride = columns + 5;
    constexpr std::ptrdiff_t output_stride = columns + 7;
    constexpr float padding = -2047.25F;
    const auto input = make_source(columns, source_size,
                                   static_cast<std::int32_t>(input_stride));

    for (const auto &[name, filter] : filters()) {
        const auto plan = getnative::build_axis_plan({
            source_size, destination_size, 31.25, -0.125, filter,
            getnative::BorderMode::mirror,
        });
        std::vector<float> full(
            static_cast<std::size_t>(destination_size * output_stride), padding);
        getnative::detail::inverse_columns_f32(
            plan, input.data(), input_stride, full.data(), output_stride,
            columns, getnative::detail::ColumnDispatchPolicy::scalar_only);

        for (const auto &[offset, count] : std::vector<std::pair<std::int32_t, std::int32_t>>{
                 {0, 0}, {0, 1}, {1, 3}, {3, 16}, {7, 17}, {16, 24}, {40, 1}}) {
            std::vector<float> cropped(full.size(), padding);
            getnative::detail::inverse_columns_f32(
                plan, input.data(), input_stride, cropped.data(), output_stride,
                offset, count, policy);
            for (std::int32_t row = 0; row < destination_size; ++row) {
                for (std::int32_t column = 0; column < output_stride; ++column) {
                    const std::size_t index = static_cast<std::size_t>(
                        row * output_stride + column);
                    if (column >= offset && column < offset + count) {
                        expect_same_float(
                            cropped[index], full[index],
                            name + " cropped inverse range differs at column "
                                + std::to_string(column));
                    } else {
                        expect(cropped[index] == padding,
                               name + " cropped inverse wrote outside its range");
                    }
                }
            }
        }
    }

    const auto plan = getnative::build_axis_plan({
        source_size, destination_size, 31.25, -0.125,
        getnative::Filter::bilinear(), getnative::BorderMode::mirror,
    });
    std::vector<float> output(
        static_cast<std::size_t>(destination_size * output_stride), padding);
    expect_throws<std::invalid_argument>([&] {
        getnative::detail::inverse_columns_f32(
            plan, input.data(), input_stride, output.data(), output_stride,
            -1, 1, policy);
    }, "negative inverse column offset is rejected");
}

void test_avx2_b5_row_major_boundaries(
    getnative::detail::ColumnDispatchPolicy policy,
    getnative::CpuIsa selected_isa) {
    if (selected_isa != getnative::CpuIsa::avx2) return;

    constexpr std::int32_t source_size = 79;
    constexpr std::int32_t destination_size = 43;
    constexpr std::int32_t base_offset = 3;
    constexpr float padding = -1173.5F;
    const std::vector<NamedFilter> b5_filters{
        {"spline36", getnative::Filter::spline36()},
        {"lanczos3", getnative::Filter::lanczos(3)},
    };
    const std::vector<std::int32_t> column_counts{
        0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63,
        1919, 1920, 1921,
    };

    for (const auto &[name, filter] : b5_filters) {
        const auto plan = getnative::build_axis_plan({
            source_size, destination_size, 43.25, -0.1875, filter,
            getnative::BorderMode::mirror,
        });
        expect(plan.support == 3 && plan.half_bandwidth == 5
                   && plan.forward_width == 6,
               name + " did not produce the AVX2 B5 specialization shape");

        for (const std::int32_t columns : column_counts) {
            const std::ptrdiff_t input_stride = columns + 7;
            const std::ptrdiff_t output_stride = columns + 11;
            const std::size_t input_count = static_cast<std::size_t>(
                source_size * input_stride + base_offset + 7);
            const std::size_t output_count = static_cast<std::size_t>(
                destination_size * output_stride + base_offset + 7);
            std::vector<float> input(input_count, padding);
            for (std::int32_t row = 0; row < source_size; ++row) {
                for (std::int32_t column = 0; column < columns; ++column) {
                    input[static_cast<std::size_t>(base_offset)
                          + static_cast<std::size_t>(row * input_stride + column)] =
                        source_value(row, column);
                }
            }
            const std::vector<float> original_input = input;
            std::vector<float> row_major(output_count, padding);
            std::vector<float> column_major(output_count, padding);

            getnative::detail::inverse_columns_f32(
                plan, input.data() + base_offset, input_stride,
                row_major.data() + base_offset, output_stride, columns, policy);
            std::int32_t reference_column = 0;
            for (; reference_column + 16 <= columns; reference_column += 16) {
                getnative::detail::inverse_columns_f32(
                    plan, input.data() + base_offset + reference_column,
                    input_stride,
                    column_major.data() + base_offset + reference_column,
                    output_stride, 16, policy);
            }
            if (reference_column + 8 <= columns) {
                getnative::detail::inverse_columns_f32(
                    plan, input.data() + base_offset + reference_column,
                    input_stride,
                    column_major.data() + base_offset + reference_column,
                    output_stride, 8, policy);
                reference_column += 8;
            }
            if (reference_column < columns) {
                getnative::detail::inverse_columns_f32(
                    plan, input.data() + base_offset + reference_column,
                    input_stride,
                    column_major.data() + base_offset + reference_column,
                    output_stride, columns - reference_column, policy);
            }

            expect(input == original_input,
                   name + " AVX2 B5 kernel modified its input");
            for (std::size_t index = 0; index < row_major.size(); ++index) {
                expect(
                    std::bit_cast<std::uint32_t>(row_major[index])
                        == std::bit_cast<std::uint32_t>(column_major[index]),
                    name + " row-major AVX2 differs bitwise at columns="
                        + std::to_string(columns) + ", storage index="
                        + std::to_string(index));
            }
        }
    }
}

[[nodiscard]] std::vector<float> make_source(
    std::int32_t width, std::int32_t height, std::int32_t stride) {
    std::vector<float> pixels(static_cast<std::size_t>(height * stride), -77.0F);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y * stride + x)] = source_value(y, x);
        }
    }
    return pixels;
}

[[nodiscard]] double full_frame_scalar_oracle(
    getnative::ConstImageView source, const getnative::AxisPlan &horizontal,
    const getnative::AxisPlan &vertical, getnative::AnalysisAxes axes,
    const getnative::MetricSpec &metric) {
    const std::size_t source_elements = static_cast<std::size_t>(source.width)
        * static_cast<std::size_t>(source.height);
    std::vector<float> reconstruction(source_elements, 0.0F);

    if (axes == getnative::AnalysisAxes::horizontal) {
        std::vector<float> native(
            static_cast<std::size_t>(horizontal.destination_size));
        for (std::int32_t y = 0; y < source.height; ++y) {
            getnative::inverse_axis_f32(
                horizontal,
                source.data + static_cast<std::ptrdiff_t>(y) * source.stride, 1,
                native.data(), 1);
            getnative::forward_axis_f32(
                horizontal, native.data(), 1,
                reconstruction.data() + static_cast<std::ptrdiff_t>(y) * source.width, 1);
        }
    } else if (axes == getnative::AnalysisAxes::vertical) {
        std::vector<float> native(
            static_cast<std::size_t>(vertical.destination_size));
        for (std::int32_t x = 0; x < source.width; ++x) {
            getnative::inverse_axis_f32(
                vertical, source.data + x, source.stride, native.data(), 1);
            getnative::forward_axis_f32(
                vertical, native.data(), 1, reconstruction.data() + x, source.width);
        }
    } else {
        const std::ptrdiff_t inverse_stride = horizontal.destination_size;
        std::vector<float> inverse_horizontal(
            static_cast<std::size_t>(inverse_stride)
                * static_cast<std::size_t>(source.height));
        for (std::int32_t y = 0; y < source.height; ++y) {
            getnative::inverse_axis_f32(
                horizontal,
                source.data + static_cast<std::ptrdiff_t>(y) * source.stride, 1,
                inverse_horizontal.data()
                    + static_cast<std::ptrdiff_t>(y) * inverse_stride, 1);
        }
        const std::ptrdiff_t native_stride = horizontal.destination_size;
        std::vector<float> native(
            static_cast<std::size_t>(native_stride)
                * static_cast<std::size_t>(vertical.destination_size));
        getnative::detail::inverse_columns_f32(
            vertical, inverse_horizontal.data(), inverse_stride,
            native.data(), native_stride, horizontal.destination_size,
            getnative::detail::ColumnDispatchPolicy::scalar_only);
        std::vector<float> forward_vertical(
            static_cast<std::size_t>(native_stride)
                * static_cast<std::size_t>(source.height));
        for (std::int32_t x = 0; x < horizontal.destination_size; ++x) {
            getnative::forward_axis_f32(
                vertical, native.data() + x, native_stride,
                forward_vertical.data() + x, native_stride);
        }
        for (std::int32_t y = 0; y < source.height; ++y) {
            getnative::forward_axis_f32(
                horizontal,
                forward_vertical.data()
                    + static_cast<std::ptrdiff_t>(y) * native_stride, 1,
                reconstruction.data()
                    + static_cast<std::ptrdiff_t>(y) * source.width, 1);
        }
    }

    return getnative::thresholded_p_norm(
        source, {reconstruction.data(), source.width, source.height, source.width},
        metric);
}

void test_crop_aware_analysis_matches_full_frame_oracle(
    getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t width = 35;
    constexpr std::int32_t height = 29;
    constexpr std::int32_t stride = width + 4;
    const auto pixels = make_source(width, height, stride);
    const getnative::ConstImageView source{pixels.data(), width, height, stride};
    const std::vector<getnative::MetricSpec> metrics{
        {0, 0, 0, 0, 0.015F, 1U},
        {1, 2, 3, 2, 0.015F, 1U},
        {14, 13, 11, 10, 0.015F, 1U},
    };

    for (const auto &[name, filter] : filters()) {
        const auto horizontal = getnative::build_axis_plan({
            width, 21, 21.25, -0.125, filter, getnative::BorderMode::mirror,
        });
        const auto vertical = getnative::build_axis_plan({
            height, 17, 17.25, 0.125, filter, getnative::BorderMode::mirror,
        });
        for (const getnative::MetricSpec &metric : metrics) {
            for (const getnative::AnalysisAxes axes : {
                     getnative::AnalysisAxes::horizontal,
                     getnative::AnalysisAxes::vertical,
                     getnative::AnalysisAxes::both}) {
                getnative::CpuWorkspace workspace;
                const double actual = axes == getnative::AnalysisAxes::both
                    ? getnative::detail::analyze_candidate_with_column_policy_f32(
                          source, horizontal, vertical, metric, workspace, policy)
                    : getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                          source,
                          axes == getnative::AnalysisAxes::horizontal
                              ? horizontal : vertical,
                          axes, metric, workspace, policy);
                const double expected = full_frame_scalar_oracle(
                    source, horizontal, vertical, axes, metric);
                expect_same_double(
                    actual, expected,
                    name + " crop-aware analysis differs from full-frame oracle");
                if (axes == getnative::AnalysisAxes::horizontal) {
                    expect(workspace.intermediate.size()
                               == static_cast<std::size_t>(horizontal.destination_size) * 4U,
                           name + " horizontal analysis retains a 4-row inverse band");
                }
            }
        }
    }
}

void expect_same_workspace(
    const getnative::CpuWorkspace &actual,
    const getnative::CpuWorkspace &scalar,
    std::string_view case_name) {
    // Production correctness is judged on metrics, not bit-identical intermediate
    // buffers. Require matching sizes and finite values so SIMD paths stay sane.
    const auto require_finite = [&](const std::vector<float> &values,
                                    std::string_view buffer_name) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (!std::isfinite(values[index])) {
                throw std::runtime_error(
                    std::string{case_name} + " " + std::string{buffer_name}
                    + " is non-finite at " + std::to_string(index));
            }
        }
    };
    expect(actual.intermediate.size() == scalar.intermediate.size(),
           "workspace intermediate sizes differ");
    expect(actual.native.size() == scalar.native.size(),
           "workspace native sizes differ");
    expect(actual.reconstruction_row.size() == scalar.reconstruction_row.size(),
           "workspace reconstruction-row sizes differ");
    require_finite(actual.intermediate, "intermediate");
    require_finite(actual.native, "native");
    require_finite(actual.reconstruction_row, "reconstruction-row");
    require_finite(scalar.intermediate, "scalar-intermediate");
    require_finite(scalar.native, "scalar-native");
    require_finite(scalar.reconstruction_row, "scalar-reconstruction-row");
}

void test_axis_and_two_axis_candidates(
    getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t width = 63;
    constexpr std::int32_t height = 53;
    constexpr std::int32_t stride = width + 5;
    const auto pixels = make_source(width, height, stride);
    const getnative::ConstImageView source{pixels.data(), width, height, stride};
    const std::vector<getnative::MetricSpec> metrics{
        {0, 0, 0, 0, 0.015F, 1U},
        {1, 2, 2, 3, 0.015F, 2U},
        {1, 2, 2, 3, 0.015F, 3U},
        {1, 2, 2, 3, 0.015F, 4U},
        {1, 2, 2, 3, 0.015F, 5U},
    };
    const auto horizontal = getnative::build_axis_plan({
        width, 47, 47.25, -0.125, getnative::Filter::lanczos(15),
        getnative::BorderMode::mirror,
    });
    const auto vertical = getnative::build_axis_plan({
        height, 37, 37.25, 0.1875, getnative::Filter::bicubic(-0.25, 0.75),
        getnative::BorderMode::repeat,
    });

    for (const getnative::MetricSpec &metric : metrics) {
        for (const auto &[name, plan, axes] : std::vector<std::tuple<
                 std::string_view, const getnative::AxisPlan *, getnative::AnalysisAxes>>{
                 {"horizontal", &horizontal, getnative::AnalysisAxes::horizontal},
                 {"vertical", &vertical, getnative::AnalysisAxes::vertical}}) {
            getnative::CpuWorkspace scalar_workspace;
            getnative::CpuWorkspace actual_workspace;
            const double scalar =
                getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                    source, *plan, axes, metric, scalar_workspace,
                    getnative::detail::ColumnDispatchPolicy::scalar_only);
            const double actual =
                getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                    source, *plan, axes, metric, actual_workspace, policy);
            expect_same_double(actual, scalar, std::string{name} + " metric differs");
            expect_same_workspace(actual_workspace, scalar_workspace, name);
        }

        for (const bool horizontal_first : {false, true}) {
            const auto horizontal_ordered = getnative::build_axis_plan({
                width, 47, horizontal_first ? 61.5 : 47.25, -0.125,
                getnative::Filter::lanczos(4), getnative::BorderMode::mirror,
            });
            const auto vertical_ordered = getnative::build_axis_plan({
                height, 37, horizontal_first ? 37.25 : 51.5, 0.1875,
                getnative::Filter::spline36(), getnative::BorderMode::repeat,
            });
            const getnative::ForwardOrder expected_order = horizontal_first
                ? getnative::ForwardOrder::horizontal_first
                : getnative::ForwardOrder::vertical_first;
            expect(getnative::select_forward_order(horizontal_ordered, vertical_ordered)
                       == expected_order,
                   "test fixture exercises requested forward order");
            getnative::CpuWorkspace scalar_workspace;
            getnative::CpuWorkspace actual_workspace;
            const double scalar =
                getnative::detail::analyze_candidate_with_column_policy_f32(
                    source, horizontal_ordered, vertical_ordered, metric,
                    scalar_workspace,
                    getnative::detail::ColumnDispatchPolicy::scalar_only);
            const double actual =
                getnative::detail::analyze_candidate_with_column_policy_f32(
                    source, horizontal_ordered, vertical_ordered, metric,
                    actual_workspace, policy);
            expect_same_double(actual, scalar, "two-axis metric differs");
            expect_same_workspace(actual_workspace, scalar_workspace, "two-axis");
        }
    }
}

void test_workspace_reuse(
    getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t width = 63;
    constexpr std::int32_t height = 53;
    constexpr std::int32_t stride = width + 5;
    const auto pixels = make_source(width, height, stride);
    const getnative::ConstImageView source{pixels.data(), width, height, stride};
    const getnative::MetricSpec metric{1, 2, 2, 3, 0.015F, 2U};

    const auto horizontal_small = getnative::build_axis_plan({
        width, 31, 31.25, -0.125, getnative::Filter::bilinear(),
        getnative::BorderMode::mirror,
    });
    const auto vertical_small = getnative::build_axis_plan({
        height, 29, 29.25, 0.125, getnative::Filter::bicubic(),
        getnative::BorderMode::repeat,
    });
    const auto horizontal_large = getnative::build_axis_plan({
        width, 47, 47.25, -0.1875, getnative::Filter::spline36(),
        getnative::BorderMode::zero,
    });
    const auto vertical_large = getnative::build_axis_plan({
        height, 41, 41.25, 0.1875, getnative::Filter::lanczos(4),
        getnative::BorderMode::mirror,
    });

    getnative::CpuWorkspace scalar_workspace;
    getnative::CpuWorkspace actual_workspace;
    const auto compare_axis = [&](const getnative::AxisPlan &plan,
                                  getnative::AnalysisAxes axes,
                                  std::string_view name) {
        const double scalar =
            getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                source, plan, axes, metric, scalar_workspace,
                getnative::detail::ColumnDispatchPolicy::scalar_only);
        const double actual =
            getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                source, plan, axes, metric, actual_workspace, policy);
        expect_same_double(actual, scalar, std::string{name} + " metric differs");
        expect_same_workspace(actual_workspace, scalar_workspace, name);
    };

    compare_axis(horizontal_small, getnative::AnalysisAxes::horizontal,
                 "workspace-reuse-horizontal-small");
    compare_axis(vertical_small, getnative::AnalysisAxes::vertical,
                 "workspace-reuse-vertical-small");

    const double scalar_both =
        getnative::detail::analyze_candidate_with_column_policy_f32(
            source, horizontal_large, vertical_large, metric, scalar_workspace,
            getnative::detail::ColumnDispatchPolicy::scalar_only);
    const double actual_both =
        getnative::detail::analyze_candidate_with_column_policy_f32(
            source, horizontal_large, vertical_large, metric, actual_workspace, policy);
    expect_same_double(actual_both, scalar_both, "workspace grow metric differs");
    expect_same_workspace(actual_workspace, scalar_workspace, "workspace-grow-both");

    compare_axis(vertical_small, getnative::AnalysisAxes::vertical,
                 "workspace-reuse-after-grow");
}

void test_all_filters_across_axes(
    getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t width = 33;
    constexpr std::int32_t height = 31;
    constexpr std::int32_t stride = width + 3;
    const auto pixels = make_source(width, height, stride);
    const getnative::ConstImageView source{pixels.data(), width, height, stride};
    const getnative::MetricSpec metric{1, 2, 2, 1, 0.015F, 1U};

    for (const auto &[name, filter] : filters()) {
        const auto horizontal = getnative::build_axis_plan({
            width, 19, 19.25, -0.125, filter, getnative::BorderMode::mirror,
        });
        const auto vertical = getnative::build_axis_plan({
            height, 17, 17.25, 0.125, filter, getnative::BorderMode::mirror,
        });
        for (const auto &[plan, axes] : std::vector<std::pair<
                 const getnative::AxisPlan *, getnative::AnalysisAxes>>{
                 {&horizontal, getnative::AnalysisAxes::horizontal},
                 {&vertical, getnative::AnalysisAxes::vertical}}) {
            getnative::CpuWorkspace scalar_workspace;
            getnative::CpuWorkspace actual_workspace;
            const double scalar =
                getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                    source, *plan, axes, metric, scalar_workspace,
                    getnative::detail::ColumnDispatchPolicy::scalar_only);
            const double actual =
                getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                    source, *plan, axes, metric, actual_workspace, policy);
            expect_same_double(actual, scalar, name + " single-axis metric differs");
            expect_same_workspace(actual_workspace, scalar_workspace, name);
        }

        getnative::CpuWorkspace scalar_workspace;
        getnative::CpuWorkspace actual_workspace;
        const double scalar = getnative::detail::analyze_candidate_with_column_policy_f32(
            source, horizontal, vertical, metric, scalar_workspace,
            getnative::detail::ColumnDispatchPolicy::scalar_only);
        const double actual = getnative::detail::analyze_candidate_with_column_policy_f32(
            source, horizontal, vertical, metric, actual_workspace, policy);
        expect_same_double(actual, scalar, name + " two-axis metric differs");
        expect_same_workspace(actual_workspace, scalar_workspace, name);
    }
}

void test_all_cpu_shape_widths(
    getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t source_size = 37;
    constexpr std::int32_t columns = 17;
    constexpr std::ptrdiff_t input_stride = columns + 3;
    constexpr std::ptrdiff_t output_stride = columns + 5;
    constexpr float padding = -381.5F;
    const auto source_pixels = make_source(columns, source_size, columns + 4);
    const getnative::ConstImageView source{
        source_pixels.data(), columns, source_size, columns + 4,
    };
    const getnative::MetricSpec metric{1, 2, 2, 3, 0.015F, 1U};

    std::vector<float> input(
        static_cast<std::size_t>(source_size * input_stride), padding);
    for (std::int32_t row = 0; row < source_size; ++row) {
        for (std::int32_t column = 0; column < columns; ++column) {
            input[static_cast<std::size_t>(row * input_stride + column)] =
                source_value(row, column);
        }
    }

    for (std::int32_t destination_size = 1; destination_size <= 30;
         ++destination_size) {
        const auto plan = getnative::build_axis_plan({
            source_size, destination_size,
            static_cast<double>(destination_size) + 0.25, -0.125,
            getnative::Filter::lanczos(15), getnative::BorderMode::mirror,
        });
        expect(plan.half_bandwidth == destination_size - 1,
               "shape fixture covers every half bandwidth from 0 through 29");
        expect(plan.forward_width == destination_size,
               "shape fixture covers every forward width from 1 through 30");

        const std::size_t output_elements = static_cast<std::size_t>(
            destination_size * output_stride);
        std::vector<float> scalar_output(output_elements, padding);
        std::vector<float> actual_output(output_elements, padding);
        getnative::detail::inverse_columns_f32(
            plan, input.data(), input_stride, scalar_output.data(), output_stride,
            columns, getnative::detail::ColumnDispatchPolicy::scalar_only);
        getnative::detail::inverse_columns_f32(
            plan, input.data(), input_stride, actual_output.data(), output_stride,
            columns, policy);
        // Production FMA/mul+add may diverge on long banded chains; metric below
        // is the correctness gate. Still require finite outputs and untouched padding.
        for (std::size_t index = 0; index < actual_output.size(); ++index) {
            if (!std::isfinite(actual_output[index])
                || !std::isfinite(scalar_output[index])) {
                throw std::runtime_error(
                    "all-shape inverse non-finite at half bandwidth "
                    + std::to_string(plan.half_bandwidth)
                    + " and storage index " + std::to_string(index));
            }
        }

        getnative::CpuWorkspace scalar_workspace;
        getnative::CpuWorkspace actual_workspace;
        const double scalar =
            getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                source, plan, getnative::AnalysisAxes::vertical, metric,
                scalar_workspace,
                getnative::detail::ColumnDispatchPolicy::scalar_only);
        const double actual =
            getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                source, plan, getnative::AnalysisAxes::vertical, metric,
                actual_workspace, policy);
        expect_same_double(
            actual, scalar,
            "all-shape metric differs at forward width "
                + std::to_string(plan.forward_width));
        expect_same_workspace(actual_workspace, scalar_workspace, "all-shape");
    }
}

void test_absolute_difference_block_boundaries(
    getnative::detail::ColumnDispatchPolicy policy) {
    const getnative::detail::AnalysisRowDispatch dispatch =
        getnative::detail::analysis_row_dispatch(policy);
    if (dispatch.absolute_difference == nullptr) return;

    const float threshold = 0.015F;
    const float below = std::nextafter(threshold, 0.0F);
    const float above = std::nextafter(threshold, 1.0F);
    const std::vector<float> reconstruction_values{
        0.0F,
        -0.0F,
        std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(),
        std::numeric_limits<float>::min(),
        -std::numeric_limits<float>::min(),
        std::numeric_limits<float>::max() / 2048.0F,
        -std::numeric_limits<float>::max() / 2048.0F,
        below,
        threshold,
        above,
        -below,
        -threshold,
        -above,
    };
    const std::size_t block_count =
        (reconstruction_values.size() + static_cast<std::size_t>(dispatch.lanes) - 1U)
        / static_cast<std::size_t>(dispatch.lanes);
    std::vector<float> source(
        block_count * static_cast<std::size_t>(dispatch.lanes), 0.0F);
    std::vector<float> reconstruction(source.size(), -0.0F);
    std::copy(reconstruction_values.begin(), reconstruction_values.end(),
              reconstruction.begin());
    std::vector<float> differences(source.size(), -1.0F);

    for (std::size_t block = 0; block < block_count; ++block) {
        const std::size_t offset = block * static_cast<std::size_t>(dispatch.lanes);
        dispatch.absolute_difference(
            source.data() + offset, reconstruction.data() + offset,
            differences.data() + offset);
    }
    for (std::size_t index = 0; index < reconstruction_values.size(); ++index) {
        expect_same_float(
            differences[index], std::abs(source[index] - reconstruction[index]),
            "SIMD absolute-difference boundary value differs at "
                + std::to_string(index));
    }
}

void test_vertical_reconstruction_norm1_fusion(
    getnative::detail::ColumnDispatchPolicy policy) {
    const getnative::detail::AnalysisRowDispatch dispatch =
        getnative::detail::analysis_row_dispatch(policy);
    if (dispatch.vertical_reconstruction == nullptr
        || dispatch.vertical_reconstruction_norm1 == nullptr) {
        return;
    }

    constexpr float threshold = 0.015F;
    const float below = std::nextafter(threshold, 0.0F);
    const float above = std::nextafter(threshold, 1.0F);
    const std::vector<float> values{
        0.0F,
        -0.0F,
        std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(),
        below,
        threshold,
        above,
        -below,
        -threshold,
        -above,
        0.125F,
        -0.25F,
    };
    const std::int32_t x_begin = 1;
    const std::int32_t x_end = x_begin + 3 * dispatch.lanes;
    std::vector<float> source(static_cast<std::size_t>(x_end), 0.0F);
    for (std::int32_t x = x_begin; x < x_end; ++x) {
        source[static_cast<std::size_t>(x)] = values[
            static_cast<std::size_t>(x - x_begin) % values.size()];
    }

    const auto plan = getnative::build_axis_plan({
        31, 17, 17.25, 0.125, getnative::Filter::bicubic(0.0, 0.5),
        getnative::BorderMode::mirror,
    });
    const std::int32_t y = 15;
    const std::uint32_t begin = plan.forward_offsets[static_cast<std::size_t>(y)];
    const std::int32_t left = plan.forward_indices[begin];
    const std::ptrdiff_t native_stride = x_end + 3;
    std::vector<float> native(
        static_cast<std::size_t>(plan.destination_size * native_stride), 0.0F);

    double expected = 0.375;
    alignas(64) float differences[16];
    for (std::int32_t x = x_begin; x < x_end; x += dispatch.lanes) {
        dispatch.vertical_reconstruction(
            plan, begin, left, source.data(), native.data(), native_stride,
            x, differences);
        for (std::int32_t lane = 0; lane < dispatch.lanes; ++lane) {
            if (differences[lane] > threshold) {
                expected += static_cast<double>(differences[lane]);
            }
        }
    }
    const double actual = dispatch.vertical_reconstruction_norm1(
        plan, begin, left, source.data(), native.data(), native_stride,
        x_begin, x_end, threshold, 0.375);
    expect(std::bit_cast<std::uint64_t>(actual)
               == std::bit_cast<std::uint64_t>(expected),
           "fused norm-1 row preserves strict threshold and accumulation order");
}

void test_absolute_difference_norm1_fusion(
    getnative::detail::ColumnDispatchPolicy policy) {
    const getnative::detail::AnalysisRowDispatch dispatch =
        getnative::detail::analysis_row_dispatch(policy);
    if (dispatch.absolute_difference == nullptr
        || dispatch.absolute_difference_norm1 == nullptr) {
        return;
    }

    constexpr float threshold = 0.015F;
    const float below = std::nextafter(threshold, 0.0F);
    const float above = std::nextafter(threshold, 1.0F);
    const std::vector<float> values{
        0.0F,
        -0.0F,
        std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(),
        below,
        threshold,
        above,
        -below,
        -threshold,
        -above,
        0.125F,
        -0.25F,
    };
    const std::int32_t x_begin = 1;
    const std::int32_t x_end = x_begin + 3 * dispatch.lanes;
    std::vector<float> source(static_cast<std::size_t>(x_end), 0.0F);
    std::vector<float> reconstruction(static_cast<std::size_t>(x_end), 0.0F);
    for (std::int32_t x = x_begin; x < x_end; ++x) {
        reconstruction[static_cast<std::size_t>(x)] = values[
            static_cast<std::size_t>(x - x_begin) % values.size()];
    }

    double expected = 0.375;
    alignas(64) float differences[16];
    for (std::int32_t x = x_begin; x < x_end; x += dispatch.lanes) {
        dispatch.absolute_difference(
            source.data() + x, reconstruction.data() + x, differences);
        for (std::int32_t lane = 0; lane < dispatch.lanes; ++lane) {
            if (differences[lane] > threshold) {
                expected += static_cast<double>(differences[lane]);
            }
        }
    }
    const double actual = dispatch.absolute_difference_norm1(
        source.data(), reconstruction.data(), x_begin, x_end, threshold, 0.375);
    expect(std::bit_cast<std::uint64_t>(actual)
               == std::bit_cast<std::uint64_t>(expected),
           "fused norm-1 absolute difference preserves strict threshold and accumulation order");
}

void test_batch_surface(getnative::detail::ColumnDispatchPolicy policy) {
    constexpr std::int32_t width = 33;
    constexpr std::int32_t height = 31;
    constexpr std::int32_t stride = width + 4;
    const auto pixels = make_source(width, height, stride);
    const getnative::ConstImageView source{pixels.data(), width, height, stride};
    const auto horizontal = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            width, 25, 25.25, 0.125, getnative::Filter::spline64(),
            getnative::BorderMode::mirror,
        }));
    const auto vertical = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            height, 23, 23.25, -0.125, getnative::Filter::lanczos(8),
            getnative::BorderMode::zero,
        }));
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"v-first", nullptr, vertical, getnative::AnalysisAxes::vertical},
        {"both", horizontal, vertical, getnative::AnalysisAxes::both},
        {"h-last", horizontal, nullptr, getnative::AnalysisAxes::horizontal},
    };
    const getnative::MetricSpec metric{1, 1, 1, 1, 0.015F, 3U};
    const auto scalar = getnative::detail::analyze_batch_with_column_policy_f32(
        source, candidates, metric,
        getnative::detail::ColumnDispatchPolicy::scalar_only, 2U);
    const auto actual = getnative::detail::analyze_batch_with_column_policy_f32(
        source, candidates, metric, policy, 3U);
    expect(actual.size() == scalar.size(), "batch result sizes match");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expect(actual[index].id == candidates[index].id
                   && scalar[index].id == candidates[index].id,
               "batch retains candidate id and order");
        expect_same_double(actual[index].error, scalar[index].error,
                           "batch metric differs from scalar beyond production tolerance");
    }
    const std::vector<getnative::CandidateAnalysis> empty;
    expect(getnative::detail::analyze_batch_with_column_policy_f32(
               source, empty, metric, policy, 4U).empty(),
           "forced batch accepts an empty candidate set");
}

void test_threshold_boundary() {
    const float threshold = 0.015F;
    const float below = std::nextafter(threshold, 0.0F);
    const float above = std::nextafter(threshold, 1.0F);
    const std::vector<float> source{0.0F, 0.0F, 0.0F};
    const std::vector<float> reconstruction{below, threshold, above};
    const getnative::MetricSpec metric{0, 0, 0, 0, threshold, 1U};
    const double result = getnative::thresholded_p_norm(
        {source.data(), 3, 1, 3}, {reconstruction.data(), 3, 1, 3}, metric);
    expect_same_double(result, static_cast<double>(above) / 3.0,
                       "only difference > threshold contributes");
}

void test_mxcsr_unchanged(
    getnative::detail::ColumnDispatchPolicy policy) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
    const unsigned int before = _mm_getcsr();
    const auto plan = getnative::build_axis_plan({
        79, 43, 43.25, -0.1875, getnative::Filter::spline36(),
        getnative::BorderMode::mirror,
    });
    const auto input = make_source(49, 79, 57);
    std::vector<float> output(43U * 61U, 0.0F);
    const unsigned int non_default =
        (before & ~static_cast<unsigned int>(_MM_ROUND_MASK))
        | static_cast<unsigned int>(_MM_ROUND_DOWN);
    _mm_setcsr(non_default);
    try {
        getnative::detail::inverse_columns_f32(
            plan, input.data(), 57, output.data(), 61, 49, policy);
        expect(_mm_getcsr() == non_default,
               "CPU library changed a caller-provided non-default MXCSR");
    } catch (...) {
        _mm_setcsr(before);
        throw;
    }
    _mm_setcsr(before);
    expect(_mm_getcsr() == before, "MXCSR restoration failed in the test harness");
#else
    (void)policy;
#endif
}

[[nodiscard]] getnative::CpuIsaRequest parse_arguments(int argc, char **argv) {
    getnative::CpuIsaRequest result = getnative::CpuIsaRequest::automatic;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--cpu-isa" && index + 1 < argc) {
            const auto parsed = getnative::parse_cpu_isa_request(argv[++index]);
            if (!parsed) throw std::invalid_argument("unknown --cpu-isa value");
            result = *parsed;
        } else if (argument == "--help") {
            std::cout << "usage: getnative_cpu_column_simd_tests "
                         "[--cpu-isa auto|scalar|sse2|avx2|avx512]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown argument: " + std::string{argument});
        }
    }
    return result;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const getnative::CpuIsaRequest request = parse_arguments(argc, argv);
        test_feature_evaluator();

        const getnative::CpuDispatchInfo dispatch = getnative::cpu_dispatch_info(request);
        if (!dispatch.request_available) {
            (void)getnative::require_cpu_isa(request);
        }
        const auto policy = getnative::detail::column_dispatch_policy(request);
        test_materialized_inverse_columns(policy, dispatch.selected);
        test_materialized_inverse_rows(policy);
        test_materialized_forward_columns(policy, dispatch.selected);
        test_materialized_forward_rows(policy);
        test_cropped_inverse_column_ranges(policy);
        test_avx2_b5_row_major_boundaries(policy, dispatch.selected);
        test_axis_and_two_axis_candidates(policy);
        test_workspace_reuse(policy);
        test_all_filters_across_axes(policy);
        test_crop_aware_analysis_matches_full_frame_oracle(policy);
        test_all_cpu_shape_widths(policy);
        test_absolute_difference_block_boundaries(policy);
        test_vertical_reconstruction_norm1_fusion(policy);
        test_absolute_difference_norm1_fusion(policy);
        test_batch_surface(policy);
        test_threshold_boundary();
        test_mxcsr_unchanged(policy);

        std::cout << "cpu column SIMD tests passed"
                  << "; requested=" << getnative::cpu_isa_request_name(request)
                  << "; selected=" << getnative::cpu_isa_name(dispatch.selected)
                  << "; forced=" << (dispatch.forced ? "true" : "false")
                  << "; math_mode=" << dispatch.math_mode
                  << "; vendor=" << getnative::cpu_vendor(dispatch.snapshot)
                  << "; family=" << dispatch.snapshot.family
                  << "; model=" << dispatch.snapshot.model
                  << "; stepping=" << dispatch.snapshot.stepping
                  << "; xcr0=0x" << std::hex << dispatch.snapshot.xcr0 << std::dec
                  << "; reason=" << dispatch.selection_reason << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "cpu column SIMD test failure: " << error.what() << '\n';
        return 1;
    }
}

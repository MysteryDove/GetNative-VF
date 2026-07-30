#include "inverse_columns.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

using NamedFilter = std::pair<std::string_view, getnative::Filter>;

[[nodiscard]] const std::vector<NamedFilter> &filters() {
    static const std::vector<NamedFilter> values{
        {"bilinear", getnative::Filter::bilinear()},
        {"bicubic-catrom", getnative::Filter::bicubic(0.0, 0.5)},
        {"bicubic-mitchell", getnative::Filter::bicubic(1.0 / 3.0, 1.0 / 3.0)},
        {"bicubic-bspline", getnative::Filter::bicubic(1.0, 0.0)},
        {"bicubic-zero", getnative::Filter::bicubic(0.0, 0.0)},
        {"spline16", getnative::Filter::spline16()},
        {"spline36", getnative::Filter::spline36()},
        {"spline64", getnative::Filter::spline64()},
        {"lanczos1", getnative::Filter::lanczos(1)},
        {"lanczos2", getnative::Filter::lanczos(2)},
        {"lanczos3", getnative::Filter::lanczos(3)},
        {"lanczos4", getnative::Filter::lanczos(4)},
        {"lanczos5", getnative::Filter::lanczos(5)},
        {"lanczos6", getnative::Filter::lanczos(6)},
        {"lanczos7", getnative::Filter::lanczos(7)},
        {"lanczos8", getnative::Filter::lanczos(8)},
    };
    return values;
}

[[nodiscard]] float source_value(std::int32_t row, std::int32_t column) {
    const int selector = (row * 7 + column * 11) % 13;
    if (selector == 0) return std::numeric_limits<float>::denorm_min();
    if (selector == 1) return -std::numeric_limits<float>::denorm_min();
    if (selector == 2) return std::numeric_limits<float>::min();
    if (selector == 3) return -std::numeric_limits<float>::min();
    if (selector == 4) return 0.0F;
    return static_cast<float>(
        0.37 + 0.21 * std::sin(0.073 * static_cast<double>(row))
        + 0.17 * std::cos(0.091 * static_cast<double>(column))
        + 0.05 * std::sin(0.037 * static_cast<double>(row + column)));
}

void expect_same_float(float actual, float expected, std::string_view message) {
    if (std::bit_cast<std::uint32_t>(actual) != std::bit_cast<std::uint32_t>(expected)) {
        throw std::runtime_error(std::string{message});
    }
}

void test_materialized_inverse_columns() {
    constexpr std::int32_t source_size = 53;
    constexpr std::int32_t destination_size = 37;
    constexpr float padding = -913.25F;
    const std::vector<getnative::BorderMode> borders{
        getnative::BorderMode::zero,
        getnative::BorderMode::repeat,
        getnative::BorderMode::mirror,
    };
    for (const auto &[name, filter] : filters()) {
        for (const getnative::BorderMode border : borders) {
            const auto plan = getnative::build_axis_plan({
                source_size, destination_size, 37.25, -0.1875, filter, border,
            });
            for (const std::int32_t columns : {1, 2, 3, 4, 5, 7, 17}) {
                const std::ptrdiff_t input_stride = columns + 3;
                const std::ptrdiff_t output_stride = columns + 5;
                std::vector<float> input(
                    static_cast<std::size_t>(source_size * input_stride), padding);
                for (std::int32_t row = 0; row < source_size; ++row) {
                    for (std::int32_t column = 0; column < columns; ++column) {
                        input[static_cast<std::size_t>(row * input_stride + column)] =
                            source_value(row, column);
                    }
                }
                std::vector<float> scalar(
                    static_cast<std::size_t>(destination_size * output_stride), padding);
                std::vector<float> automatic = scalar;
                getnative::detail::inverse_columns_f32(
                    plan, input.data(), input_stride, scalar.data(), output_stride, columns,
                    getnative::detail::ColumnDispatchPolicy::scalar_only);
                getnative::detail::inverse_columns_f32(
                    plan, input.data(), input_stride, automatic.data(), output_stride, columns,
                    getnative::detail::ColumnDispatchPolicy::automatic);
                for (std::int32_t row = 0; row < destination_size; ++row) {
                    for (std::int32_t column = 0; column < columns; ++column) {
                        expect_same_float(
                            automatic[static_cast<std::size_t>(row * output_stride + column)],
                            scalar[static_cast<std::size_t>(row * output_stride + column)],
                            std::string{name} + " automatic inverse differs from scalar bits");
                    }
                    for (std::int32_t column = columns; column < output_stride; ++column) {
                        expect_same_float(
                            automatic[static_cast<std::size_t>(row * output_stride + column)],
                            padding, "SIMD inverse overwrote row padding");
                    }
                }

                if (getnative::detail::column_simd_available()) {
                    std::vector<float> required(
                        static_cast<std::size_t>(destination_size * output_stride), padding);
                    getnative::detail::inverse_columns_f32(
                        plan, input.data(), input_stride, required.data(), output_stride, columns,
                        getnative::detail::ColumnDispatchPolicy::required_simd);
                    for (std::size_t index = 0; index < required.size(); ++index) {
                        expect_same_float(
                            required[index], automatic[index],
                            "required SIMD inverse differs from automatic bits");
                    }
                } else {
                    expect_throws<std::runtime_error>(
                        [&] {
                            getnative::detail::inverse_columns_f32(
                                plan, input.data(), input_stride, automatic.data(), output_stride,
                                columns, getnative::detail::ColumnDispatchPolicy::required_simd);
                        },
                        "non-NEON host rejects required SIMD");
                }
            }
        }
    }
}

void test_whole_vertical_candidate() {
    constexpr std::int32_t height = 53;
    constexpr std::int32_t native_height = 37;
    const std::vector<getnative::MetricSpec> metrics{
        {0, 0, 0, 0, 0.015F, 1U},
        {1, 1, 2, 3, 0.015F, 1U},
        {1, 1, 2, 3, 0.015F, 2U},
    };
    for (const auto &[name, filter] : filters()) {
        for (const getnative::BorderMode border : {
                 getnative::BorderMode::zero,
                 getnative::BorderMode::repeat,
                 getnative::BorderMode::mirror}) {
            const auto plan = getnative::build_axis_plan({
                height, native_height, 37.25, 0.1875, filter, border,
            });
            for (const std::int32_t width : {3, 5, 17, 63}) {
                const std::int32_t stride = width + 3;
                std::vector<float> pixels(
                    static_cast<std::size_t>(height * stride), -77.0F);
                for (std::int32_t y = 0; y < height; ++y) {
                    for (std::int32_t x = 0; x < width; ++x) {
                        pixels[static_cast<std::size_t>(y * stride + x)] = source_value(y, x);
                    }
                }
                const getnative::ConstImageView source{
                    pixels.data(), width, height, stride,
                };
                for (const getnative::MetricSpec &metric : metrics) {
                    getnative::CpuWorkspace scalar_workspace;
                    getnative::CpuWorkspace automatic_workspace;
                    const double scalar =
                        getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                            source, plan, getnative::AnalysisAxes::vertical, metric,
                            scalar_workspace,
                            getnative::detail::ColumnDispatchPolicy::scalar_only);
                    const double automatic =
                        getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                            source, plan, getnative::AnalysisAxes::vertical, metric,
                            automatic_workspace,
                            getnative::detail::ColumnDispatchPolicy::automatic);
                    if (std::bit_cast<std::uint64_t>(automatic)
                        != std::bit_cast<std::uint64_t>(scalar)) {
                        throw std::runtime_error(std::string{name}
                                                 + " whole-candidate metric differs from scalar bits");
                    }
                    expect(automatic_workspace.native.size() == scalar_workspace.native.size(),
                           "automatic and scalar workspaces have matching sizes");
                    for (std::size_t index = 0; index < scalar_workspace.native.size(); ++index) {
                        expect_same_float(
                            automatic_workspace.native[index], scalar_workspace.native[index],
                            "whole-candidate inverse workspace differs from scalar bits");
                    }
                }
            }
        }
    }
}

} // namespace

int main() {
    try {
        test_materialized_inverse_columns();
        test_whole_vertical_candidate();
        std::cout << "cpu column SIMD tests passed; selected="
                  << getnative::detail::column_simd_name() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "cpu column SIMD test failure: " << error.what() << '\n';
        return 1;
    }
}

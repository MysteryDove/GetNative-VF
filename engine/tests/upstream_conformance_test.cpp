#include "getnative/axis_plan.hpp"
#include "getnative/filter.hpp"

extern "C" {
#include "descale.h"
}

#include "resize/filter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

[[nodiscard]] bool same_bits(float lhs, float rhs) noexcept {
    return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs);
}

constexpr double output_absolute_tolerance = 2.0e-6;
constexpr double output_relative_tolerance = 2.0e-6;

struct OutputErrorStats {
    double maximum_absolute = 0.0;
    double maximum_scaled = 0.0;
    double maximum_tolerance_ratio = 0.0;
    std::string maximum_absolute_case;
    std::string maximum_scaled_case;
    std::string maximum_tolerance_case;
    std::int32_t maximum_absolute_index = -1;
    std::int32_t maximum_scaled_index = -1;
    std::int32_t maximum_tolerance_index = -1;
};

void compare_output(std::string_view name, std::int32_t index,
                    float actual, float expected, OutputErrorStats &stats) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) {
        expect(same_bits(actual, expected),
               std::string{name} + " produced a non-finite output mismatch");
        return;
    }

    const double absolute = std::abs(static_cast<double>(actual)
                                     - static_cast<double>(expected));
    const double scale = std::max(1.0, std::abs(static_cast<double>(expected)));
    const double tolerance = std::max(
        output_absolute_tolerance,
        output_relative_tolerance * std::abs(static_cast<double>(expected)));
    const double scaled = absolute / scale;
    const double tolerance_ratio = absolute / tolerance;
    if (absolute > stats.maximum_absolute) {
        stats.maximum_absolute = absolute;
        stats.maximum_absolute_case = std::string{name};
        stats.maximum_absolute_index = index;
    }
    if (scaled > stats.maximum_scaled) {
        stats.maximum_scaled = scaled;
        stats.maximum_scaled_case = std::string{name};
        stats.maximum_scaled_index = index;
    }
    if (tolerance_ratio > stats.maximum_tolerance_ratio) {
        stats.maximum_tolerance_ratio = tolerance_ratio;
        stats.maximum_tolerance_case = std::string{name};
        stats.maximum_tolerance_index = index;
    }
    if (absolute > tolerance) {
        throw std::runtime_error(
            std::string{name} + " descale output exceeds numerical tolerance at index "
            + std::to_string(index) + ": absolute error="
            + std::to_string(absolute) + ", tolerance="
            + std::to_string(tolerance));
    }
}

[[nodiscard]] DescaleBorder descale_border(getnative::BorderMode border) {
    switch (border) {
    case getnative::BorderMode::zero: return DESCALE_BORDER_ZERO;
    case getnative::BorderMode::repeat: return DESCALE_BORDER_REPEAT;
    case getnative::BorderMode::mirror: return DESCALE_BORDER_MIRROR;
    }
    throw std::runtime_error("unknown border mode");
}

void compare_descale(std::string_view name, const getnative::Filter &filter,
                     DescaleMode mode, getnative::BorderMode border,
                     OutputErrorStats &output_stats) {
    constexpr std::int32_t source_size = 37;
    constexpr std::int32_t destination_size = 23;
    const getnative::AxisPlanRequest request{
        source_size, destination_size, 23.4, -0.375, filter, border,
    };
    const auto plan = getnative::build_axis_plan(request);

    DescaleParams params{};
    params.mode = mode;
    params.taps = filter.taps;
    params.param1 = filter.b;
    params.param2 = filter.c;
    params.shift = request.shift;
    params.active_dim = request.active_length;
    params.border_handling = descale_border(border);
    const DescaleAPI api = get_descale_api(DESCALE_OPT_NONE);
    DescaleCore *raw_core = api.create_core(source_size, destination_size, &params);
    expect(raw_core != nullptr, "descale reference plan creation failed");
    const auto free_core = [api](DescaleCore *core) { api.free_core(core); };
    const std::unique_ptr<DescaleCore, decltype(free_core)> core(raw_core, free_core);

    expect(plan.half_bandwidth == core->bandwidth / 2,
           "descale half-bandwidth differs");
    for (std::int32_t row = 0; row < destination_size; ++row) {
        expect(same_bits(plan.inverse_diagonal[static_cast<std::size_t>(row)],
                         core->diagonal[row]),
               "descale inverse diagonal differs");

        const auto begin = plan.transpose_offsets[static_cast<std::size_t>(row)];
        const auto end = plan.transpose_offsets[static_cast<std::size_t>(row) + 1U];
        for (std::int32_t source = core->weights_left_idx[row];
             source < core->weights_right_idx[row]; ++source) {
            float actual = 0.0F;
            for (std::uint32_t index = begin; index < end; ++index) {
                if (plan.transpose_indices[index] == source) {
                    actual = plan.transpose_weights[index];
                    break;
                }
            }
            const float expected = core->weights[
                static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(core->weights_columns)
                + static_cast<std::size_t>(source - core->weights_left_idx[row])];
            expect(same_bits(actual, expected), "descale transpose coefficient differs");
        }

        for (std::int32_t distance = 1;
             distance <= plan.half_bandwidth && row + distance < destination_size;
             ++distance) {
            const float actual = plan.upper_l[
                static_cast<std::size_t>(distance - 1)
                    * static_cast<std::size_t>(destination_size)
                + static_cast<std::size_t>(row)];
            expect(same_bits(actual, core->upper[distance - 1][row]),
                   "descale upper factor differs");
        }
        for (std::int32_t distance = 1;
             distance <= plan.half_bandwidth && distance <= row; ++distance) {
            const float actual = plan.lower_ld[
                static_cast<std::size_t>(distance - 1)
                    * static_cast<std::size_t>(destination_size)
                + static_cast<std::size_t>(row)];
            const float expected = core->lower[plan.half_bandwidth - distance][row];
            expect(same_bits(actual, expected), "descale lower factor differs");
        }
    }

    std::vector<float> input(static_cast<std::size_t>(source_size));
    std::vector<float> actual(static_cast<std::size_t>(destination_size));
    std::vector<float> expected(static_cast<std::size_t>(destination_size));
    for (std::int32_t i = 0; i < source_size; ++i) {
        input[static_cast<std::size_t>(i)] =
            static_cast<float>(std::sin(static_cast<double>(i) * 0.37)
                               + static_cast<double>(i) * 0.011);
    }
    getnative::inverse_axis_f32(plan, input, actual);
    api.process_vectors(core.get(), DESCALE_DIR_HORIZONTAL, 1,
                        source_size, destination_size, input.data(), expected.data());
    for (std::int32_t i = 0; i < destination_size; ++i) {
        compare_output(name, i, actual[static_cast<std::size_t>(i)],
                       expected[static_cast<std::size_t>(i)], output_stats);
    }
}

void compare_zimg(std::string_view name, const getnative::Filter &filter,
                  const zimg::resize::Filter &zimg_filter) {
    constexpr std::int32_t source_size = 37;
    constexpr std::int32_t destination_size = 23;
    const getnative::AxisPlanRequest request{
        source_size, destination_size, 23.4, -0.375, filter,
        getnative::BorderMode::mirror,
    };
    const auto plan = getnative::build_axis_plan(request);
    const auto reference = zimg::resize::compute_filter(
        zimg_filter, static_cast<unsigned>(destination_size),
        static_cast<unsigned>(source_size), request.shift, request.active_length);

    expect(plan.forward_width == static_cast<std::int32_t>(reference.filter_width),
           "zimg forward width differs");
    for (std::int32_t row = 0; row < source_size; ++row) {
        const auto begin = plan.forward_offsets[static_cast<std::size_t>(row)];
        expect(plan.forward_indices[begin]
                   == static_cast<std::int32_t>(reference.left[static_cast<unsigned>(row)]),
               "zimg forward left offset differs");
        for (unsigned tap = 0; tap < reference.filter_width; ++tap) {
            const float actual = plan.forward_weights[begin + tap];
            const float expected = reference.data[
                static_cast<std::size_t>(row) * reference.stride + tap];
            if (!same_bits(actual, expected)) {
                throw std::runtime_error(std::string{name}
                                         + " zimg forward coefficient differs");
            }
        }
    }
}

template <class ZimgFilter>
void run_filter(std::string_view name, const getnative::Filter &filter,
                DescaleMode mode, ZimgFilter zimg_filter,
                OutputErrorStats &output_stats) {
    for (const auto border : {getnative::BorderMode::zero,
                              getnative::BorderMode::repeat,
                              getnative::BorderMode::mirror}) {
        compare_descale(name, filter, mode, border, output_stats);
    }
    compare_zimg(name, filter, zimg_filter);
}

} // namespace

int main() {
    try {
        OutputErrorStats output_stats;
        run_filter("bilinear", getnative::Filter::bilinear(),
                   DESCALE_MODE_BILINEAR, zimg::resize::BilinearFilter{}, output_stats);
        run_filter("bicubic", getnative::Filter::bicubic(),
                   DESCALE_MODE_BICUBIC, zimg::resize::BicubicFilter{0.0, 0.5}, output_stats);
        run_filter("bicubic-b025-c040", getnative::Filter::bicubic(0.25, 0.4),
                   DESCALE_MODE_BICUBIC, zimg::resize::BicubicFilter{0.25, 0.4}, output_stats);
        for (unsigned taps = 1; taps <= 8; ++taps) {
            run_filter("lanczos" + std::to_string(taps),
                       getnative::Filter::lanczos(static_cast<std::int32_t>(taps)),
                       DESCALE_MODE_LANCZOS, zimg::resize::LanczosFilter{taps}, output_stats);
        }
        run_filter("spline16", getnative::Filter::spline16(),
                   DESCALE_MODE_SPLINE16, zimg::resize::Spline16Filter{}, output_stats);
        run_filter("spline36", getnative::Filter::spline36(),
                   DESCALE_MODE_SPLINE36, zimg::resize::Spline36Filter{}, output_stats);
        run_filter("spline64", getnative::Filter::spline64(),
                   DESCALE_MODE_SPLINE64, zimg::resize::Spline64Filter{}, output_stats);
        std::cout << "all upstream conformance tests passed; max_abs_error="
                  << output_stats.maximum_absolute << " ("
                  << output_stats.maximum_absolute_case << "["
                  << output_stats.maximum_absolute_index << "])"
                  << ", max_scaled_error=" << output_stats.maximum_scaled
                  << " (" << output_stats.maximum_scaled_case << "["
                  << output_stats.maximum_scaled_index << "])"
                  << ", max_tolerance_ratio="
                  << output_stats.maximum_tolerance_ratio << " ("
                  << output_stats.maximum_tolerance_case << "["
                  << output_stats.maximum_tolerance_index << "])\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "upstream conformance failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

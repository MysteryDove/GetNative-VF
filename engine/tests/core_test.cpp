#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void expect_close(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string{message} + ": actual=" + std::to_string(actual)
                                 + ", expected=" + std::to_string(expected));
    }
}

template <class Exception = std::invalid_argument, class Function>
void expect_throws(Function &&function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

[[nodiscard]] double oracle_weight(const getnative::Filter &filter, double distance) {
    const double x = std::abs(distance);
    const auto square = [](double value) { return value * value; };
    const auto cube = [&](double value) { return square(value) * value; };
    switch (filter.type) {
    case getnative::KernelType::bilinear:
        return x < 1.0 ? 1.0 - x : 0.0;
    case getnative::KernelType::bicubic:
        if (x < 1.0) {
            return ((12.0 - 9.0 * filter.b - 6.0 * filter.c) * cube(x)
                    + (-18.0 + 12.0 * filter.b + 6.0 * filter.c) * square(x)
                    + 6.0 - 2.0 * filter.b) / 6.0;
        }
        if (x < 2.0) {
            return ((-filter.b - 6.0 * filter.c) * cube(x)
                    + (6.0 * filter.b + 30.0 * filter.c) * square(x)
                    + (-12.0 * filter.b - 48.0 * filter.c) * x
                    + 8.0 * filter.b + 24.0 * filter.c) / 6.0;
        }
        return 0.0;
    case getnative::KernelType::lanczos: {
        if (filter.taps <= 0 || x >= static_cast<double>(filter.taps)) return 0.0;
        const auto sinc = [](double value) {
            if (value == 0.0) return 1.0;
            const double angle = std::numbers::pi * value;
            return std::sin(angle) / angle;
        };
        return sinc(x) * sinc(x / static_cast<double>(filter.taps));
    }
    case getnative::KernelType::spline16:
        if (x < 1.0) return 1.0 - x / 5.0 - 9.0 * square(x) / 5.0 + cube(x);
        if (x < 2.0) { const double y = x - 1.0; return -7.0 * y / 15.0 + 4.0 * square(y) / 5.0 - cube(y) / 3.0; }
        return 0.0;
    case getnative::KernelType::spline36:
        if (x < 1.0) return 1.0 - 3.0 * x / 209.0 - 453.0 * square(x) / 209.0 + 13.0 * cube(x) / 11.0;
        if (x < 2.0) { const double y = x - 1.0; return -156.0 * y / 209.0 + 270.0 * square(y) / 209.0 - 6.0 * cube(y) / 11.0; }
        if (x < 3.0) { const double y = x - 2.0; return 26.0 * y / 209.0 - 45.0 * square(y) / 209.0 + cube(y) / 11.0; }
        return 0.0;
    case getnative::KernelType::spline64:
        if (x < 1.0) return 1.0 - 3.0 * x / 2911.0 - 6387.0 * square(x) / 2911.0 + 49.0 * cube(x) / 41.0;
        if (x < 2.0) { const double y = x - 1.0; return -2328.0 * y / 2911.0 + 4032.0 * square(y) / 2911.0 - 24.0 * cube(y) / 41.0; }
        if (x < 3.0) { const double y = x - 2.0; return 582.0 * y / 2911.0 - 1008.0 * square(y) / 2911.0 + 6.0 * cube(y) / 41.0; }
        if (x < 4.0) { const double y = x - 3.0; return -97.0 * y / 2911.0 + 168.0 * square(y) / 2911.0 - cube(y) / 41.0; }
        return 0.0;
    }
    throw std::runtime_error("unknown oracle kernel");
}

[[nodiscard]] double round_half_up(double value) {
    return value < 0.0 ? std::floor(value + 0.5)
                       : std::floor(value + 0.49999999999999994);
}

[[nodiscard]] std::vector<double> dense_forward(const getnative::AxisPlanRequest &request) {
    const std::int32_t support = request.filter.support();
    const double ratio = static_cast<double>(request.source_size) / request.active_length;
    std::vector<double> matrix(static_cast<std::size_t>(request.source_size)
                               * static_cast<std::size_t>(request.destination_size));
    for (std::int32_t row = 0; row < request.source_size; ++row) {
        const double position = (static_cast<double>(row) + 0.5) / ratio + request.shift;
        const double begin = round_half_up(position - static_cast<double>(support)) + 0.5;
        double total = 0.0;
        for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
            total += oracle_weight(request.filter, begin + static_cast<double>(tap) - position);
        }
        for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
            const double center = begin + static_cast<double>(tap);
            double mapped = center;
            if (center < 0.0 || center >= static_cast<double>(request.destination_size)) {
                if (request.border == getnative::BorderMode::zero) continue;
                if (request.border == getnative::BorderMode::repeat) {
                    mapped = center < 0.0 ? 0.0 : static_cast<double>(request.destination_size) - 0.5;
                } else {
                    mapped = center < 0.0
                        ? -center
                        : std::min(2.0 * static_cast<double>(request.destination_size) - center,
                                   static_cast<double>(request.destination_size) - 0.5);
                }
            }
            const auto column = static_cast<std::int32_t>(std::floor(mapped));
            if (column >= 0 && column < request.destination_size) {
                matrix[static_cast<std::size_t>(row) * static_cast<std::size_t>(request.destination_size)
                       + static_cast<std::size_t>(column)] += oracle_weight(request.filter, center - position) / total;
            }
        }
    }
    return matrix;
}

[[nodiscard]] std::vector<double> dense_least_squares(const std::vector<double> &a,
                                                       std::int32_t rows, std::int32_t columns,
                                                       const std::vector<float> &observations) {
    const auto n = static_cast<std::size_t>(columns);
    std::vector<double> normal(n * n);
    std::vector<double> rhs(n);
    for (std::int32_t row = 0; row < rows; ++row) {
        for (std::int32_t i = 0; i < columns; ++i) {
            const double ai = a[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(i)];
            rhs[static_cast<std::size_t>(i)] += ai * observations[static_cast<std::size_t>(row)];
            for (std::int32_t j = 0; j < columns; ++j) {
                normal[static_cast<std::size_t>(i) * n + static_cast<std::size_t>(j)] +=
                    ai * a[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(j)];
            }
        }
    }
    for (std::int32_t pivot = 0; pivot < columns; ++pivot) {
        std::int32_t best = pivot;
        for (std::int32_t row = pivot + 1; row < columns; ++row) {
            if (std::abs(normal[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(pivot)])
                > std::abs(normal[static_cast<std::size_t>(best) * n + static_cast<std::size_t>(pivot)])) best = row;
        }
        expect(std::abs(normal[static_cast<std::size_t>(best) * n + static_cast<std::size_t>(pivot)]) > 1e-12,
               "dense normal matrix must be nonsingular");
        if (best != pivot) {
            for (std::int32_t column = pivot; column < columns; ++column) {
                std::swap(normal[static_cast<std::size_t>(pivot) * n + static_cast<std::size_t>(column)],
                          normal[static_cast<std::size_t>(best) * n + static_cast<std::size_t>(column)]);
            }
            std::swap(rhs[static_cast<std::size_t>(pivot)], rhs[static_cast<std::size_t>(best)]);
        }
        const double diagonal = normal[static_cast<std::size_t>(pivot) * n + static_cast<std::size_t>(pivot)];
        for (std::int32_t row = pivot + 1; row < columns; ++row) {
            const double multiplier = normal[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(pivot)] / diagonal;
            for (std::int32_t column = pivot; column < columns; ++column) {
                normal[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(column)] -=
                    multiplier * normal[static_cast<std::size_t>(pivot) * n + static_cast<std::size_t>(column)];
            }
            rhs[static_cast<std::size_t>(row)] -= multiplier * rhs[static_cast<std::size_t>(pivot)];
        }
    }
    std::vector<double> solution(n);
    for (std::int32_t row = columns - 1; row >= 0; --row) {
        double value = rhs[static_cast<std::size_t>(row)];
        for (std::int32_t column = row + 1; column < columns; ++column) {
            value -= normal[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(column)]
                * solution[static_cast<std::size_t>(column)];
        }
        solution[static_cast<std::size_t>(row)] =
            value / normal[static_cast<std::size_t>(row) * n + static_cast<std::size_t>(row)];
    }
    return solution;
}

[[nodiscard]] getnative::ConstImageView const_view(const std::vector<float> &pixels,
                                                   std::int32_t width, std::int32_t height) {
    return {pixels.data(), width, height, width};
}

void test_kernel_values_and_support() {
    const std::vector<getnative::Filter> filters{
        getnative::Filter::bilinear(), getnative::Filter::bicubic(1.0 / 3.0, 1.0 / 3.0),
        getnative::Filter::lanczos(4), getnative::Filter::spline16(),
        getnative::Filter::spline36(), getnative::Filter::spline64(),
    };
    for (const auto &filter : filters) {
        expect_close(filter.weight(0.0), oracle_weight(filter, 0.0), 1e-15, "kernel center matches oracle");
        expect_close(filter.weight(0.625), oracle_weight(filter, 0.625), 1e-15, "kernel interior matches oracle");
        expect_close(filter.weight(-1.375), oracle_weight(filter, -1.375), 1e-15, "kernel is symmetric");
        expect_close(filter.weight(static_cast<double>(filter.support())), 0.0, 1e-15, "kernel is zero at support");
    }
    expect_throws([&] { (void)getnative::Filter::lanczos(0).support(); }, "nonpositive Lanczos taps are rejected");
    expect_throws([&] { (void)getnative::Filter::lanczos(16).support(); },
                  "Lanczos taps beyond zimg's supported range are rejected");
}

void test_planner_weights_match_border_and_shift_oracle() {
    std::vector<getnative::Filter> filters{
        getnative::Filter::bilinear(), getnative::Filter::bicubic(),
        getnative::Filter::bicubic(0.25, 0.4), getnative::Filter::spline16(),
        getnative::Filter::spline36(), getnative::Filter::spline64(),
    };
    for (std::int32_t taps = 1; taps <= 8; ++taps) {
        filters.push_back(getnative::Filter::lanczos(taps));
    }
    for (std::size_t filter_index = 0; filter_index < filters.size(); ++filter_index) {
        const auto &filter = filters[filter_index];
        for (const auto border : {getnative::BorderMode::zero, getnative::BorderMode::repeat,
                                  getnative::BorderMode::mirror}) {
            const double shift = filter_index % 2U == 0U ? -0.625 : 0.375;
            const getnative::AxisPlanRequest request{31, 19, 19.375, shift, filter, border};
            const auto expected = dense_forward(request);
            const auto plan = getnative::build_axis_plan(request);
            expect(plan.valid(), "planner returns a valid plan for every supported kernel");
            expect(plan.half_bandwidth == 2 * filter.support() - 1,
                   "planner half-bandwidth follows kernel support");
            expect(plan.forward_width == 2 * filter.support(),
                   "planner forward width follows kernel support");
            for (std::int32_t column = 0; column < request.destination_size; ++column) {
                std::vector<double> actual(static_cast<std::size_t>(request.source_size));
                for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(column)];
                     p < plan.transpose_offsets[static_cast<std::size_t>(column) + 1U]; ++p) {
                    actual[static_cast<std::size_t>(plan.transpose_indices[p])] = plan.transpose_weights[p];
                }
                for (std::int32_t row = 0; row < request.source_size; ++row) {
                    expect_close(actual[static_cast<std::size_t>(row)],
                                 expected[static_cast<std::size_t>(row)
                                          * static_cast<std::size_t>(request.destination_size)
                                          + static_cast<std::size_t>(column)],
                                 2e-7, "planner weights match dense kernel/border/geometry oracle");
                }
            }
        }
    }

    const auto widest_core_plan = getnative::build_axis_plan({
        47, 31, 31.25, -0.125, getnative::Filter::lanczos(15),
        getnative::BorderMode::mirror,
    });
    expect(widest_core_plan.valid(), "Lanczos 15 produces a valid CPU plan");
    expect(widest_core_plan.half_bandwidth == 29
               && widest_core_plan.forward_width == 30,
           "the CPU core retains the full Lanczos 15 plan shape");
}

void test_banded_inverse_matches_dense_reference() {
    const getnative::AxisPlanRequest request{17, 11, 11.4, 0.2,
                                             getnative::Filter::spline36(),
                                             getnative::BorderMode::mirror};
    const auto plan = getnative::build_axis_plan(request);
    std::vector<float> observations(static_cast<std::size_t>(request.source_size));
    for (std::size_t i = 0; i < observations.size(); ++i) {
        observations[i] = static_cast<float>(0.3 * std::sin(0.7 * static_cast<double>(i))
                                             + 0.02 * static_cast<double>(i));
    }
    std::vector<float> banded(static_cast<std::size_t>(request.destination_size));
    getnative::inverse_axis_f32(plan, observations, banded);
    const auto dense = dense_least_squares(dense_forward(request), request.source_size,
                                           request.destination_size, observations);
    for (std::size_t i = 0; i < dense.size(); ++i) {
        expect_close(banded[i], dense[i], 2e-5, "banded planner solve matches dense normal-equation solve");
    }
}

void test_forward_inverse_roundtrip() {
    const getnative::AxisPlanRequest request{23, 15, 15.0, 0.0,
                                             getnative::Filter::bicubic(),
                                             getnative::BorderMode::mirror};
    const auto plan = getnative::build_axis_plan(request);
    std::vector<float> native(static_cast<std::size_t>(request.destination_size));
    for (std::size_t i = 0; i < native.size(); ++i) {
        native[i] = static_cast<float>(std::cos(0.31 * static_cast<double>(i)));
    }
    std::vector<float> scaled(static_cast<std::size_t>(request.source_size));
    std::vector<float> recovered(native.size());
    getnative::forward_axis_f32(plan, native, scaled);
    getnative::inverse_axis_f32(plan, scaled, recovered);
    for (std::size_t i = 0; i < native.size(); ++i) {
        expect_close(recovered[i], native[i], 3e-5, "forward/inverse roundtrip recovers native samples");
    }
}

void test_threshold_is_strict_and_crop_is_applied() {
    std::vector<float> source(25, 0.0F);
    std::vector<float> reconstruction(25, 0.0F);
    reconstruction[6] = 0.5F;
    reconstruction[12] = 1.0F;
    reconstruction[18] = 2.0F;
    const getnative::MetricSpec metric{1, 1, 1, 1, 1.0F, 1U};
    expect_close(getnative::thresholded_p_norm(const_view(source, 5, 5),
                                                const_view(reconstruction, 5, 5), metric),
                 2.0 / 9.0, 1e-12, "threshold equality is excluded and cropped interior is averaged");
}

void test_p_norm_uses_full_cropped_sample_count() {
    std::vector<float> source(4, 0.0F);
    std::vector<float> reconstruction{3.0F, 4.0F, 0.0F, 0.0F};
    const getnative::MetricSpec metric{0, 0, 0, 0, 0.0F, 2U};
    expect_close(getnative::thresholded_p_norm(const_view(source, 2, 2),
                                                const_view(reconstruction, 2, 2), metric),
                 2.5, 1e-12, "p-norm divides by all cropped pixels before taking root");
}

void test_metric_uses_float_moments_and_strict_threshold_boundary() {
    std::vector<float> source(4, 0.0F);
    const float threshold = 0.5F;
    std::vector<float> boundary{
        threshold,
        std::nextafter(threshold, std::numeric_limits<float>::infinity()),
        std::nextafter(threshold, -std::numeric_limits<float>::infinity()),
        0.0F,
    };
    const getnative::MetricSpec l1{0, 0, 0, 0, threshold, 1U};
    expect_close(getnative::thresholded_p_norm(const_view(source, 2, 2),
                                                const_view(boundary, 2, 2), l1),
                 static_cast<double>(boundary[1]) / 4.0, 0.0,
                 "only the next float above threshold contributes");

    std::vector<float> reconstruction{0.1F, 0.2F, 0.3F, 0.4F};
    double sum = 0.0;
    for (const float value : reconstruction) {
        sum += static_cast<double>(value * value * value);
    }
    const double expected = std::pow(sum / 4.0, 1.0 / 3.0);
    const getnative::MetricSpec l3{0, 0, 0, 0, 0.0F, 3U};
    expect_close(getnative::thresholded_p_norm(
                     const_view(source, 2, 2),
                     const_view(reconstruction, 2, 2), l3),
                 expected, 0.0, "p3 accumulates Float32 moments in raster order");
}

void test_high_order_p_norm_is_numerically_stable() {
    std::vector<float> source(4, 0.0F);
    std::vector<float> reconstruction{0.5F, 0.25F, 0.0F, 0.0F};

    for (const std::uint32_t norm : {256U, std::numeric_limits<std::uint32_t>::max()}) {
        const getnative::MetricSpec metric{0, 0, 0, 0, 0.0F, norm};
        const double expected = 0.5 * std::pow(
            (1.0 + std::pow(0.5, static_cast<double>(norm))) / 4.0,
            1.0 / static_cast<double>(norm));
        expect_close(getnative::thresholded_p_norm(
                         const_view(source, 2, 2),
                         const_view(reconstruction, 2, 2), metric),
                     expected, 1e-15,
                     "high-order p-norm remains finite instead of underflowing");
    }
}

void test_batch_matches_scalar_and_preserves_order() {
    constexpr std::int32_t width = 12;
    constexpr std::int32_t height = 10;
    std::vector<float> source(static_cast<std::size_t>(width * height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            source[static_cast<std::size_t>(y * width + x)] =
                static_cast<float>(0.2 * std::sin(0.4 * x) + 0.3 * std::cos(0.3 * y));
        }
    }
    getnative::AxisPlanCache cache;
    std::vector<getnative::CandidateAnalysis> candidates;
    for (const auto &[id, native_width, native_height] :
         {std::tuple{"third", 8, 7}, std::tuple{"first", 9, 6}, std::tuple{"second", 7, 8}}) {
        candidates.push_back({id,
                              cache.get_or_build({width, native_width, static_cast<double>(native_width),
                                                  0.0, getnative::Filter::bicubic(), getnative::BorderMode::mirror}),
                              cache.get_or_build({height, native_height, static_cast<double>(native_height),
                                                  0.0, getnative::Filter::bicubic(), getnative::BorderMode::mirror}),
                              getnative::AnalysisAxes::both});
    }
    candidates.push_back({
        "horizontal-only",
        cache.get_or_build({width, 8, 8.0, 0.125, getnative::Filter::spline16(),
                            getnative::BorderMode::mirror}),
        nullptr,
        getnative::AnalysisAxes::horizontal,
    });
    candidates.push_back({
        "vertical-only",
        nullptr,
        cache.get_or_build({height, 7, 7.0, -0.125, getnative::Filter::spline16(),
                            getnative::BorderMode::mirror}),
        getnative::AnalysisAxes::vertical,
    });
    const getnative::MetricSpec metric{1, 1, 1, 1, 0.001F, 3U};
    std::vector<double> scalar;
    for (const auto &candidate : candidates) {
        getnative::CpuWorkspace workspace;
        if (candidate.axes == getnative::AnalysisAxes::both) {
            scalar.push_back(getnative::analyze_candidate_f32(
                const_view(source, width, height), *candidate.horizontal, *candidate.vertical,
                metric, workspace));
        } else {
            const auto &plan = candidate.axes == getnative::AnalysisAxes::horizontal
                ? candidate.horizontal : candidate.vertical;
            scalar.push_back(getnative::analyze_axis_candidate_f32(
                const_view(source, width, height), *plan, candidate.axes, metric, workspace));
        }
    }
    const auto batch = getnative::analyze_batch_f32(const_view(source, width, height), candidates, metric, 3);
    expect(batch.size() == candidates.size(), "batch returns one result per candidate");
    for (std::size_t i = 0; i < batch.size(); ++i) {
        expect(batch[i].id == candidates[i].id, "batch retains input order");
        expect_close(batch[i].error, scalar[i], 0.0, "batch metric is bit-identical to scalar metric");
    }
}

void test_horizontal_first_fused_metric_matches_full_reconstruction() {
    constexpr std::int32_t width = 12;
    constexpr std::int32_t height = 20;
    const auto horizontal = getnative::build_axis_plan({
        width, 11, 11.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    });
    const auto vertical = getnative::build_axis_plan({
        height, 8, 8.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    });
    expect(getnative::select_forward_order(horizontal, vertical)
               == getnative::ForwardOrder::horizontal_first,
           "anisotropic candidate selects zimg horizontal-first reconstruction");

    std::vector<float> source(static_cast<std::size_t>(width * height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            source[static_cast<std::size_t>(y * width + x)] =
                static_cast<float>(0.17 * std::sin(0.31 * x)
                                   + 0.23 * std::cos(0.19 * y));
        }
    }
    const getnative::MetricSpec metric{1, 2, 2, 1, 0.0001F, 2U};
    getnative::CpuWorkspace fused_workspace;
    const double fused = getnative::analyze_candidate_f32(
        const_view(source, width, height), horizontal, vertical, metric,
        fused_workspace);

    std::vector<float> native(11U * 8U);
    std::vector<float> reconstruction(source.size());
    getnative::CpuWorkspace separate_workspace;
    getnative::descale_2d_f32(
        const_view(source, width, height), horizontal, vertical, separate_workspace,
        {native.data(), 11, 8, 11});
    getnative::reconstruct_2d_f32(
        {native.data(), 11, 8, 11}, horizontal, vertical, separate_workspace,
        {reconstruction.data(), width, height, width});
    const double separate = getnative::thresholded_p_norm(
        const_view(source, width, height),
        const_view(reconstruction, width, height), metric);
    expect_close(fused, separate, 0.0,
                 "horizontal-first fused metric matches full reconstruction exactly");
}

void test_cache_is_singleton_per_key_under_concurrency() {
    getnative::AxisPlanCache cache;
    const getnative::AxisPlanRequest request{64, 43, 43.25, -0.125,
                                             getnative::Filter::lanczos(3),
                                             getnative::BorderMode::repeat};
    std::vector<std::shared_ptr<const getnative::AxisPlan>> plans(32);
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < plans.size(); ++i) {
        threads.emplace_back([&, i] { plans[i] = cache.get_or_build(request); });
    }
    threads.clear();
    expect(cache.size() == 1, "concurrent cache misses publish one key");
    for (const auto &plan : plans) expect(plan.get() == plans.front().get(), "cache returns the same immutable plan");
    cache.clear();
    expect(cache.size() == 0, "cache clear removes all plans");
}

void test_cache_separates_every_plan_parameter() {
    getnative::AxisPlanCache cache;
    const getnative::AxisPlanRequest base{64, 43, 43.25, -0.125,
                                         getnative::Filter::bicubic(0.2, 0.4),
                                         getnative::BorderMode::mirror};
    const auto original = cache.get_or_build(base);
    expect(cache.get_or_build(base).get() == original.get(),
           "identical requests reuse the cached plan");
    const std::vector<getnative::AxisPlanRequest> distinct{
        {65, 43, 43.25, -0.125, base.filter, base.border},
        {64, 42, 43.25, -0.125, base.filter, base.border},
        {64, 43, 43.5, -0.125, base.filter, base.border},
        {64, 43, 43.25, 0.125, base.filter, base.border},
        {64, 43, 43.25, -0.125, getnative::Filter::bicubic(0.3, 0.4), base.border},
        {64, 43, 43.25, -0.125, getnative::Filter::bicubic(0.2, 0.3), base.border},
        {64, 43, 43.25, -0.125, getnative::Filter::lanczos(3), base.border},
        {64, 43, 43.25, -0.125, getnative::Filter::lanczos(4), base.border},
        {64, 43, 43.25, -0.125, base.filter, getnative::BorderMode::repeat},
    };
    for (const auto &request : distinct) {
        expect(cache.get_or_build(request).get() != original.get(),
               "each plan parameter participates in the cache key");
    }
    expect(cache.size() == distinct.size() + 1U,
           "cache contains one entry for every distinct request");
}

void test_invalid_inputs_are_rejected() {
    expect_throws([] { (void)getnative::build_axis_plan({0, 8, 8.0}); }, "zero source size is rejected");
    expect_throws([] { (void)getnative::build_axis_plan({8, 0, 8.0}); }, "zero destination size is rejected");
    expect_throws([] { (void)getnative::build_axis_plan({8, 8, 0.0}); }, "zero active length is rejected");
    for (const double nonfinite : {
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
         }) {
        expect_throws([=] { (void)getnative::build_axis_plan({8, 8, nonfinite}); },
                      "nonfinite active length is rejected");
        expect_throws([=] { (void)getnative::build_axis_plan({8, 8, 8.0, nonfinite}); },
                      "nonfinite shift is rejected");
        expect_throws<std::runtime_error>([=] {
            (void)getnative::build_axis_plan({
                8, 8, 8.0, 0.0, getnative::Filter::bicubic(nonfinite, 0.5),
            });
        }, "nonfinite bicubic B is rejected");
        expect_throws<std::runtime_error>([=] {
            (void)getnative::build_axis_plan({
                8, 8, 8.0, 0.0, getnative::Filter::bicubic(0.0, nonfinite),
            });
        }, "nonfinite bicubic C is rejected");
    }
    expect_throws<std::out_of_range>([] {
        (void)getnative::build_axis_plan({
            8, 8, 8.0, std::numeric_limits<double>::max(),
            getnative::Filter::bicubic(), getnative::BorderMode::mirror,
        });
    }, "finite shift outside the 32-bit pixel grid is rejected");

    std::vector<float> pixels(16);
    const getnative::MetricSpec bad_crop{2, 2, 0, 0, 0.0F, 1U};
    expect_throws([&] { (void)getnative::thresholded_p_norm(const_view(pixels, 4, 4),
                                                            const_view(pixels, 4, 4), bad_crop); },
                  "crop removing the image is rejected");
    const getnative::MetricSpec bad_norm{0, 0, 0, 0, 0.0F, 0U};
    expect_throws([&] { (void)getnative::thresholded_p_norm(const_view(pixels, 4, 4),
                                                            const_view(pixels, 4, 4), bad_norm); },
                  "nonpositive norm is rejected");
    for (const float threshold : {std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()}) {
        const getnative::MetricSpec nonfinite_threshold{0, 0, 0, 0, threshold, 1U};
        expect_throws([&] {
            (void)getnative::thresholded_p_norm(
                const_view(pixels, 4, 4), const_view(pixels, 4, 4), nonfinite_threshold);
        }, "nonfinite metric threshold is rejected");
    }
    expect_throws<std::length_error>(
        [&] { getnative::CpuWorkspace workspace(1); workspace.reserve(4, 4, 3, 3); },
        "workspace limit is enforced");

    const std::vector<getnative::CandidateAnalysis> null_candidate{
        {"null", nullptr, nullptr, getnative::AnalysisAxes::both},
    };
    const getnative::MetricSpec metric{0, 0, 0, 0, 0.0F, 1U};
    expect_throws([&] {
        (void)getnative::analyze_batch_f32(const_view(pixels, 4, 4), null_candidate, metric, 1);
    }, "batch rejects a candidate with missing plans");
}

void test_workspace_is_bounded_reusable_and_row_based() {
    constexpr std::int32_t source_width = 1920;
    constexpr std::int32_t source_height = 1080;
    constexpr std::int32_t native_width = 1280;
    constexpr std::int32_t native_height = 720;
    getnative::CpuWorkspace workspace;
    workspace.reserve(source_width, source_height, native_width, native_height,
                      getnative::AnalysisAxes::both);
    const std::size_t full_source_frame = static_cast<std::size_t>(source_width)
        * static_cast<std::size_t>(source_height);
    expect(workspace.reconstruction_row.size() == static_cast<std::size_t>(source_width),
           "workspace reconstruction storage is one source row");
    const std::size_t inverse_intermediate = static_cast<std::size_t>(native_width)
        * static_cast<std::size_t>(source_height);
    const std::size_t forward_intermediate = static_cast<std::size_t>(source_width)
        * static_cast<std::size_t>(native_height);
    const std::size_t intermediate = std::max(inverse_intermediate, forward_intermediate);
    expect(workspace.intermediate.size() == intermediate,
           "workspace shares storage between inverse and forward intermediates");
    const std::size_t native = static_cast<std::size_t>(native_width)
        * static_cast<std::size_t>(native_height);
    expect(workspace.current_elements() == intermediate + native
                                              + static_cast<std::size_t>(source_width),
           "workspace accounting equals one shared intermediate, native frame, and one row");
    expect(workspace.current_elements() < full_source_frame * 2U,
           "workspace does not add a reconstructed source frame per candidate");
    const std::size_t peak = workspace.peak_elements();
    const std::vector<std::size_t> capacities{
        workspace.intermediate.capacity(), workspace.native.capacity(),
        workspace.reconstruction_row.capacity(),
    };
    workspace.reserve(640, 360, 426, 240, getnative::AnalysisAxes::both);
    expect(workspace.peak_elements() == peak, "smaller reuse does not increase peak accounting");
    workspace.reserve(source_width, source_height, native_width, native_height,
                      getnative::AnalysisAxes::both);
    expect(workspace.intermediate.capacity() == capacities[0]
               && workspace.native.capacity() == capacities[1]
               && workspace.reconstruction_row.capacity() == capacities[2],
           "smaller and same-size reuse do not grow any workspace allocation");
}

} // namespace

int main() {
    try {
        test_kernel_values_and_support();
        test_planner_weights_match_border_and_shift_oracle();
        test_banded_inverse_matches_dense_reference();
        test_forward_inverse_roundtrip();
        test_threshold_is_strict_and_crop_is_applied();
        test_p_norm_uses_full_cropped_sample_count();
        test_metric_uses_float_moments_and_strict_threshold_boundary();
        test_high_order_p_norm_is_numerically_stable();
        test_batch_matches_scalar_and_preserves_order();
        test_horizontal_first_fused_metric_matches_full_reconstruction();
        test_cache_is_singleton_per_key_under_concurrency();
        test_cache_separates_every_plan_parameter();
        test_invalid_inputs_are_rejected();
        test_workspace_is_bounded_reusable_and_row_based();
        std::cout << "all core tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "core test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

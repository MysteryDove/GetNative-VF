#include "getnative/metal_analysis.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

[[nodiscard]] std::vector<float> make_source(std::int32_t width, std::int32_t height) {
    std::vector<float> result(static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            result[static_cast<std::size_t>(y * width + x)] = static_cast<float>(
                0.41 + 0.21 * std::sin(0.071 * static_cast<double>(x))
                + 0.17 * std::cos(0.089 * static_cast<double>(y))
                + 0.09 * std::sin(0.031 * static_cast<double>(x + 2 * y)));
        }
    }
    return result;
}

using NamedFilter = std::pair<std::string_view, getnative::Filter>;

[[nodiscard]] const std::vector<NamedFilter> &wider_filters() {
    static const std::vector<NamedFilter> filters{
        {"spline36", getnative::Filter::spline36()},
        {"lanczos3", getnative::Filter::lanczos(3)},
        {"spline64", getnative::Filter::spline64()},
        {"lanczos4", getnative::Filter::lanczos(4)},
    };
    return filters;
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> make_axis_candidates(
    getnative::AxisPlanCache &cache, std::int32_t width, std::int32_t height,
    getnative::AnalysisAxes axes) {
    const bool horizontal = axes == getnative::AnalysisAxes::horizontal;
    std::vector<getnative::CandidateAnalysis> result;
    const auto &filters = wider_filters();
    result.reserve(filters.size());
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const auto &[name, filter] = filters[index];
        const std::int32_t source_size = horizontal ? width : height;
        const std::int32_t native_size = (horizontal ? 56 : 46)
            + static_cast<std::int32_t>(index);
        auto plan = cache.get_or_build({
            source_size, native_size, static_cast<double>(native_size) + 0.1875,
            horizontal ? 0.125 : -0.0625, filter, getnative::BorderMode::mirror,
        });
        const std::int32_t expected_half = filter.support() * 2 - 1;
        expect(plan->half_bandwidth == expected_half
                   && plan->forward_width == filter.support() * 2,
               "test filter produces the expected fixed Metal shape");
        result.push_back({
            std::string{name} + (horizontal ? "-horizontal" : "-vertical"),
            horizontal ? plan : nullptr,
            horizontal ? nullptr : plan,
            axes,
        });
    }
    return result;
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> make_dual_candidates(
    getnative::AxisPlanCache &cache, std::int32_t width, std::int32_t height,
    getnative::ForwardOrder expected_order) {
    const bool horizontal_first = expected_order == getnative::ForwardOrder::horizontal_first;
    std::vector<getnative::CandidateAnalysis> result;
    const auto &filters = wider_filters();
    result.reserve(filters.size());
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const auto &[name, filter] = filters[index];
        const std::int32_t native_width = (horizontal_first ? 88 : 44)
            + static_cast<std::int32_t>(index);
        const std::int32_t native_height = (horizontal_first ? 38 : 72)
            + static_cast<std::int32_t>(index);
        auto horizontal = cache.get_or_build({
            width, native_width, static_cast<double>(native_width) + 0.25, 0.125,
            filter, getnative::BorderMode::mirror,
        });
        auto vertical = cache.get_or_build({
            height, native_height, static_cast<double>(native_height) + 0.125, -0.0625,
            filter, getnative::BorderMode::mirror,
        });
        expect(getnative::select_forward_order(*horizontal, *vertical) == expected_order,
               "dual-axis specialization test selects its intended forward order");
        result.push_back({
            std::string{name} + (horizontal_first ? "-horizontal-first" : "-vertical-first"),
            std::move(horizontal), std::move(vertical), getnative::AnalysisAxes::both,
        });
    }
    return result;
}

void compare_paths(getnative::ConstImageView source,
                   const std::vector<getnative::CandidateAnalysis> &candidates,
                   const getnative::MetricSpec &metric,
                   getnative::MetalAnalysisEngine &generic,
                   getnative::MetalAnalysisEngine &specialized,
                   std::string_view label) {
    const auto cpu = getnative::analyze_batch_f32(source, candidates, metric);
    const auto generic_results = generic.analyze_axis_batch_f32(source, candidates, metric);
    const auto specialized_results = specialized.analyze_axis_batch_f32(
        source, candidates, metric);
    expect(cpu.size() == candidates.size()
               && generic_results.size() == candidates.size()
               && specialized_results.size() == candidates.size(),
           "all specialization paths return one result per candidate");
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        expect(generic_results[index].id == candidates[index].id
                   && specialized_results[index].id == candidates[index].id,
               "specialization paths retain candidate order");
        if (std::bit_cast<std::uint64_t>(generic_results[index].error)
            != std::bit_cast<std::uint64_t>(specialized_results[index].error)) {
            throw std::runtime_error(std::string{label}
                                     + " specialized and generic results differ by bits");
        }
        const double tolerance = std::max(1e-7, 5e-4 * std::abs(cpu[index].error));
        if (std::abs(specialized_results[index].error - cpu[index].error) > tolerance) {
            throw std::runtime_error(std::string{label}
                                     + " specialized result exceeds CPU tolerance");
        }
    }
    const auto cpu_minimum = static_cast<std::size_t>(
        std::min_element(cpu.begin(), cpu.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.error < rhs.error;
        }) - cpu.begin());
    const auto metal_minimum = static_cast<std::size_t>(
        std::min_element(specialized_results.begin(), specialized_results.end(),
                         [](const auto &lhs, const auto &rhs) {
                             return lhs.error < rhs.error;
                         }) - specialized_results.begin());
    const std::size_t distance = cpu_minimum > metal_minimum
        ? cpu_minimum - metal_minimum : metal_minimum - cpu_minimum;
    expect(distance <= 1, "specialized Metal valley remains within one CPU candidate");
}

void test_wider_fixed_shapes() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto pixels = make_source(width, height);
    const getnative::ConstImageView source{pixels.data(), width, height, width};
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    getnative::MetalAnalysisEngine generic({
        4, 8, 0, false, 32, getnative::MetalKernelDispatchPolicy::generic_only,
    });
    getnative::MetalAnalysisEngine specialized({
        4, 8, 0, false, 32,
        getnative::MetalKernelDispatchPolicy::required_specialized,
    });

    compare_paths(source, make_axis_candidates(
                              cache, width, height, getnative::AnalysisAxes::vertical),
                  metric, generic, specialized, "vertical B11/B15");
    compare_paths(source, make_axis_candidates(
                              cache, width, height, getnative::AnalysisAxes::horizontal),
                  metric, generic, specialized, "horizontal B11/B15");
    compare_paths(source, make_dual_candidates(
                              cache, width, height, getnative::ForwardOrder::vertical_first),
                  metric, generic, specialized, "vertical-first B11/B15");
    compare_paths(source, make_dual_candidates(
                              cache, width, height, getnative::ForwardOrder::horizontal_first),
                  metric, generic, specialized, "horizontal-first B11/B15");

    const auto generic_telemetry = generic.runtime_telemetry();
    const auto specialized_telemetry = specialized.runtime_telemetry();
    expect(generic_telemetry.created_pipeline_names.size() == 5
               && generic_telemetry.generic_tile_count > 0
               && generic_telemetry.specialized_tile_count == 0,
           "generic oracle stays on the five generic pipelines");
    expect(specialized_telemetry.created_pipeline_names.size() == 25
               && specialized_telemetry.specialized_tile_count > 0
               && specialized_telemetry.generic_tile_count == 0,
           "wider fixed shapes create all ten lazy stages without fallback");
    for (const std::string_view name : {
             "inverse_axis_b11", "metric_axis_p1_b11", "inverse_axis_matrix_b11",
             "forward_axis_matrix_b11", "metric_axis_p1_horizontal_first_b11",
             "inverse_axis_b15", "metric_axis_p1_b15", "inverse_axis_matrix_b15",
             "forward_axis_matrix_b15", "metric_axis_p1_horizontal_first_b15"}) {
        expect(std::find(specialized_telemetry.created_pipeline_names.begin(),
                         specialized_telemetry.created_pipeline_names.end(), name)
                   != specialized_telemetry.created_pipeline_names.end(),
               "every wider fixed pipeline stage is reported");
    }
    expect(specialized.peak_working_set_bytes() < 2ULL * 1024ULL * 1024ULL * 1024ULL,
           "wider fixed specialization remains below the absolute memory gate");
}

} // namespace

int main() {
    try {
        if (!getnative::metal_backend_available()) {
            std::cout << "metal kernel specialization tests skipped: no Metal device\n";
            return 0;
        }
        test_wider_fixed_shapes();
        std::cout << "metal kernel specialization tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "metal kernel specialization test failure: " << error.what() << '\n';
        return 1;
    }
}

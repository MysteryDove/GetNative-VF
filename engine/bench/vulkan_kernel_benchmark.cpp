#include "getnative/filter.hpp"
#include "getnative/vulkan_analysis.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Sample {
    double wall_ms = 0.0;
    double gpu_ms = 0.0;
    std::vector<getnative::CandidateResult> results;
    getnative::VulkanRuntimeTelemetry telemetry;
};

[[nodiscard]] double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

void require_exact(
    const std::vector<getnative::CandidateResult> &left,
    const std::vector<getnative::CandidateResult> &right) {
    if (left.size() != right.size()) {
        throw std::runtime_error("Vulkan benchmark result counts differ");
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].id != right[index].id
            || left[index].error != right[index].error) {
            throw std::runtime_error(
                "Vulkan generic and specialized results are not bit-identical");
        }
    }
}

[[nodiscard]] Sample run_once(
    getnative::VulkanAnalysisEngine &engine,
    getnative::ConstImageView source,
    const std::vector<getnative::CandidateAnalysis> &candidates,
    const getnative::MetricSpec &metric) {
    engine.reset_analysis_telemetry();
    const auto start = std::chrono::steady_clock::now();
    Sample sample;
    sample.results = engine.analyze_axis_batch_f32(
        source, candidates, metric, {}, getnative::GpuStageProfile::stages);
    sample.wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    sample.telemetry = engine.runtime_telemetry();
    sample.gpu_ms = sample.telemetry.gpu_execution_ms;
    return sample;
}

void run_regular_scan(bool assert_gate) {
    const auto probe = getnative::vulkan_runtime_probe();
    if (!probe.device_available) {
        std::cout << "SKIP: " << probe.reason << '\n';
        return;
    }
    const std::int32_t device_index =
        getnative::select_default_vulkan_device_index(probe.devices);
    const auto device = std::find_if(
        probe.devices.begin(), probe.devices.end(), [device_index](const auto &value) {
            return value.index == device_index;
        });
    if (device == probe.devices.end()) {
        throw std::runtime_error("Vulkan scan could not resolve the selected device");
    }
    constexpr std::int32_t width = 1920;
    constexpr std::int32_t height = 1080;
    constexpr std::size_t warm_samples = 3U;
    constexpr std::array<std::size_t, 3U> scan_counts{64U, 256U, 1000U};
    std::vector<float> source_storage(
        static_cast<std::size_t>(width * height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            source_storage[static_cast<std::size_t>(y * width + x)] =
                static_cast<float>(
                    0.43 + 0.19 * std::sin(0.017 * x)
                    + 0.13 * std::cos(0.021 * y)
                    + 0.07 * std::sin(0.37 * x + 0.23 * y)
                    + 0.05 * std::cos(0.61 * x - 0.17 * y)
                    + 0.12 * std::sin(0.07 * x + 1.31 * y));
        }
    }
    const getnative::ConstImageView source{
        source_storage.data(), width, height, width};
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};

    std::cout << "regular_scan device="
              << device->name
              << " source=1920x1080 active_height=800.0..899.9"
                 " filter=bicubic-catrom tile_limit=32\n";
    for (const std::size_t candidate_count : scan_counts) {
        std::vector<getnative::AxisPlanRequest> requests;
        requests.reserve(candidate_count);
        for (std::size_t index = 0U; index < candidate_count; ++index) {
            const double position = candidate_count == 1U
                ? 0.0
                : static_cast<double>(index)
                    / static_cast<double>(candidate_count - 1U);
            const double active_height = 800.0 + position * 99.9;
            const std::int32_t native_height = std::clamp(
                static_cast<std::int32_t>(std::floor(active_height)), 800, 899);
            requests.push_back({
                height, native_height, active_height, 0.0,
                getnative::Filter::bicubic(), getnative::BorderMode::mirror});
        }
        getnative::AxisPlanCache cache({
            candidate_count + 1U, 512U * 1024U * 1024U});
        const auto planner_start = std::chrono::steady_clock::now();
        auto planned = cache.get_or_build_batch(requests, 0U);
        const double planner_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - planner_start).count();
        std::vector<getnative::CandidateAnalysis> candidates;
        candidates.reserve(candidate_count);
        for (std::size_t index = 0U; index < candidate_count; ++index) {
            candidates.push_back({
                std::to_string(index), nullptr, planned.plans[index],
                getnative::AnalysisAxes::vertical});
        }

        getnative::VulkanAnalysisOptions options;
        options.device_index = device_index;
        options.execution_slots = 1U;
        getnative::VulkanAnalysisEngine engine(options);
        const Sample cold = run_once(engine, source, candidates, metric);
        std::vector<double> warm_wall;
        std::vector<double> warm_gpu;
        warm_wall.reserve(warm_samples);
        warm_gpu.reserve(warm_samples);
        getnative::VulkanRuntimeTelemetry warm_telemetry;
        for (std::size_t sample = 0U; sample < warm_samples; ++sample) {
            const Sample warm = run_once(engine, source, candidates, metric);
            require_exact(cold.results, warm.results);
            warm_wall.push_back(warm.wall_ms);
            warm_gpu.push_back(warm.gpu_ms);
            warm_telemetry = warm.telemetry;
        }

        double maximum_cpu_error = 0.0;
        double minimum_sample_metric = std::numeric_limits<double>::infinity();
        double maximum_sample_metric = 0.0;
        const std::array<std::size_t, 3U> oracle_indices{
            0U, candidate_count / 2U, candidate_count - 1U};
        for (const std::size_t index : oracle_indices) {
            getnative::CpuWorkspace workspace;
            const double expected = getnative::analyze_axis_candidate_f32(
                source, *candidates[index].vertical,
                getnative::AnalysisAxes::vertical, metric, workspace);
            const double error = std::abs(cold.results[index].error - expected);
            maximum_cpu_error = std::max(maximum_cpu_error, error);
            minimum_sample_metric = std::min(minimum_sample_metric, expected);
            maximum_sample_metric = std::max(maximum_sample_metric, expected);
            const double tolerance = std::max(1e-7, 5e-4 * std::abs(expected));
            if (!std::isfinite(cold.results[index].error) || error > tolerance) {
                throw std::runtime_error(
                    "regular Vulkan scan exceeded the CPU numerical tolerance");
            }
        }

        const double warm_wall_median = median(warm_wall);
        const double warm_gpu_median = median(warm_gpu);
        const double candidates_per_second =
            static_cast<double>(candidate_count) * 1000.0 / warm_gpu_median;
        const std::size_t minimum_tiles =
            (candidate_count + 31U) / 32U;
        std::cout << "candidates=" << candidate_count
                  << " unique_plans=" << planned.unique_key_count
                  << " planner_ms=" << planner_ms
                  << " cold_wall_ms=" << cold.wall_ms
                  << " cold_gpu_ms=" << cold.gpu_ms
                  << " cold_plan_mib="
                  << static_cast<double>(cold.telemetry.plan_upload_bytes)
                        / (1024.0 * 1024.0)
                  << " warm_wall_ms=" << warm_wall_median
                  << " warm_gpu_ms=" << warm_gpu_median
                  << " candidates_per_second=" << candidates_per_second
                  << " tiles=" << warm_telemetry.tile_count
                  << " specialized_dispatches="
                  << warm_telemetry.specialized_inverse_dispatch_count
                  << " peak_workspace_mib="
                  << static_cast<double>(warm_telemetry.peak_workspace_elements
                                          * sizeof(float))
                        / (1024.0 * 1024.0)
                  << " sample_metric_range=" << minimum_sample_metric
                  << ".." << maximum_sample_metric
                  << " max_sample_cpu_error=" << maximum_cpu_error << '\n';

        if (assert_gate
            && (planned.unique_key_count != candidate_count
                || cold.telemetry.plan_cache_miss_count != 1U
                || cold.telemetry.plan_upload_bytes == 0U
                || warm_telemetry.plan_cache_hit_count != 1U
                || warm_telemetry.plan_upload_bytes != 0U
                || warm_telemetry.tile_count < minimum_tiles
                || warm_telemetry.specialized_inverse_dispatch_count
                    != warm_telemetry.tile_count
                || warm_telemetry.generic_inverse_dispatch_count != 0U
                || maximum_sample_metric <= 0.0
                || warm_telemetry.command_buffer_submission_count
                    != warm_telemetry.command_buffer_completion_count)) {
            throw std::runtime_error(
                "regular Vulkan scan failed its multi-block scheduling gate");
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        bool assert_gate = false;
        bool scan_mode = false;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view{argv[index]} == "--assert") {
                assert_gate = true;
            } else if (std::string_view{argv[index]} == "--scan") {
                scan_mode = true;
            } else {
                throw std::invalid_argument(
                    "usage: getnative_vulkan_kernel_benchmark [--scan] [--assert]");
            }
        }

        if (scan_mode) {
            run_regular_scan(assert_gate);
            return 0;
        }

        const auto probe = getnative::vulkan_runtime_probe();
        if (!probe.device_available) {
            std::cout << "SKIP: " << probe.reason << '\n';
            return 0;
        }
        const std::int32_t device_index =
            getnative::select_default_vulkan_device_index(probe.devices);
        constexpr std::int32_t width = 960;
        constexpr std::int32_t height = 540;
        constexpr std::size_t candidate_count = 64U;
        constexpr std::size_t sample_count = 7U;
        std::vector<float> source_storage(
            static_cast<std::size_t>(width * height));
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                source_storage[static_cast<std::size_t>(y * width + x)] =
                    static_cast<float>(
                        0.43 + 0.19 * std::sin(0.017 * x)
                        + 0.13 * std::cos(0.021 * y)
                        + 0.07 * std::sin(0.37 * x + 0.23 * y)
                        + 0.05 * std::cos(0.61 * x - 0.17 * y));
            }
        }
        const getnative::ConstImageView source{
            source_storage.data(), width, height, width};
        const auto plan = std::make_shared<const getnative::AxisPlan>(
            getnative::build_axis_plan({
                width, 720, 720.25, -0.125,
                getnative::Filter::spline64(1.5),
                getnative::BorderMode::mirror}));
        if (plan->half_bandwidth != 11) {
            throw std::runtime_error("Vulkan benchmark fixture did not produce b11");
        }
        std::vector<getnative::CandidateAnalysis> candidates;
        candidates.reserve(candidate_count);
        for (std::size_t index = 0U; index < candidate_count; ++index) {
            candidates.push_back({
                "b11-" + std::to_string(index), plan, nullptr,
                getnative::AnalysisAxes::horizontal});
        }
        const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};

        getnative::VulkanAnalysisOptions generic_options;
        generic_options.device_index = device_index;
        generic_options.execution_slots = 1U;
        generic_options.kernel_dispatch =
            getnative::VulkanKernelDispatchPolicy::generic_only;
        auto specialized_options = generic_options;
        specialized_options.kernel_dispatch =
            getnative::VulkanKernelDispatchPolicy::automatic;
        getnative::VulkanAnalysisEngine generic(generic_options);
        getnative::VulkanAnalysisEngine specialized(specialized_options);

        const Sample generic_warm = run_once(
            generic, source, candidates, metric);
        const Sample specialized_warm = run_once(
            specialized, source, candidates, metric);
        require_exact(generic_warm.results, specialized_warm.results);

        std::vector<double> generic_gpu;
        std::vector<double> specialized_gpu;
        std::vector<double> generic_wall;
        std::vector<double> specialized_wall;
        std::size_t generic_dispatches = 0U;
        std::size_t specialized_dispatches = 0U;
        for (std::size_t sample = 0U; sample < sample_count; ++sample) {
            const bool specialized_first = (sample & 1U) != 0U;
            Sample first = run_once(
                specialized_first ? specialized : generic,
                source, candidates, metric);
            Sample second = run_once(
                specialized_first ? generic : specialized,
                source, candidates, metric);
            const Sample &generic_sample = specialized_first ? second : first;
            const Sample &specialized_sample = specialized_first ? first : second;
            require_exact(generic_sample.results, specialized_sample.results);
            generic_gpu.push_back(generic_sample.gpu_ms);
            specialized_gpu.push_back(specialized_sample.gpu_ms);
            generic_wall.push_back(generic_sample.wall_ms);
            specialized_wall.push_back(specialized_sample.wall_ms);
            generic_dispatches +=
                generic_sample.telemetry.generic_inverse_dispatch_count;
            specialized_dispatches +=
                specialized_sample.telemetry.specialized_inverse_dispatch_count;
        }

        const double generic_gpu_median = median(generic_gpu);
        const double specialized_gpu_median = median(specialized_gpu);
        const double speedup = generic_gpu_median / specialized_gpu_median;
        std::cout << "device=" << generic.device_info().name
                  << " bandwidth=11 candidates=" << candidate_count
                  << " samples=" << sample_count << '\n'
                  << "generic_gpu_median_ms=" << generic_gpu_median
                  << " specialized_gpu_median_ms=" << specialized_gpu_median
                  << " speedup=" << speedup << '\n'
                  << "generic_wall_median_ms=" << median(generic_wall)
                  << " specialized_wall_median_ms=" << median(specialized_wall)
                  << " generic_dispatches=" << generic_dispatches
                  << " specialized_dispatches=" << specialized_dispatches << '\n';

        if (assert_gate
            && (generic_dispatches == 0U || specialized_dispatches == 0U
                || specialized_gpu_median > generic_gpu_median * 1.05)) {
            throw std::runtime_error(
                "Vulkan fixed-bandwidth kernel failed the 5% non-regression gate");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Vulkan kernel benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

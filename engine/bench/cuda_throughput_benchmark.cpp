#include "benchmark_support.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cuda_analysis.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Configuration {
    std::int32_t width = 640;
    std::int32_t height = 360;
    std::int32_t native_width = 426;
    std::int32_t native_height = 240;
    std::size_t candidates = 8U;
    std::size_t warmups = 1U;
    std::size_t samples = 5U;
    std::size_t concurrency = 1U;
    std::size_t workspace_mib = 0U;
    getnative::AnalysisAxes axes = getnative::AnalysisAxes::vertical;
};

[[nodiscard]] getnative::AnalysisAxes parse_axes(std::string_view text) {
    if (text == "horizontal") return getnative::AnalysisAxes::horizontal;
    if (text == "vertical") return getnative::AnalysisAxes::vertical;
    if (text == "both") return getnative::AnalysisAxes::both;
    throw std::invalid_argument("--axes requires horizontal, vertical, or both");
}

[[nodiscard]] std::string_view axes_name(getnative::AnalysisAxes axes) {
    switch (axes) {
    case getnative::AnalysisAxes::horizontal: return "horizontal";
    case getnative::AnalysisAxes::vertical: return "vertical";
    case getnative::AnalysisAxes::both: return "both";
    }
    return "unknown";
}

[[nodiscard]] std::size_t parse_size(std::string_view text,
                                     std::string_view option,
                                     bool allow_zero = false) {
    std::size_t consumed = 0U;
    const auto value = std::stoull(std::string{text}, &consumed);
    if (consumed != text.size() || (!allow_zero && value == 0U)) {
        throw std::invalid_argument(std::string{option} + " requires a "
                                    + (allow_zero ? "non-negative" : "positive")
                                    + " integer");
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::int32_t parse_dimension(std::string_view text,
                                           std::string_view option) {
    const std::size_t value = parse_size(text, option);
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument(std::string{option} + " exceeds int32");
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next = [&](std::string_view option) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string{option} + " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--full") {
            result.width = 1920;
            result.height = 1080;
            result.native_width = 1280;
            result.native_height = 720;
            result.candidates = 64U;
            result.warmups = 3U;
            result.samples = 21U;
        } else if (argument == "--profile") {
            result.width = 1920;
            result.height = 1080;
            result.native_width = 1280;
            result.native_height = 720;
            result.candidates = 8U;
            result.warmups = 0U;
            result.samples = 1U;
        } else if (argument == "--width") {
            result.width = parse_dimension(next(argument), argument);
        } else if (argument == "--height") {
            result.height = parse_dimension(next(argument), argument);
        } else if (argument == "--native-width") {
            result.native_width = parse_dimension(next(argument), argument);
        } else if (argument == "--native-height") {
            result.native_height = parse_dimension(next(argument), argument);
        } else if (argument == "--candidates") {
            result.candidates = parse_size(next(argument), argument);
        } else if (argument == "--warmups") {
            result.warmups = parse_size(next(argument), argument, true);
        } else if (argument == "--samples") {
            result.samples = parse_size(next(argument), argument);
        } else if (argument == "--concurrency") {
            result.concurrency = parse_size(next(argument), argument);
        } else if (argument == "--workspace-mib") {
            result.workspace_mib = parse_size(next(argument), argument);
        } else if (argument == "--axes") {
            result.axes = parse_axes(next(argument));
        } else if (argument == "--help") {
            std::cout
                << "usage: getnative_cuda_throughput_benchmark [--full|--profile] "
                   "[--width N] [--height N] [--native-height N] "
                   "[--native-width N] [--axes horizontal|vertical|both] "
                   "[--candidates N] [--warmups N] [--samples N] "
                   "[--concurrency N] [--workspace-mib N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + std::string{argument});
        }
    }
    if (result.native_width > result.width || result.native_height > result.height) {
        throw std::invalid_argument("native dimensions must not exceed source dimensions");
    }
    if (result.concurrency > 16U) {
        throw std::invalid_argument("concurrency must not exceed 16");
    }
    return result;
}

struct Fixture {
    std::vector<float> pixels;
    getnative::ConstImageView source;
    std::vector<getnative::CandidateAnalysis> candidates;
    getnative::MetricSpec metric;
};

[[nodiscard]] Fixture make_fixture(const Configuration &config) {
    Fixture result;
    const std::size_t area = static_cast<std::size_t>(config.width)
        * static_cast<std::size_t>(config.height);
    result.pixels.resize(area);
    for (std::int32_t y = 0; y < config.height; ++y) {
        for (std::int32_t x = 0; x < config.width; ++x) {
            const double value = 0.43
                + 0.19 * std::sin(0.013 * static_cast<double>(x))
                + 0.17 * std::cos(0.017 * static_cast<double>(y))
                + 0.11 * std::sin(0.007 * static_cast<double>(x + 3 * y));
            result.pixels[static_cast<std::size_t>(y) * config.width
                          + static_cast<std::size_t>(x)] = static_cast<float>(value);
        }
    }
    result.source = {result.pixels.data(), config.width, config.height, config.width};
    result.candidates.reserve(config.candidates);
    for (std::size_t index = 0; index < config.candidates; ++index) {
        const double centered = static_cast<double>(index % 33U) - 16.0;
        const double vertical_active = static_cast<double>(config.native_height)
            + centered / 32.0;
        const double horizontal_active = static_cast<double>(config.native_width)
            - centered / 32.0;
        const double vertical_shift =
            (static_cast<double>((index * 7U) % 17U) - 8.0) / 32.0;
        const double horizontal_shift =
            (static_cast<double>((index * 5U) % 19U) - 9.0) / 32.0;
        std::shared_ptr<const getnative::AxisPlan> horizontal;
        std::shared_ptr<const getnative::AxisPlan> vertical;
        if (config.axes != getnative::AnalysisAxes::vertical) {
            horizontal = std::make_shared<const getnative::AxisPlan>(
                getnative::build_axis_plan({
                    config.width,
                    config.native_width,
                    horizontal_active,
                    horizontal_shift,
                    getnative::Filter::lanczos(3),
                    getnative::BorderMode::mirror,
                }));
        }
        if (config.axes != getnative::AnalysisAxes::horizontal) {
            vertical = std::make_shared<const getnative::AxisPlan>(
                getnative::build_axis_plan({
                config.height,
                config.native_height,
                vertical_active,
                vertical_shift,
                getnative::Filter::lanczos(3),
                getnative::BorderMode::mirror,
            }));
        }
        result.candidates.push_back({
            std::to_string(index), std::move(horizontal), std::move(vertical),
            config.axes,
        });
    }
    result.metric = {8, 8, 8, 8, 0.0F, 1U};
    if (config.width <= 16 || config.height <= 16) result.metric = {};
    return result;
}

struct WaveResult {
    double milliseconds = 0.0;
    double checksum = 0.0;
};

[[nodiscard]] WaveResult run_wave(
    getnative::CudaAnalysisEngine &engine, const Fixture &fixture,
    std::size_t concurrency) {
    if (concurrency == 1U) {
        const auto start = std::chrono::steady_clock::now();
        const auto results = engine.analyze_axis_batch_f32(
            fixture.source, fixture.candidates, fixture.metric);
        const auto end = std::chrono::steady_clock::now();
        double checksum = 0.0;
        for (const auto &result : results) checksum += result.error;
        return {
            std::chrono::duration<double, std::milli>(end - start).count(),
            checksum,
        };
    }

    std::barrier start_gate(static_cast<std::ptrdiff_t>(concurrency + 1U));
    std::vector<double> checksums(concurrency, 0.0);
    std::vector<std::exception_ptr> failures(concurrency);
    std::vector<std::jthread> workers;
    workers.reserve(concurrency);
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            start_gate.arrive_and_wait();
            try {
                const auto results = engine.analyze_axis_batch_f32(
                    fixture.source, fixture.candidates, fixture.metric);
                for (const auto &result : results) checksums[worker] += result.error;
            } catch (...) {
                failures[worker] = std::current_exception();
            }
        });
    }
    const auto start = std::chrono::steady_clock::now();
    start_gate.arrive_and_wait();
    workers.clear();
    const auto end = std::chrono::steady_clock::now();
    for (const auto &failure : failures) {
        if (failure) std::rethrow_exception(failure);
    }
    double checksum = 0.0;
    for (const double value : checksums) checksum += value;
    return {
        std::chrono::duration<double, std::milli>(end - start).count(),
        checksum,
    };
}

[[nodiscard]] double percentile95(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("p95 requires samples");
    std::sort(values.begin(), values.end());
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(values.size())));
    return values[std::max<std::size_t>(1U, rank) - 1U];
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Configuration config = parse_arguments(argc, argv);
        const Fixture fixture = make_fixture(config);
        getnative::CudaAnalysisOptions options;
        const getnative::CudaRuntimeProbe probe = getnative::cuda_runtime_probe();
        const auto selected_device = std::find_if(
            probe.devices.begin(), probe.devices.end(),
            [](const getnative::CudaDeviceInfo &device) {
                return device.backend_compatible;
            });
        if (!probe.device_available || selected_device == probe.devices.end()) {
            throw std::runtime_error(probe.reason.empty()
                ? "no compatible CUDA device is available" : probe.reason);
        }
        options.device_ordinal = selected_device->ordinal;
        if (config.workspace_mib != 0U) {
            if (config.workspace_mib
                > std::numeric_limits<std::size_t>::max() / (1024U * 1024U)) {
                throw std::invalid_argument("workspace MiB value overflows size_t");
            }
            options.workspace_limit_elements = config.workspace_mib
                * 1024U * 1024U / sizeof(float);
        }
        options.execution_slots = std::max<std::size_t>(2U, config.concurrency);
        getnative::CudaAnalysisEngine engine(options);

        double reference_checksum = 0.0;
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            const WaveResult result = run_wave(engine, fixture, config.concurrency);
            reference_checksum = result.checksum;
        }
        engine.reset_analysis_telemetry();

        std::vector<double> wave_samples;
        wave_samples.reserve(config.samples);
        double checksum = 0.0;
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            const WaveResult result = run_wave(engine, fixture, config.concurrency);
            wave_samples.push_back(result.milliseconds);
            checksum += result.checksum;
            if (config.warmups != 0U && result.checksum != reference_checksum) {
                throw std::runtime_error("CUDA benchmark output changed after warmup");
            }
        }

        const auto summary = getnative::benchmark::summarize(wave_samples);
        const double p95_wave_ms = percentile95(wave_samples);
        const double frame_ms = summary.median / static_cast<double>(config.concurrency);
        const double p95_frame_ms = p95_wave_ms / static_cast<double>(config.concurrency);
        const double frames_per_second = 1000.0 / frame_ms;
        const double candidates_per_second = frames_per_second
            * static_cast<double>(config.candidates);
        const auto telemetry = engine.runtime_telemetry();

        std::cout << '{';
        getnative::benchmark::append_common_metadata(
            std::cout, "getnative_cuda_throughput_benchmark",
            std::string{"cuda-"} + std::string{axes_name(config.axes)},
            "synthetic-trigonometric-v1", argc, argv);
        std::cout << ",\"workload\":{\"width\":" << config.width
                  << ",\"height\":" << config.height
                  << ",\"native_width\":" << config.native_width
                  << ",\"native_height\":" << config.native_height
                  << ",\"axes\":"
                  << getnative::benchmark::json_string(axes_name(config.axes))
                  << ",\"candidates\":" << config.candidates
                  << ",\"warmups\":" << config.warmups
                  << ",\"samples\":" << config.samples
                  << ",\"concurrency\":" << config.concurrency
                  << ",\"workspace_mib\":" << config.workspace_mib << '}'
                  << ",\"device\":{\"name\":"
                  << getnative::benchmark::json_string(engine.device_info().name)
                  << ",\"compute_capability\":\""
                  << engine.device_info().compute_capability_major << '.'
                  << engine.device_info().compute_capability_minor << "\"}"
                  << ",\"wave_ms\":";
        getnative::benchmark::append_summary(std::cout, summary);
        std::cout << ",\"p95_wave_ms\":" << std::setprecision(17) << p95_wave_ms
                  << ",\"frame_ms\":" << frame_ms
                  << ",\"p95_frame_ms\":" << p95_frame_ms
                  << ",\"fps\":" << frames_per_second
                  << ",\"candidates_per_second\":" << candidates_per_second
                  << ",\"checksum\":" << checksum
                  << ",\"memory\":{\"peak_workspace_elements\":"
                  << engine.peak_workspace_elements()
                  << ",\"peak_working_set_bytes\":"
                  << engine.peak_working_set_bytes() << '}'
                  << ",\"telemetry\":{\"kernel_launch_count\":"
                  << telemetry.kernel_launch_count
                  << ",\"analyzed_candidate_count\":"
                  << telemetry.analyzed_candidate_count
                  << ",\"source_upload_bytes\":" << telemetry.source_upload_bytes
                  << ",\"plan_upload_bytes\":" << telemetry.plan_upload_bytes
                  << ",\"result_readback_bytes\":" << telemetry.result_readback_bytes
                  << ",\"workspace_bytes\":" << telemetry.workspace_bytes
                  << ",\"initial_device_free_bytes\":"
                  << telemetry.initial_device_free_bytes
                  << ",\"device_memory_reserve_bytes\":"
                  << telemetry.device_memory_reserve_bytes
                  << ",\"device_memory_budget_bytes\":"
                  << telemetry.device_memory_budget_bytes
                  << ",\"per_slot_memory_budget_bytes\":"
                  << telemetry.per_slot_memory_budget_bytes
                  << ",\"effective_workspace_limit_bytes\":"
                  << telemetry.effective_workspace_limit_bytes
                  << ",\"workspace_limit_clamped\":"
                  << (telemetry.workspace_limit_clamped ? "true" : "false")
                  << ",\"pinned_staging_bytes\":"
                  << telemetry.pinned_staging_bytes
                  << ",\"tile_count\":" << telemetry.tile_count
                  << ",\"buffer_allocation_count\":"
                  << telemetry.buffer_allocation_count
                  << ",\"plan_cache_hits\":" << telemetry.plan_cache_hits
                  << ",\"plan_cache_misses\":" << telemetry.plan_cache_misses
                  << ",\"execution_slot_wait_ms\":"
                  << telemetry.execution_slot_wait_ms
                  << ",\"host_pack_ms\":" << telemetry.host_pack_ms
                  << ",\"source_staging_ms\":"
                  << telemetry.source_staging_ms
                  << ",\"source_upload_ms\":" << telemetry.source_upload_ms
                  << ",\"plan_upload_ms\":" << telemetry.plan_upload_ms
                  << ",\"source_transpose_ms\":"
                  << telemetry.source_transpose_ms
                  << ",\"horizontal_fused_ms\":"
                  << telemetry.horizontal_fused_ms
                  << ",\"inverse_horizontal_ms\":"
                  << telemetry.inverse_horizontal_ms
                  << ",\"inverse_vertical_ms\":"
                  << telemetry.inverse_vertical_ms
                  << ",\"forward_intermediate_ms\":"
                  << telemetry.forward_intermediate_ms
                  << ",\"metric_ms\":" << telemetry.metric_ms
                  << ",\"kernel_ms\":" << telemetry.kernel_ms
                  << ",\"result_readback_ms\":" << telemetry.result_readback_ms
                  << ",\"gpu_total_ms\":" << telemetry.gpu_total_ms
                  << ",\"artifact_stage\":"
                  << getnative::benchmark::json_string(telemetry.artifact_stage)
                  << ",\"artifact_target\":"
                  << getnative::benchmark::json_string(telemetry.artifact_target)
                  << ",\"artifact_hash_fnv1a64\":"
                  << getnative::benchmark::json_string(
                         telemetry.artifact_hash_fnv1a64)
                  << "}}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CUDA throughput benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

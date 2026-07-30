#include "benchmark_support.hpp"

#include "getnative/metal_analysis.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Configuration {
    std::size_t candidates = 64;
    std::size_t tile_size = 32;
    std::size_t reduction_groups = 8;
    std::size_t inverse_threads = 32;
    std::size_t samples = 1;
    std::int32_t width = 640;
    std::int32_t height = 360;
    std::int32_t native_height = 240;
    std::string kernel = "bicubic";
    bool assert_correctness = false;
    bool profile_split_kernels = false;
    std::optional<std::filesystem::path> json_output;
};

struct BenchmarkFixture {
    std::vector<float> source;
    std::vector<std::string> candidate_ids;
    std::vector<getnative::AxisPlanRequest> requests;
};

struct Sample {
    double plan_ms = 0.0;
    double cpu_ms = 0.0;
    double metal_ms = 0.0;
    double cpu_total_ms = 0.0;
    double metal_total_ms = 0.0;
    double plan_share = 0.0;
    double maximum_error = 0.0;
    double valley_distance = 0.0;
    bool within_tolerance = true;
    std::size_t cache_size = 0;
};

struct Measurements {
    getnative::benchmark::Summary plan_ms;
    getnative::benchmark::Summary cpu_ms;
    getnative::benchmark::Summary metal_ms;
    getnative::benchmark::Summary cpu_total_ms;
    getnative::benchmark::Summary metal_total_ms;
    getnative::benchmark::Summary plan_share;
    getnative::benchmark::Summary maximum_error;
    getnative::benchmark::Summary valley_distance;
    bool within_tolerance = true;
    std::size_t cache_size = 0;
};

[[nodiscard]] std::size_t parse_size(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0U) {
        throw std::invalid_argument("numeric option must be a positive integer");
    }
    return value;
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--assert") {
            result.assert_correctness = true;
        } else if (argument == "--profile-split-kernels") {
            result.profile_split_kernels = true;
        } else if (argument == "--full") {
            result.candidates = 1000;
            result.width = 1920;
            result.height = 1080;
            result.native_height = 720;
        } else if (argument == "--candidates" && index + 1 < argc) {
            result.candidates = parse_size(argv[++index]);
        } else if (argument == "--tile-size" && index + 1 < argc) {
            result.tile_size = parse_size(argv[++index]);
        } else if (argument == "--reduction-groups" && index + 1 < argc) {
            result.reduction_groups = parse_size(argv[++index]);
        } else if (argument == "--inverse-threads" && index + 1 < argc) {
            result.inverse_threads = parse_size(argv[++index]);
        } else if (argument == "--kernel" && index + 1 < argc) {
            result.kernel = argv[++index];
        } else if (argument == "--samples" && index + 1 < argc) {
            result.samples = parse_size(argv[++index]);
        } else if (argument == "--planner-mode" && index + 1 < argc) {
            const std::string_view mode{argv[++index]};
            if (mode != "serial") {
                throw std::invalid_argument("Stage 0 supports only --planner-mode serial");
            }
        } else if (argument == "--json-out" && index + 1 < argc) {
            result.json_output = std::filesystem::path{argv[++index]};
        } else {
            throw std::invalid_argument(
                "usage: getnative_metal_benchmark [--full] [--candidates N] "
                "[--tile-size N] [--reduction-groups N] [--inverse-threads N] "
                "[--samples N] [--planner-mode serial] [--json-out PATH] [--assert] "
                "[--profile-split-kernels] "
                "[--kernel bilinear|bicubic|spline36|spline64|lanczos3|lanczos8]");
        }
    }
    return result;
}

[[nodiscard]] getnative::Filter benchmark_filter(std::string_view name) {
    if (name == "bilinear") return getnative::Filter::bilinear();
    if (name == "bicubic") return getnative::Filter::bicubic();
    if (name == "spline36") return getnative::Filter::spline36();
    if (name == "spline64") return getnative::Filter::spline64();
    if (name == "lanczos3") return getnative::Filter::lanczos(3);
    if (name == "lanczos8") return getnative::Filter::lanczos(8);
    throw std::invalid_argument(
        "kernel must be bilinear, bicubic, spline36, spline64, lanczos3, or lanczos8");
}

[[nodiscard]] BenchmarkFixture make_fixture(
    const Configuration &config,
    const getnative::Filter &filter) {
    BenchmarkFixture result;
    result.source.resize(
        static_cast<std::size_t>(config.width) * static_cast<std::size_t>(config.height));
    for (std::size_t index = 0; index < result.source.size(); ++index) {
        result.source[index] = static_cast<float>(
            ((index * 131U + (index >> 3U) * 17U) % 1024U) / 1023.0);
    }
    result.candidate_ids.reserve(config.candidates);
    result.requests.reserve(config.candidates);
    for (std::size_t index = 0; index < config.candidates; ++index) {
        result.candidate_ids.push_back(std::to_string(index));
        const double active = static_cast<double>(config.native_height)
            + static_cast<double>(index) / static_cast<double>(config.candidates + 1U);
        result.requests.push_back({
            config.height, config.native_height, active, 0.0, filter,
            getnative::BorderMode::mirror,
        });
    }
    return result;
}

[[nodiscard]] Sample run_sample(
    const Configuration &config,
    const BenchmarkFixture &fixture,
    getnative::MetalAnalysisEngine &metal,
    const getnative::MetricSpec &metric) {
    const getnative::ConstImageView view{
        fixture.source.data(), config.width, config.height, config.width,
    };
    getnative::AxisPlanCache cache;
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(fixture.requests.size());

    const auto plan_start = Clock::now();
    for (std::size_t index = 0; index < fixture.requests.size(); ++index) {
        candidates.push_back({
            fixture.candidate_ids[index],
            nullptr,
            cache.get_or_build(fixture.requests[index]),
            getnative::AnalysisAxes::vertical,
        });
    }
    const auto plan_elapsed = Clock::now() - plan_start;

    const auto cpu_start = Clock::now();
    const auto cpu = getnative::analyze_batch_f32(view, candidates, metric);
    const auto cpu_elapsed = Clock::now() - cpu_start;
    const auto metal_start = Clock::now();
    const auto gpu = metal.analyze_axis_batch_f32(view, candidates, metric);
    const auto metal_elapsed = Clock::now() - metal_start;

    double maximum_error = 0.0;
    bool within_tolerance = true;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const double error = std::abs(cpu[index].error - gpu[index].error);
        maximum_error = std::max(maximum_error, error);
        const double tolerance = std::max(1e-7, 5e-4 * std::abs(cpu[index].error));
        within_tolerance = within_tolerance && error <= tolerance;
    }
    const auto cpu_minimum = static_cast<std::size_t>(
        std::min_element(cpu.begin(), cpu.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.error < rhs.error;
        }) - cpu.begin());
    const auto gpu_minimum = static_cast<std::size_t>(
        std::min_element(gpu.begin(), gpu.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.error < rhs.error;
        }) - gpu.begin());
    const std::size_t valley_distance = cpu_minimum > gpu_minimum
        ? cpu_minimum - gpu_minimum : gpu_minimum - cpu_minimum;

    const double plan_ms = std::chrono::duration<double, std::milli>(plan_elapsed).count();
    const double cpu_ms = std::chrono::duration<double, std::milli>(cpu_elapsed).count();
    const double metal_ms = std::chrono::duration<double, std::milli>(metal_elapsed).count();
    const double metal_total_ms = plan_ms + metal_ms;
    return {
        plan_ms,
        cpu_ms,
        metal_ms,
        plan_ms + cpu_ms,
        metal_total_ms,
        plan_ms / metal_total_ms,
        maximum_error,
        static_cast<double>(valley_distance),
        within_tolerance,
        cache.size(),
    };
}

[[nodiscard]] Measurements measure(
    const Configuration &config,
    const BenchmarkFixture &fixture,
    getnative::MetalAnalysisEngine &metal,
    const getnative::MetricSpec &metric) {
    const Sample warmup = run_sample(config, fixture, metal, metric);
    std::vector<double> plan_samples;
    std::vector<double> cpu_samples;
    std::vector<double> metal_samples;
    std::vector<double> cpu_total_samples;
    std::vector<double> metal_total_samples;
    std::vector<double> share_samples;
    std::vector<double> error_samples;
    std::vector<double> valley_samples;
    for (auto *samples : {&plan_samples, &cpu_samples, &metal_samples, &cpu_total_samples,
                          &metal_total_samples, &share_samples, &error_samples, &valley_samples}) {
        samples->reserve(config.samples);
    }

    bool within_tolerance = warmup.within_tolerance;
    for (std::size_t index = 0; index < config.samples; ++index) {
        const Sample sample = run_sample(config, fixture, metal, metric);
        if (sample.cache_size != fixture.requests.size()) {
            throw std::runtime_error("Metal staged benchmark cache cardinality changed");
        }
        plan_samples.push_back(sample.plan_ms);
        cpu_samples.push_back(sample.cpu_ms);
        metal_samples.push_back(sample.metal_ms);
        cpu_total_samples.push_back(sample.cpu_total_ms);
        metal_total_samples.push_back(sample.metal_total_ms);
        share_samples.push_back(sample.plan_share);
        error_samples.push_back(sample.maximum_error);
        valley_samples.push_back(sample.valley_distance);
        within_tolerance = within_tolerance && sample.within_tolerance;
    }

    return {
        getnative::benchmark::summarize(std::move(plan_samples)),
        getnative::benchmark::summarize(std::move(cpu_samples)),
        getnative::benchmark::summarize(std::move(metal_samples)),
        getnative::benchmark::summarize(std::move(cpu_total_samples)),
        getnative::benchmark::summarize(std::move(metal_total_samples)),
        getnative::benchmark::summarize(std::move(share_samples)),
        getnative::benchmark::summarize(std::move(error_samples)),
        getnative::benchmark::summarize(std::move(valley_samples)),
        within_tolerance,
        warmup.cache_size,
    };
}

[[nodiscard]] std::string stage0_outcome(
    const Configuration &config,
    const Measurements &measurements,
    bool assertions_pass) {
    if (config.samples != 21U) return "NOT_EVALUATED";
    if (!assertions_pass) return "STAGE0_BLOCKED";
    return measurements.plan_share.median >= 0.10
        ? "PROCEED_STAGE1" : "STOP_AND_REDIRECT_GPU";
}

[[nodiscard]] std::string make_json(
    const Configuration &config,
    const Measurements &measurements,
    const getnative::MetalAnalysisEngine &metal,
    std::size_t workspace_bytes,
    std::size_t working_set_bytes,
    bool assertions_pass,
    int argc,
    char **argv) {
    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_metal_benchmark",
        "synthetic-lcg-vertical-active-scan-v1", argc, argv);
    output << ",\"sample_count\":" << config.samples
           << ",\"warmup_count\":1"
           << ",\"stage0_outcome\":"
           << getnative::benchmark::json_string(
                  stage0_outcome(config, measurements, assertions_pass))
           << ",\"device\":" << getnative::benchmark::json_string(metal.device_info().name)
           << ",\"fixture\":{\"width\":" << config.width
           << ",\"height\":" << config.height
           << ",\"candidates\":" << config.candidates
           << ",\"native_height\":" << config.native_height
           << ",\"kernel\":" << getnative::benchmark::json_string(config.kernel) << '}'
           << ",\"configuration\":{\"tile_size\":" << config.tile_size
           << ",\"reduction_groups\":" << config.reduction_groups
           << ",\"inverse_threads\":" << config.inverse_threads
           << ",\"profile_split_kernels\":"
           << (config.profile_split_kernels ? "true" : "false") << '}'
           << ",\"metrics\":{\"plan_ms\":";
    getnative::benchmark::append_summary(output, measurements.plan_ms);
    output << ",\"cpu_ms\":";
    getnative::benchmark::append_summary(output, measurements.cpu_ms);
    output << ",\"metal_ms\":";
    getnative::benchmark::append_summary(output, measurements.metal_ms);
    output << ",\"cpu_total_ms\":";
    getnative::benchmark::append_summary(output, measurements.cpu_total_ms);
    output << ",\"metal_total_ms\":";
    getnative::benchmark::append_summary(output, measurements.metal_total_ms);
    output << ",\"plan_share\":";
    getnative::benchmark::append_summary(output, measurements.plan_share);
    output << "}"
           << ",\"correctness\":{\"maximum_metric_error\":";
    getnative::benchmark::append_summary(output, measurements.maximum_error);
    output << ",\"valley_step_distance\":";
    getnative::benchmark::append_summary(output, measurements.valley_distance);
    output << ",\"strict_tolerance\":"
           << (measurements.within_tolerance ? "true" : "false")
           << ",\"cache_size\":" << measurements.cache_size
           << ",\"metal_peak_workspace_bytes\":" << workspace_bytes
           << ",\"metal_peak_working_set_bytes\":" << working_set_bytes
           << ",\"assertions\":" << (assertions_pass ? "true" : "false")
           << "}}\n";
    return output.str();
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Configuration config = parse_arguments(argc, argv);
        if (config.json_output) {
            getnative::benchmark::validate_json_output_path(*config.json_output);
        }
        const getnative::Filter filter = benchmark_filter(config.kernel);
        if (!getnative::metal_backend_available()) {
            throw std::runtime_error("no Metal device is available");
        }
        const BenchmarkFixture fixture = make_fixture(config, filter);
        const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
        getnative::MetalAnalysisEngine metal({
            config.tile_size, config.reduction_groups, 0, config.profile_split_kernels,
            config.inverse_threads,
        });
        const Measurements measurements = measure(config, fixture, metal, metric);
        const std::size_t workspace_bytes = metal.peak_workspace_elements() * sizeof(float);
        const std::size_t working_set_bytes = metal.peak_working_set_bytes();
        const bool assertions_pass = measurements.within_tolerance
            && measurements.valley_distance.maximum <= 1.0
            && workspace_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL
            && working_set_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL;
        const double speedup = measurements.cpu_ms.median / measurements.metal_ms.median;

        std::cout << std::fixed << std::setprecision(3)
                  << "device=" << metal.device_info().name << '\n'
                  << "case=vertical-" << config.kernel << "-p1 source="
                  << config.width << 'x' << config.height
                  << " candidates=" << config.candidates << " native_height="
                  << config.native_height << '\n'
                  << "tile_size=" << config.tile_size
                  << " reduction_groups=" << config.reduction_groups
                  << " inverse_threads=" << config.inverse_threads << '\n'
                  << "planner_mode=serial\n"
                  << "samples=" << config.samples << '\n'
                  << "plan_ms=" << measurements.plan_ms.median << '\n'
                  << "plan_mad_ms=" << measurements.plan_ms.mad << '\n'
                  << "cpu_ms=" << measurements.cpu_ms.median << '\n'
                  << "metal_ms=" << measurements.metal_ms.median << '\n'
                  << "cpu_total_ms=" << measurements.cpu_total_ms.median << '\n'
                  << "metal_total_ms=" << measurements.metal_total_ms.median << '\n'
                  << "plan_share=" << measurements.plan_share.median << '\n'
                  << "plan_share_mad=" << measurements.plan_share.mad << '\n'
                  << "stage0_outcome="
                  << stage0_outcome(config, measurements, assertions_pass) << '\n'
                  << "speedup=" << speedup << "x\n"
                  << "maximum_metric_error=" << std::setprecision(10)
                  << measurements.maximum_error.maximum << '\n'
                  << "valley_step_distance=" << std::setprecision(0)
                  << measurements.valley_distance.maximum << '\n'
                  << "metal_peak_workspace_mib=" << std::setprecision(3)
                  << static_cast<double>(workspace_bytes) / (1024.0 * 1024.0) << '\n'
                  << "metal_peak_working_set_mib="
                  << static_cast<double>(working_set_bytes) / (1024.0 * 1024.0) << '\n'
                  << "profile_split_kernels="
                  << (config.profile_split_kernels ? "true" : "false") << '\n'
                  << "strict_tolerance="
                  << (measurements.within_tolerance ? "pass" : "fail") << '\n'
                  << "default_enable_gate="
                  << (speedup >= 3.0 && assertions_pass ? "pass" : "fail") << '\n';

        if (config.json_output) {
            getnative::benchmark::atomic_write_json(
                *config.json_output,
                make_json(config, measurements, metal, workspace_bytes,
                          working_set_bytes, assertions_pass, argc, argv));
        }
        if (config.assert_correctness && !assertions_pass) {
            std::cerr << "Metal benchmark correctness or memory assertion failed\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "Metal benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

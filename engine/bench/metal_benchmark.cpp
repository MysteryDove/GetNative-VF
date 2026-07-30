#include "getnative/metal_analysis.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
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
    std::int32_t width = 640;
    std::int32_t height = 360;
    std::int32_t native_height = 240;
    std::string kernel = "bicubic";
    bool assert_correctness = false;
    bool profile_split_kernels = false;
};

[[nodiscard]] std::size_t parse_size(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
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
        } else {
            throw std::invalid_argument(
                "usage: getnative_metal_benchmark [--full] [--candidates N] "
                "[--tile-size N] [--reduction-groups N] [--inverse-threads N] [--assert] "
                "[--profile-split-kernels] "
                "[--kernel bilinear|bicubic|spline36|spline64|lanczos3|lanczos8]");
        }
    }
    return result;
}

[[nodiscard]] std::vector<float> make_source(std::int32_t width, std::int32_t height) {
    std::vector<float> source(static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height));
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<float>(
            ((index * 131U + (index >> 3U) * 17U) % 1024U) / 1023.0);
    }
    return source;
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

} // namespace

int main(int argc, char **argv) {
    try {
        const Configuration config = parse_arguments(argc, argv);
        const getnative::Filter filter = benchmark_filter(config.kernel);
        if (!getnative::metal_backend_available()) {
            throw std::runtime_error("no Metal device is available");
        }
        const auto source = make_source(config.width, config.height);
        const getnative::ConstImageView view{
            source.data(), config.width, config.height, config.width,
        };
        const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
        getnative::AxisPlanCache cache;
        std::vector<getnative::CandidateAnalysis> candidates;
        candidates.reserve(config.candidates);
        for (std::size_t index = 0; index < config.candidates; ++index) {
            const double active = static_cast<double>(config.native_height)
                + static_cast<double>(index) / static_cast<double>(config.candidates + 1U);
            candidates.push_back({
                std::to_string(index), nullptr,
                cache.get_or_build({config.height, config.native_height, active, 0.0,
                                    filter,
                                    getnative::BorderMode::mirror}),
                getnative::AnalysisAxes::vertical,
            });
        }

        getnative::MetalAnalysisEngine metal({
            config.tile_size, config.reduction_groups, 0, config.profile_split_kernels,
            config.inverse_threads,
        });
        const std::size_t warmup_count = std::min<std::size_t>(8, candidates.size());
        (void)metal.analyze_axis_batch_f32(
            view, std::span<const getnative::CandidateAnalysis>{candidates.data(), warmup_count},
            metric);

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
        const double cpu_ms = std::chrono::duration<double, std::milli>(cpu_elapsed).count();
        const double metal_ms = std::chrono::duration<double, std::milli>(metal_elapsed).count();
        const double speedup = cpu_ms / metal_ms;
        const std::size_t workspace_bytes = metal.peak_workspace_elements() * sizeof(float);
        const std::size_t working_set_bytes = metal.peak_working_set_bytes();

        std::cout << std::fixed << std::setprecision(3)
                  << "device=" << metal.device_info().name << '\n'
                  << "case=vertical-" << config.kernel << "-p1 source="
                  << config.width << 'x' << config.height
                  << " candidates=" << config.candidates << " native_height="
                  << config.native_height << '\n'
                  << "tile_size=" << config.tile_size
                  << " reduction_groups=" << config.reduction_groups
                  << " inverse_threads=" << config.inverse_threads << '\n'
                  << "cpu_ms=" << cpu_ms << '\n'
                  << "metal_ms=" << metal_ms << '\n'
                  << "speedup=" << speedup << "x\n"
                  << "maximum_metric_error=" << std::setprecision(10) << maximum_error << '\n'
                  << "valley_step_distance=" << valley_distance << '\n'
                  << "metal_peak_workspace_mib=" << std::setprecision(3)
                  << static_cast<double>(workspace_bytes) / (1024.0 * 1024.0) << '\n'
                  << "metal_peak_working_set_mib="
                  << static_cast<double>(working_set_bytes) / (1024.0 * 1024.0) << '\n'
                  << "profile_split_kernels="
                  << (config.profile_split_kernels ? "true" : "false") << '\n'
                  << "strict_tolerance=" << (within_tolerance ? "pass" : "fail") << '\n'
                  << "default_enable_gate=" << (speedup >= 3.0 && within_tolerance
                                                      && valley_distance <= 1
                                                  ? "pass" : "fail")
                  << '\n';

        if (config.assert_correctness
            && (!within_tolerance || valley_distance > 1
                || workspace_bytes >= 2ULL * 1024ULL * 1024ULL * 1024ULL
                || working_set_bytes >= 2ULL * 1024ULL * 1024ULL * 1024ULL)) {
            std::cerr << "Metal benchmark correctness or memory assertion failed\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "Metal benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

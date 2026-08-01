#include "benchmark_support.hpp"

#include "getnative/cpu_features.hpp"
#include "getnative/filter.hpp"
#include "getnative/vulkan_analysis.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Configuration {
    std::size_t candidates = 1000;
    std::size_t tile_size = 32;
    std::size_t reduction_groups = 8;
    std::size_t inverse_threads = 32;
    std::size_t samples = 21;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    std::int32_t native_height = 810;
    std::string kernel = "bicubic-catrom";
    bool assert_gates = false;
    bool enable_validation = false;
    bool generic_only = false;
    std::optional<std::filesystem::path> json_output;
};

struct Fixture {
    std::vector<float> source;
    std::vector<std::string> ids;
    std::vector<getnative::AxisPlanRequest> requests;
    std::string identity;
};

struct Sample {
    double cold_plan_ms = 0.0;
    double warm_plan_ms = 0.0;
    double cpu_ms = 0.0;
    double backend_ms = 0.0;
    double cold_end_to_end_ms = 0.0;
    double warm_end_to_end_ms = 0.0;
    double plan_pack_ms = 0.0;
    double source_upload_ms = 0.0;
    double plan_upload_ms = 0.0;
    double inverse_image_ms = 0.0;
    double inverse_matrix_ms = 0.0;
    double first_forward_ms = 0.0;
    double metric_ms = 0.0;
    double partial_readback_ms = 0.0;
    double cpu_merge_ms = 0.0;
    double gpu_execution_ms = 0.0;
    double maximum_error = 0.0;
    double valley_distance = 0.0;
    bool within_tolerance = true;
    getnative::VulkanRuntimeTelemetry telemetry{};
};

struct Measurements {
    getnative::benchmark::Summary cold_plan_ms;
    getnative::benchmark::Summary warm_plan_ms;
    getnative::benchmark::Summary cpu_ms;
    getnative::benchmark::Summary backend_ms;
    getnative::benchmark::Summary cold_end_to_end_ms;
    getnative::benchmark::Summary warm_end_to_end_ms;
    getnative::benchmark::Summary plan_pack_ms;
    getnative::benchmark::Summary source_upload_ms;
    getnative::benchmark::Summary plan_upload_ms;
    getnative::benchmark::Summary inverse_image_ms;
    getnative::benchmark::Summary inverse_matrix_ms;
    getnative::benchmark::Summary first_forward_ms;
    getnative::benchmark::Summary metric_ms;
    getnative::benchmark::Summary partial_readback_ms;
    getnative::benchmark::Summary cpu_merge_ms;
    getnative::benchmark::Summary gpu_execution_ms;
    getnative::benchmark::Summary maximum_error;
    getnative::benchmark::Summary valley_distance;
    bool within_tolerance = true;
    std::size_t peak_device_bytes = 0;
    std::size_t peak_host_bytes = 0;
    std::size_t peak_total_bytes = 0;
    std::size_t maximum_allocations = 0;
    std::size_t maximum_reuses = 0;
    std::size_t maximum_submissions = 0;
    std::size_t maximum_completions = 0;
    bool used_non_coherent_upload = false;
    bool used_non_coherent_readback = false;
};

[[nodiscard]] std::size_t parse_size(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw std::invalid_argument("numeric option must be a positive integer");
    }
    return value;
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto require_value = [&](std::string_view option) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string{option} + " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--candidates") {
            result.candidates = parse_size(require_value(argument));
        } else if (argument == "--tile") {
            result.tile_size = parse_size(require_value(argument));
        } else if (argument == "--groups") {
            result.reduction_groups = parse_size(require_value(argument));
        } else if (argument == "--inverse-threads") {
            result.inverse_threads = parse_size(require_value(argument));
        } else if (argument == "--samples") {
            result.samples = parse_size(require_value(argument));
        } else if (argument == "--width") {
            result.width = static_cast<std::int32_t>(
                parse_size(require_value(argument)));
        } else if (argument == "--height") {
            result.height = static_cast<std::int32_t>(
                parse_size(require_value(argument)));
        } else if (argument == "--native-height") {
            result.native_height = static_cast<std::int32_t>(
                parse_size(require_value(argument)));
        } else if (argument == "--kernel") {
            result.kernel = require_value(argument);
        } else if (argument == "--json-out") {
            result.json_output = std::filesystem::path{require_value(argument)};
        } else if (argument == "--assert") {
            result.assert_gates = true;
        } else if (argument == "--validation") {
            result.enable_validation = true;
        } else if (argument == "--generic-only") {
            result.generic_only = true;
        } else if (argument == "--help") {
            throw std::invalid_argument(
                "usage: getnative_vulkan_benchmark [--candidates N] [--tile N] "
                "[--groups N] [--inverse-threads N] [--samples N] "
                "[--width N] [--height N] [--native-height N] "
                "[--kernel NAME] [--generic-only] [--validation] "
                "[--json-out PATH] [--assert]");
        } else {
            throw std::invalid_argument("unknown benchmark option: "
                                        + std::string{argument});
        }
    }
    if (result.width <= 0 || result.height <= 0 || result.native_height <= 0
        || result.native_height >= result.height) {
        throw std::invalid_argument("benchmark dimensions are invalid");
    }
    return result;
}

[[nodiscard]] getnative::Filter benchmark_filter(std::string_view name) {
    if (name == "bilinear") return getnative::Filter::bilinear();
    if (name == "bicubic" || name == "bicubic-catrom") {
        return getnative::Filter::bicubic(0.0, 0.5);
    }
    if (name == "bicubic-mitchell") {
        return getnative::Filter::bicubic(1.0 / 3.0, 1.0 / 3.0);
    }
    if (name == "spline16") return getnative::Filter::spline16();
    if (name == "spline36") return getnative::Filter::spline36();
    if (name == "spline64") return getnative::Filter::spline64();
    if (name.starts_with("lanczos")) {
        const std::size_t taps = parse_size(name.substr(7));
        if (taps >= 1 && taps <= 8) {
            return getnative::Filter::lanczos(static_cast<std::int32_t>(taps));
        }
    }
    throw std::invalid_argument("unsupported Vulkan benchmark kernel");
}

[[nodiscard]] std::uint64_t fnv1a_float_bytes(
    std::span<const float> values) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float value : values) {
        std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned byte = 0; byte < 4; ++byte) {
            hash ^= static_cast<std::uint8_t>(bits >> (byte * 8));
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

[[nodiscard]] Fixture make_fixture(const Configuration &config,
                                   const getnative::Filter &filter) {
    Fixture result;
    result.source.resize(static_cast<std::size_t>(config.width)
                         * static_cast<std::size_t>(config.height));
    for (std::int32_t y = 0; y < config.height; ++y) {
        for (std::int32_t x = 0; x < config.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y)
                * static_cast<std::size_t>(config.width)
                + static_cast<std::size_t>(x);
            result.source[index] = static_cast<float>(
                0.43 + 0.22 * std::sin(0.011 * static_cast<double>(x))
                + 0.18 * std::cos(0.017 * static_cast<double>(y))
                + 0.08 * std::sin(0.007 * static_cast<double>(x + y)));
        }
    }
    result.ids.reserve(config.candidates);
    result.requests.reserve(config.candidates);
    for (std::size_t index = 0; index < config.candidates; ++index) {
        const double active = 800.0 + 0.1 * static_cast<double>(index);
        std::ostringstream id;
        id << std::fixed << std::setprecision(1) << active;
        result.ids.push_back(id.str());
        result.requests.push_back({
            config.height, config.native_height, active, 0.0, filter,
            getnative::BorderMode::mirror});
    }
    std::ostringstream identity;
    identity << "synthetic-sincos-f32:"
             << std::hex << std::setfill('0') << std::setw(16)
             << fnv1a_float_bytes(result.source);
    result.identity = identity.str();
    return result;
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> attach_candidates(
    const Fixture &fixture,
    std::vector<std::shared_ptr<const getnative::AxisPlan>> plans) {
    if (plans.size() != fixture.ids.size()) {
        throw std::runtime_error("planner returned the wrong number of plans");
    }
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(plans.size());
    for (std::size_t index = 0; index < plans.size(); ++index) {
        candidates.push_back({fixture.ids[index], nullptr,
                              std::move(plans[index]),
                              getnative::AnalysisAxes::vertical});
    }
    return candidates;
}

[[nodiscard]] double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] Sample run_sample(
    const Configuration &config, const Fixture &fixture,
    getnative::AxisPlanCache &warm_cache,
    getnative::VulkanAnalysisEngine &vulkan,
    const getnative::MetricSpec &metric,
    bool warm_planner_first, bool gpu_first) {
    getnative::AxisPlanCache cold_cache;
    getnative::AxisPlanCacheBatchResult cold;
    getnative::AxisPlanCacheBatchResult warm;
    double cold_plan_ms = 0.0;
    double warm_plan_ms = 0.0;
    const auto run_cold = [&] {
        const auto start = Clock::now();
        cold = cold_cache.get_or_build_batch(fixture.requests);
        cold_plan_ms = milliseconds(start);
    };
    const auto run_warm = [&] {
        const auto start = Clock::now();
        warm = warm_cache.get_or_build_batch(fixture.requests);
        warm_plan_ms = milliseconds(start);
    };
    if (warm_planner_first) {
        run_warm();
        run_cold();
    } else {
        run_cold();
        run_warm();
    }
    if (cold.unique_key_count != fixture.requests.size()
        || cold.physical_build_count != cold.unique_key_count
        || cold.ready_hit_count != 0
        || warm.unique_key_count != fixture.requests.size()
        || warm.physical_build_count != 0
        || warm.ready_hit_count != fixture.requests.size()) {
        throw std::runtime_error("cold/warm planner cache invariant changed");
    }
    auto candidates = attach_candidates(fixture, std::move(warm.plans));
    const getnative::ConstImageView source{
        fixture.source.data(), config.width, config.height, config.width};
    std::vector<getnative::CandidateResult> cpu;
    std::vector<getnative::CandidateResult> gpu;
    double cpu_ms = 0.0;
    double backend_ms = 0.0;
    const auto run_cpu = [&] {
        const auto start = Clock::now();
        cpu = getnative::analyze_batch_f32(source, candidates, metric);
        cpu_ms = milliseconds(start);
    };
    const auto run_gpu = [&] {
        vulkan.reset_analysis_telemetry();
        const auto start = Clock::now();
        gpu = vulkan.analyze_axis_batch_f32(source, candidates, metric);
        backend_ms = milliseconds(start);
    };
    if (gpu_first) {
        run_gpu();
        run_cpu();
    } else {
        run_cpu();
        run_gpu();
    }
    if (cpu.size() != gpu.size()) {
        throw std::runtime_error("CPU and Vulkan result counts differ");
    }
    double maximum_error = 0.0;
    bool within_tolerance = true;
    for (std::size_t index = 0; index < cpu.size(); ++index) {
        if (cpu[index].id != gpu[index].id || !std::isfinite(gpu[index].error)) {
            throw std::runtime_error("Vulkan result identity or finiteness changed");
        }
        const double difference = std::abs(cpu[index].error - gpu[index].error);
        maximum_error = std::max(maximum_error, difference);
        within_tolerance = within_tolerance
            && difference <= std::max(1e-7, 5e-4 * std::abs(cpu[index].error));
    }
    const std::size_t cpu_minimum = static_cast<std::size_t>(
        std::min_element(cpu.begin(), cpu.end(), [](const auto &lhs,
                                                    const auto &rhs) {
            return lhs.error < rhs.error;
        }) - cpu.begin());
    const std::size_t gpu_minimum = static_cast<std::size_t>(
        std::min_element(gpu.begin(), gpu.end(), [](const auto &lhs,
                                                    const auto &rhs) {
            return lhs.error < rhs.error;
        }) - gpu.begin());
    const std::size_t valley_distance = cpu_minimum > gpu_minimum
        ? cpu_minimum - gpu_minimum : gpu_minimum - cpu_minimum;
    const getnative::VulkanRuntimeTelemetry telemetry =
        vulkan.runtime_telemetry();
    if (telemetry.queue_submission_count != telemetry.queue_completion_count) {
        throw std::runtime_error("Vulkan benchmark returned before queue drain");
    }
    return {
        cold_plan_ms, warm_plan_ms, cpu_ms, backend_ms,
        cold_plan_ms + backend_ms, warm_plan_ms + backend_ms,
        telemetry.plan_pack_ms, telemetry.source_upload_ms,
        telemetry.plan_upload_ms, telemetry.inverse_image_ms,
        telemetry.inverse_matrix_ms, telemetry.first_forward_ms,
        telemetry.metric_ms, telemetry.partial_readback_ms,
        telemetry.cpu_merge_ms, telemetry.gpu_execution_ms,
        maximum_error, static_cast<double>(valley_distance), within_tolerance,
        telemetry,
    };
}

template <class Projection>
[[nodiscard]] getnative::benchmark::Summary summarize_field(
    const std::vector<Sample> &samples, Projection projection) {
    std::vector<double> values;
    values.reserve(samples.size());
    for (const Sample &sample : samples) values.push_back(projection(sample));
    return getnative::benchmark::summarize(std::move(values));
}

[[nodiscard]] Measurements summarize(const std::vector<Sample> &samples) {
    Measurements result{
        summarize_field(samples, [](const Sample &s) { return s.cold_plan_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.warm_plan_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.cpu_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.backend_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.cold_end_to_end_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.warm_end_to_end_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.plan_pack_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.source_upload_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.plan_upload_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.inverse_image_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.inverse_matrix_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.first_forward_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.metric_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.partial_readback_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.cpu_merge_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.gpu_execution_ms; }),
        summarize_field(samples, [](const Sample &s) { return s.maximum_error; }),
        summarize_field(samples, [](const Sample &s) { return s.valley_distance; }),
    };
    for (const Sample &sample : samples) {
        result.within_tolerance = result.within_tolerance && sample.within_tolerance;
        result.peak_device_bytes = std::max(
            result.peak_device_bytes, sample.telemetry.peak_device_bytes);
        result.peak_host_bytes = std::max(
            result.peak_host_bytes, sample.telemetry.peak_host_bytes);
        result.peak_total_bytes = std::max(
            result.peak_total_bytes,
            sample.telemetry.peak_total_explicit_bytes);
        result.maximum_allocations = std::max(
            result.maximum_allocations,
            sample.telemetry.working_buffer_allocation_count);
        result.maximum_reuses = std::max(
            result.maximum_reuses,
            sample.telemetry.working_buffer_reuse_count);
        result.maximum_submissions = std::max(
            result.maximum_submissions,
            sample.telemetry.queue_submission_count);
        result.maximum_completions = std::max(
            result.maximum_completions,
            sample.telemetry.queue_completion_count);
        result.used_non_coherent_upload = result.used_non_coherent_upload
            || sample.telemetry.used_non_coherent_upload;
        result.used_non_coherent_readback = result.used_non_coherent_readback
            || sample.telemetry.used_non_coherent_readback;
    }
    return result;
}

void append_phase(std::ostream &output, std::string_view name,
                  const getnative::benchmark::Summary &summary,
                  bool &first) {
    if (!first) output << ',';
    first = false;
    output << getnative::benchmark::json_string(name) << ':';
    getnative::benchmark::append_summary(output, summary);
}

[[nodiscard]] std::string make_json(
    const Configuration &config, const Fixture &fixture,
    const Measurements &measurements,
    const getnative::VulkanAnalysisEngine &vulkan,
    const getnative::CpuDispatchInfo &cpu_dispatch,
    double warm_cache_setup_ms, int argc, char **argv) {
    const auto &device = vulkan.device_info();
    const double speedup = measurements.cpu_ms.median
        / measurements.backend_ms.median;
    const bool correctness = measurements.within_tolerance
        && measurements.valley_distance.maximum <= 1.0;
    const bool memory_gate = measurements.peak_total_bytes
        < 2ULL * 1024ULL * 1024ULL * 1024ULL;
    const bool complete_sample_set = config.samples == 21;
    const bool default_eligible = correctness && memory_gate
        && complete_sample_set && speedup >= 3.0;

    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_vulkan_benchmark",
        "cold AxisPlanCache batch + retained warm session cache",
        fixture.identity, argc, argv);
    output << ",\"sample_count\":" << config.samples
           << ",\"warmup_count\":1"
           << ",\"configuration\":{\"width\":" << config.width
           << ",\"height\":" << config.height
           << ",\"native_height\":" << config.native_height
           << ",\"candidate_count\":" << config.candidates
           << ",\"fractional_scan\":\"800.0 + 0.1 * index\""
           << ",\"kernel\":"
           << getnative::benchmark::json_string(config.kernel)
           << ",\"tile_size\":" << config.tile_size
           << ",\"groups_per_candidate\":" << config.reduction_groups
           << ",\"inverse_threads\":" << config.inverse_threads
           << ",\"crop\":5,\"threshold\":0.015,\"p\":1}"
           << ",\"device\":{\"name\":"
           << getnative::benchmark::json_string(device.name)
           << ",\"stable_selector\":"
           << getnative::benchmark::json_string(device.stable_selector)
           << ",\"vendor_id\":" << device.vendor_id
           << ",\"device_id\":" << device.device_id
           << ",\"driver_version\":" << device.driver_version
           << ",\"api_version\":" << device.api_version
           << ",\"compute_queue_family\":" << device.compute_queue_family
           << ",\"non_coherent_atom_size\":" << device.non_coherent_atom_size
           << ",\"float_controls\":{\"denorm_preserve_f32\":"
           << (device.float_controls.denorm_preserve_f32 ? "true" : "false")
           << ",\"signed_zero_inf_nan_preserve_f32\":"
           << (device.float_controls.signed_zero_inf_nan_preserve_f32
                   ? "true" : "false")
           << ",\"rounding_mode_rte_f32\":"
           << (device.float_controls.rounding_mode_rte_f32 ? "true" : "false")
           << "}}"
           << ",\"cpu_denominator\":{\"request\":\"auto\",\"math_mode\":"
           << getnative::benchmark::json_string(cpu_dispatch.math_mode)
           << ",\"selected_isa\":"
           << getnative::benchmark::json_string(
                  getnative::cpu_isa_name(cpu_dispatch.selected))
           << ",\"selection_reason\":"
           << getnative::benchmark::json_string(cpu_dispatch.selection_reason)
           << ",\"vendor\":"
           << getnative::benchmark::json_string(
                  getnative::cpu_vendor(cpu_dispatch.snapshot))
           << ",\"family\":" << cpu_dispatch.snapshot.family
           << ",\"model\":" << cpu_dispatch.snapshot.model
           << ",\"stepping\":" << cpu_dispatch.snapshot.stepping << '}'
           << ",\"artifact\":{\"runtime_shader_compiler\":false"
           << ",\"target\":\"Vulkan 1.2 / SPIR-V\""
           << ",\"math\":{\"path\":\"production\",\"fma_allowed\":true,"
              "\"multi_math_mode\":false,"
              "\"notes\":\"ordinary Float32 ops; no GLSL precise / NoContraction dual path\"}}"
           << ",\"planner\":{\"warm_cache_setup_ms\":"
           << std::setprecision(17) << warm_cache_setup_ms << '}'
           << ",\"phases_ms\":{";
    bool first = true;
    append_phase(output, "cold_planner", measurements.cold_plan_ms, first);
    append_phase(output, "warm_session_cache_planner", measurements.warm_plan_ms, first);
    append_phase(output, "plan_pack", measurements.plan_pack_ms, first);
    append_phase(output, "source_upload", measurements.source_upload_ms, first);
    append_phase(output, "plan_upload", measurements.plan_upload_ms, first);
    append_phase(output, "inverse_image", measurements.inverse_image_ms, first);
    append_phase(output, "inverse_matrix", measurements.inverse_matrix_ms, first);
    append_phase(output, "first_forward", measurements.first_forward_ms, first);
    append_phase(output, "metric", measurements.metric_ms, first);
    append_phase(output, "partial_readback_host", measurements.partial_readback_ms, first);
    append_phase(output, "cpu_merge", measurements.cpu_merge_ms, first);
    append_phase(output, "gpu_compute_timestamp", measurements.gpu_execution_ms, first);
    append_phase(output, "cpu_execution", measurements.cpu_ms, first);
    append_phase(output, "backend_total_wall", measurements.backend_ms, first);
    append_phase(output, "cold_end_to_end", measurements.cold_end_to_end_ms, first);
    append_phase(output, "warm_end_to_end", measurements.warm_end_to_end_ms, first);
    output << '}'
           << ",\"resources\":{\"peak_device_bytes\":"
           << measurements.peak_device_bytes
           << ",\"peak_host_bytes\":" << measurements.peak_host_bytes
           << ",\"peak_total_explicit_bytes\":" << measurements.peak_total_bytes
           << ",\"maximum_working_allocations\":"
           << measurements.maximum_allocations
           << ",\"maximum_working_reuses\":" << measurements.maximum_reuses
           << ",\"maximum_submissions\":" << measurements.maximum_submissions
           << ",\"maximum_completions\":" << measurements.maximum_completions
           << ",\"used_non_coherent_upload\":"
           << (measurements.used_non_coherent_upload ? "true" : "false")
           << ",\"used_non_coherent_readback\":"
           << (measurements.used_non_coherent_readback ? "true" : "false") << '}'
           << ",\"correctness\":{\"maximum_error\":";
    getnative::benchmark::append_summary(output, measurements.maximum_error);
    output << ",\"valley_distance\":";
    getnative::benchmark::append_summary(output, measurements.valley_distance);
    output << ",\"within_tolerance\":"
           << (measurements.within_tolerance ? "true" : "false") << '}'
           << ",\"decision\":{\"cpu_to_backend_speedup\":"
           << std::setprecision(17) << speedup
           << ",\"requires_21_samples\":"
           << (complete_sample_set ? "true" : "false")
           << ",\"correctness_gate\":" << (correctness ? "true" : "false")
           << ",\"memory_gate\":" << (memory_gate ? "true" : "false")
           << ",\"default_eligible\":"
           << (default_eligible ? "true" : "false")
           << ",\"status\":"
           << getnative::benchmark::json_string(default_eligible
                  ? "ELIGIBLE_FOR_DEFAULT_REVIEW"
                  : "KEEP_EXPLICIT_EXPERIMENTAL") << "}}";
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
        const Fixture fixture = make_fixture(config, filter);
        const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
        getnative::AxisPlanCache warm_cache;
        const auto cache_start = Clock::now();
        const auto setup = warm_cache.get_or_build_batch(fixture.requests);
        const double warm_cache_setup_ms = milliseconds(cache_start);
        if (setup.unique_key_count != fixture.requests.size()
            || setup.physical_build_count != setup.unique_key_count
            || setup.ready_hit_count != 0) {
            throw std::runtime_error("warm session cache setup invariant changed");
        }

        getnative::VulkanAnalysisOptions options;
        options.tile_size = config.tile_size;
        options.reduction_groups_per_candidate = config.reduction_groups;
        options.inverse_threads_per_workgroup = config.inverse_threads;
        options.enable_validation = config.enable_validation;
        options.kernel_dispatch = config.generic_only
            ? getnative::VulkanKernelDispatchPolicy::generic_only
            : getnative::VulkanKernelDispatchPolicy::automatic;
        getnative::VulkanAnalysisEngine vulkan(options);

        // One unmeasured warmup establishes pipelines, retained buffers, and driver caches.
        (void)run_sample(config, fixture, warm_cache, vulkan, metric, false, false);
        std::vector<Sample> samples;
        samples.reserve(config.samples);
        for (std::size_t index = 0; index < config.samples; ++index) {
            samples.push_back(run_sample(
                config, fixture, warm_cache, vulkan, metric,
                (index & 1U) != 0, (index & 1U) != 0));
        }
        const Measurements measurements = summarize(samples);
        const getnative::CpuDispatchInfo cpu_dispatch =
            getnative::cpu_dispatch_info();
        const std::string json = make_json(
            config, fixture, measurements, vulkan, cpu_dispatch,
            warm_cache_setup_ms, argc, argv);

        const double speedup = measurements.cpu_ms.median
            / measurements.backend_ms.median;
        const bool assertions_pass = measurements.within_tolerance
            && measurements.valley_distance.maximum <= 1.0
            && measurements.maximum_submissions == measurements.maximum_completions
            && measurements.peak_total_bytes
                < 2ULL * 1024ULL * 1024ULL * 1024ULL;
        std::cout << "device=" << vulkan.device_info().name << '\n'
                  << "samples=" << config.samples << '\n'
                  << "cold_plan_ms=" << measurements.cold_plan_ms.median << '\n'
                  << "warm_plan_ms=" << measurements.warm_plan_ms.median << '\n'
                  << "cpu_auto_strict_ms=" << measurements.cpu_ms.median << '\n'
                  << "vulkan_backend_ms=" << measurements.backend_ms.median << '\n'
                  << "speedup=" << speedup << '\n'
                  << "maximum_error=" << measurements.maximum_error.maximum << '\n'
                  << "valley_distance=" << measurements.valley_distance.maximum << '\n'
                  << "peak_total_explicit_bytes=" << measurements.peak_total_bytes << '\n'
                  << "assertions=" << (assertions_pass ? "PASS" : "FAIL") << '\n';
        if (config.json_output) {
            getnative::benchmark::atomic_write_json(*config.json_output, json);
        }
        if (config.assert_gates && !assertions_pass) return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Vulkan benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

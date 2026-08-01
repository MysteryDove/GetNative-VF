#include "benchmark_support.hpp"
#include "formal_matrix.hpp"

#include "getnative/cpu_features.hpp"
#include "getnative/cuda_analysis.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef GETNATIVE_CUDA_TOOLKIT_VERSION
#define GETNATIVE_CUDA_TOOLKIT_VERSION "unknown"
#endif

#ifndef GETNATIVE_CUDA_FATBIN_PATH
#define GETNATIVE_CUDA_FATBIN_PATH ""
#endif

namespace {

using Clock = std::chrono::steady_clock;
namespace formal = getnative::benchmark::formal;

constexpr std::array<std::size_t, 5> multiframe_counts{
    1U, 2U, 10U, 100U, 1000U,
};

struct Configuration {
    std::filesystem::path matrix_path;
    std::optional<std::string> selected_case;
    std::optional<std::filesystem::path> artifact_root;
    std::optional<std::filesystem::path> json_output;
    std::optional<std::filesystem::path> fatbin_path;
    std::size_t samples = 21U;
    std::size_t multiframe_samples = 21U;
    std::int32_t device_ordinal = 0;
    std::string device_uuid;
    getnative::CudaKernelVariant variant = getnative::CudaKernelVariant::automatic;
    bool all_variants = false;
    bool assert_correctness = false;
    bool list_cases = false;
    bool profile_mode = false;
    bool skip_multiframe = false;
};

[[nodiscard]] std::size_t parse_size(
    std::string_view text, std::string_view name) {
    std::size_t result = 0U;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()
        || result == 0U) {
        throw std::invalid_argument(std::string{name} + " must be positive");
    }
    return result;
}

[[nodiscard]] std::int32_t parse_i32(
    std::string_view text, std::string_view name) {
    std::int32_t result = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument(std::string{name} + " must fit int32");
    }
    return result;
}

[[nodiscard]] getnative::CudaKernelVariant parse_variant(
    std::string_view value) {
    if (value == "automatic") return getnative::CudaKernelVariant::automatic;
    if (value == "cpp-generic") return getnative::CudaKernelVariant::cpp_generic;
    if (value == "cpp-specialized") {
        return getnative::CudaKernelVariant::cpp_specialized;
    }
    if (value == "architecture-specific") {
        return getnative::CudaKernelVariant::architecture_specific;
    }
    if (value == "inline-ptx") return getnative::CudaKernelVariant::inline_ptx;
    throw std::invalid_argument("unknown --cuda-variant value");
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next = [&](std::string_view name) -> std::string_view {
            ++index;
            if (index >= argc) {
                throw std::invalid_argument(std::string{name} + " needs a value");
            }
            return argv[index];
        };
        if (argument == "--matrix") {
            result.matrix_path = std::filesystem::path{next(argument)};
        } else if (argument == "--case") {
            result.selected_case = std::string{next(argument)};
        } else if (argument == "--samples") {
            result.samples = parse_size(next(argument), argument);
        } else if (argument == "--multiframe-samples") {
            result.multiframe_samples = parse_size(next(argument), argument);
        } else if (argument == "--device-ordinal") {
            result.device_ordinal = parse_i32(next(argument), argument);
        } else if (argument == "--device-uuid") {
            result.device_uuid = next(argument);
        } else if (argument == "--cuda-variant") {
            const std::string_view value = next(argument);
            if (value == "all") {
                result.all_variants = true;
            } else {
                result.variant = parse_variant(value);
            }
        } else if (argument == "--artifact-root") {
            result.artifact_root = std::filesystem::path{next(argument)};
        } else if (argument == "--json-out") {
            result.json_output = std::filesystem::path{next(argument)};
        } else if (argument == "--fatbin") {
            result.fatbin_path = std::filesystem::path{next(argument)};
        } else if (argument == "--assert") {
            result.assert_correctness = true;
        } else if (argument == "--list-cases") {
            result.list_cases = true;
        } else if (argument == "--profile") {
            result.profile_mode = true;
            result.skip_multiframe = true;
        } else if (argument == "--skip-multiframe") {
            result.skip_multiframe = true;
        } else if (argument == "--help") {
            std::cout
                << "usage: getnative_cuda_benchmark --matrix PATH "
                   "[--case ID] [--samples N] [--multiframe-samples N] "
                   "[--cuda-variant automatic|cpp-generic|cpp-specialized|all] "
                   "[--device-ordinal N|--device-uuid UUID] "
                   "[--artifact-root PATH|--json-out PATH] [--fatbin PATH] "
                   "[--assert] [--list-cases] [--profile] [--skip-multiframe]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument(
                "unknown CUDA benchmark argument: " + std::string{argument});
        }
    }
    if (result.matrix_path.empty()) {
        throw std::invalid_argument("--matrix is required");
    }
    if (!result.list_cases && !result.artifact_root && !result.json_output) {
        throw std::invalid_argument(
            "measurement requires --artifact-root or --json-out");
    }
    if (result.artifact_root && result.json_output) {
        throw std::invalid_argument(
            "--artifact-root and --json-out are mutually exclusive");
    }
    if (result.all_variants && !result.selected_case) {
        throw std::invalid_argument(
            "--cuda-variant all is diagnostic and requires --case");
    }
    if (result.profile_mode && !result.selected_case) {
        throw std::invalid_argument("--profile requires --case");
    }
    return result;
}

[[nodiscard]] std::filesystem::path default_fatbin_path() {
    return std::filesystem::path{GETNATIVE_CUDA_FATBIN_PATH};
}

[[nodiscard]] std::vector<getnative::CudaKernelVariant> variants_to_run(
    const Configuration &config) {
    if (config.all_variants) {
        return {
            getnative::CudaKernelVariant::automatic,
            getnative::CudaKernelVariant::cpp_generic,
            getnative::CudaKernelVariant::cpp_specialized,
        };
    }
    return {config.variant};
}

[[nodiscard]] getnative::CudaAnalysisOptions cuda_options(
    const Configuration &config, getnative::CudaKernelVariant variant,
    const formal::Matrix &matrix) {
    getnative::CudaAnalysisOptions options{};
    options.device_uuid = config.device_uuid;
    options.device_ordinal = config.device_ordinal;
    options.tile_size = static_cast<std::size_t>(matrix.tile_size);
    options.reduction_groups_per_candidate =
        static_cast<std::size_t>(matrix.reduction_groups);
    options.inverse_threads_per_block =
        static_cast<std::size_t>(matrix.inverse_threads);
    options.kernel_variant = variant;
    return options;
}

std::atomic<std::uint64_t> benchmark_sink{0U};

[[nodiscard]] std::uint64_t checksum(
    const std::vector<getnative::CandidateResult> &results) noexcept {
    std::uint64_t value = 0U;
    for (const auto &result : results) {
        value ^= std::bit_cast<std::uint64_t>(result.error);
        value = (value << 7U) | (value >> (64U - 7U));
    }
    return value;
}

template <class Function>
[[nodiscard]] double elapsed_ms(Function &&function) {
    const auto begin = Clock::now();
    const std::uint64_t value = static_cast<std::uint64_t>(function());
    const auto end = Clock::now();
    benchmark_sink.fetch_xor(value, std::memory_order_relaxed);
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class Function>
[[nodiscard]] getnative::benchmark::Summary measure(
    std::size_t samples, Function &&function) {
    (void)function();
    std::vector<double> raw;
    raw.reserve(samples);
    for (std::size_t sample = 0U; sample < samples; ++sample) {
        raw.push_back(elapsed_ms(function));
    }
    return getnative::benchmark::summarize(std::move(raw));
}

struct PlannerMeasurement {
    getnative::benchmark::Summary cold_ms;
    getnative::benchmark::Summary warm_session_cache_ms;
};

[[nodiscard]] PlannerMeasurement measure_planner(
    std::size_t samples, const formal::PreparedCase &prepared) {
    PlannerMeasurement result{};
    result.cold_ms = measure(samples, [&]() -> std::uint64_t {
        getnative::AxisPlanCache cache;
        const auto built = cache.get_or_build_batch(prepared.requests);
        return static_cast<std::uint64_t>(built.plans.size())
            ^ (static_cast<std::uint64_t>(built.physical_build_count) << 32U);
    });

    getnative::AxisPlanCache warm_cache;
    (void)warm_cache.get_or_build_batch(prepared.requests);
    result.warm_session_cache_ms = measure(samples, [&]() -> std::uint64_t {
        const auto built = warm_cache.get_or_build_batch(prepared.requests);
        return static_cast<std::uint64_t>(built.ready_hit_count)
            ^ (static_cast<std::uint64_t>(built.physical_build_count) << 32U);
    });
    return result;
}

struct Correctness {
    double maximum_absolute_error = 0.0;
    double maximum_relative_error = 0.0;
    std::size_t valley_distance = 0U;
    std::size_t top_k = 0U;
    std::size_t top_k_overlap = 0U;
    bool ids_and_order = true;
    bool finite = true;
    bool within_tolerance = true;
};

[[nodiscard]] double relative_tolerance() noexcept {
    // Handover §15.4 production GPU tolerance vs CPU reference.
    return 5e-4;
}

[[nodiscard]] std::vector<std::size_t> sorted_metric_indices(
    const std::vector<getnative::CandidateResult> &results) {
    std::vector<std::size_t> indices(results.size());
    for (std::size_t index = 0U; index < indices.size(); ++index) {
        indices[index] = index;
    }
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t left,
                                                          std::size_t right) {
        return results[left].error < results[right].error;
    });
    return indices;
}

[[nodiscard]] Correctness assess(
    const std::vector<getnative::CandidateResult> &cpu,
    const std::vector<getnative::CandidateResult> &gpu) {
    Correctness result{};
    if (cpu.size() != gpu.size()) {
        result.ids_and_order = false;
        result.finite = false;
        result.within_tolerance = false;
        return result;
    }
    const double relative_limit = relative_tolerance();
    for (std::size_t index = 0U; index < cpu.size(); ++index) {
        result.ids_and_order = result.ids_and_order
            && cpu[index].id == gpu[index].id;
        result.finite = result.finite && std::isfinite(cpu[index].error)
            && std::isfinite(gpu[index].error);
        const double absolute = std::abs(gpu[index].error - cpu[index].error);
        const double relative = absolute
            / std::max(std::abs(cpu[index].error), 1e-30);
        result.maximum_absolute_error = std::max(
            result.maximum_absolute_error, absolute);
        result.maximum_relative_error = std::max(
            result.maximum_relative_error, relative);
        const double tolerance = std::max(
            1e-7, relative_limit * std::abs(cpu[index].error));
        result.within_tolerance = result.within_tolerance
            && absolute <= tolerance;
    }
    if (!cpu.empty()) {
        const auto cpu_order = sorted_metric_indices(cpu);
        const auto gpu_order = sorted_metric_indices(gpu);
        result.valley_distance = cpu_order.front() > gpu_order.front()
            ? cpu_order.front() - gpu_order.front()
            : gpu_order.front() - cpu_order.front();
        result.top_k = std::min<std::size_t>(10U, cpu.size());
        std::set<std::size_t> cpu_top(
            cpu_order.begin(), cpu_order.begin()
                + static_cast<std::ptrdiff_t>(result.top_k));
        for (std::size_t index = 0U; index < result.top_k; ++index) {
            if (cpu_top.contains(gpu_order[index])) ++result.top_k_overlap;
        }
    }
    return result;
}

[[nodiscard]] bool same_bits(
    const std::vector<getnative::CandidateResult> &left,
    const std::vector<getnative::CandidateResult> &right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].id != right[index].id
            || std::bit_cast<std::uint64_t>(left[index].error)
                != std::bit_cast<std::uint64_t>(right[index].error)) {
            return false;
        }
    }
    return true;
}

struct PhaseSamples {
    std::vector<double> backend_wall_ms;
    std::vector<double> plan_pack_ms;
    std::vector<double> source_upload_ms;
    std::vector<double> plan_upload_ms;
    std::vector<double> inverse_h_ms;
    std::vector<double> inverse_v_ms;
    std::vector<double> first_forward_ms;
    std::vector<double> metric_ms;
    std::vector<double> gpu_execution_ms;
    std::vector<double> partial_readback_ms;
    std::vector<double> cpu_merge_ms;
    std::vector<double> allocation_ms;
    std::vector<double> allocation_count;
    std::vector<double> working_reuse_count;
};

void append_phase_sample(
    PhaseSamples &samples, double wall,
    const getnative::CudaRuntimeTelemetry &telemetry) {
    samples.backend_wall_ms.push_back(wall);
    samples.plan_pack_ms.push_back(telemetry.plan_pack_ms);
    samples.source_upload_ms.push_back(telemetry.source_upload_ms);
    samples.plan_upload_ms.push_back(telemetry.plan_upload_ms);
    samples.inverse_h_ms.push_back(telemetry.inverse_h_ms);
    samples.inverse_v_ms.push_back(telemetry.inverse_v_ms);
    samples.first_forward_ms.push_back(telemetry.first_forward_ms);
    samples.metric_ms.push_back(telemetry.metric_ms);
    samples.gpu_execution_ms.push_back(telemetry.gpu_execution_ms);
    samples.partial_readback_ms.push_back(telemetry.partial_readback_ms);
    samples.cpu_merge_ms.push_back(telemetry.cpu_merge_ms);
    samples.allocation_ms.push_back(telemetry.buffer_allocation_ms);
    samples.allocation_count.push_back(
        static_cast<double>(telemetry.buffer_allocation_count));
    samples.working_reuse_count.push_back(
        static_cast<double>(telemetry.working_buffer_reuse_count));
}

struct EngineMeasurement {
    getnative::CudaKernelVariant variant = getnative::CudaKernelVariant::automatic;
    std::unique_ptr<getnative::CudaAnalysisEngine> engine;
    double cold_constructor_wall_ms = 0.0;
    double cold_first_prepared_frame_wall_ms = 0.0;
    double cold_module_load_ms = 0.0;
    double warm_constructor_wall_ms = 0.0;
    double warm_module_load_ms = 0.0;
};

[[nodiscard]] std::vector<EngineMeasurement> create_engines(
    const Configuration &config, const formal::Matrix &matrix,
    const getnative::ConstImageView source,
    const formal::PreparedCase &primary_prepared) {
    std::vector<EngineMeasurement> result;
    for (const auto variant : variants_to_run(config)) {
        EngineMeasurement measurement{};
        measurement.variant = variant;
        const auto cold_begin = Clock::now();
        auto cold = std::make_unique<getnative::CudaAnalysisEngine>(
            cuda_options(config, variant, matrix));
        const auto cold_constructed = Clock::now();
        measurement.cold_constructor_wall_ms =
            std::chrono::duration<double, std::milli>(
                cold_constructed - cold_begin).count();
        measurement.cold_module_load_ms =
            cold->runtime_telemetry().module_load_ms;
        const std::size_t locked_index = primary_prepared.candidates.size() / 2U;
        const std::array<getnative::CandidateAnalysis, 1> locked_candidate{
            primary_prepared.candidates[locked_index],
        };
        (void)cold->analyze_axis_batch_f32(
            source, locked_candidate, matrix.metric);
        const auto cold_finished = Clock::now();
        measurement.cold_first_prepared_frame_wall_ms =
            std::chrono::duration<double, std::milli>(
                cold_finished - cold_begin).count();
        cold.reset();

        const auto warm_begin = Clock::now();
        measurement.engine = std::make_unique<getnative::CudaAnalysisEngine>(
            cuda_options(config, variant, matrix));
        const auto warm_end = Clock::now();
        measurement.warm_constructor_wall_ms =
            std::chrono::duration<double, std::milli>(warm_end - warm_begin).count();
        measurement.warm_module_load_ms =
            measurement.engine->runtime_telemetry().module_load_ms;
        result.push_back(std::move(measurement));
    }
    return result;
}

struct VariantCaseMeasurement {
    getnative::CudaKernelVariant variant = getnative::CudaKernelVariant::automatic;
    PhaseSamples phases;
    std::vector<double> end_to_end_raw;
    std::vector<getnative::CandidateResult> reference_results;
    Correctness correctness;
    bool stable_bits = true;
    bool generic_comparison_measured = false;
    bool bit_identical_to_generic = true;
    getnative::CudaRuntimeTelemetry final_telemetry{};
};

struct CaseMeasurement {
    formal::Case benchmark_case;
    std::string candidate_fingerprint;
    PlannerMeasurement planner;
    std::vector<double> cpu_execution_raw;
    std::vector<double> cpu_end_to_end_raw;
    std::vector<getnative::CandidateResult> cpu_reference;
    std::vector<VariantCaseMeasurement> variants;
};

[[nodiscard]] std::vector<getnative::CandidateResult> run_cpu_execution(
    const getnative::ConstImageView source,
    const formal::PreparedCase &prepared,
    const getnative::MetricSpec &metric) {
    return getnative::analyze_batch_f32(
        source, prepared.candidates, metric);
}

[[nodiscard]] std::vector<getnative::CandidateResult> run_cpu_end_to_end(
    const getnative::ConstImageView source,
    const formal::PreparedCase &prepared,
    const getnative::MetricSpec &metric) {
    getnative::AxisPlanCache cache;
    const auto plans = cache.get_or_build_batch(prepared.requests).plans;
    const auto candidates = formal::make_candidates(plans);
    return getnative::analyze_batch_f32(source, candidates, metric);
}

void run_cpu_sample(
    const getnative::ConstImageView source,
    const formal::PreparedCase &prepared,
    const getnative::MetricSpec &metric,
    CaseMeasurement &measurement) {
    std::vector<getnative::CandidateResult> execution_results;
    measurement.cpu_execution_raw.push_back(elapsed_ms([&]() -> std::uint64_t {
        execution_results = run_cpu_execution(source, prepared, metric);
        return checksum(execution_results);
    }));
    measurement.cpu_end_to_end_raw.push_back(elapsed_ms([&]() -> std::uint64_t {
        const auto results = run_cpu_end_to_end(source, prepared, metric);
        if (!same_bits(results, measurement.cpu_reference)) {
            throw std::runtime_error("CPU end-to-end result changed by bits");
        }
        return checksum(results);
    }));
    if (!same_bits(execution_results, measurement.cpu_reference)) {
        throw std::runtime_error("CPU prepared result changed by bits");
    }
}

void run_cuda_sample(
    const getnative::ConstImageView source,
    const formal::PreparedCase &prepared,
    const getnative::MetricSpec &metric,
    EngineMeasurement &engine,
    VariantCaseMeasurement &measurement) {
    engine.engine->reset_analysis_telemetry();
    std::vector<getnative::CandidateResult> backend_results;
    const double backend_wall = elapsed_ms([&]() -> std::uint64_t {
        backend_results = engine.engine->analyze_axis_batch_f32(
            source, prepared.candidates, metric);
        return checksum(backend_results);
    });
    measurement.final_telemetry = engine.engine->runtime_telemetry();
    append_phase_sample(
        measurement.phases, backend_wall, measurement.final_telemetry);
    measurement.stable_bits = measurement.stable_bits
        && same_bits(backend_results, measurement.reference_results);

    measurement.end_to_end_raw.push_back(elapsed_ms([&]() -> std::uint64_t {
        getnative::AxisPlanCache cache;
        const auto plans = cache.get_or_build_batch(prepared.requests).plans;
        const auto candidates = formal::make_candidates(plans);
        const auto results = engine.engine->analyze_axis_batch_f32(
            source, candidates, metric);
        if (!same_bits(results, measurement.reference_results)) {
            throw std::runtime_error("CUDA end-to-end result changed by bits");
        }
        return checksum(results);
    }));
}

[[nodiscard]] CaseMeasurement measure_case(
    const Configuration &config,
    const formal::Matrix &matrix,
    const getnative::ConstImageView source,
    const formal::Case &benchmark_case,
    const formal::PreparedCase &prepared,
    std::vector<EngineMeasurement> &engines) {
    CaseMeasurement result{};
    result.benchmark_case = benchmark_case;
    result.candidate_fingerprint = formal::candidate_contract_fingerprint(
        matrix, benchmark_case);
    result.planner = measure_planner(config.samples, prepared);
    result.cpu_reference = run_cpu_execution(source, prepared, matrix.metric);

    result.variants.reserve(engines.size());
    for (EngineMeasurement &engine : engines) {
        VariantCaseMeasurement variant{};
        variant.variant = engine.variant;
        variant.reference_results = engine.engine->analyze_axis_batch_f32(
            source, prepared.candidates, matrix.metric);
        variant.correctness = assess(
            result.cpu_reference, variant.reference_results);
        result.variants.push_back(std::move(variant));
    }

    for (std::size_t sample = 0U; sample < config.samples; ++sample) {
        if ((sample & 1U) == 0U) {
            run_cpu_sample(source, prepared, matrix.metric, result);
            for (std::size_t index = 0U; index < engines.size(); ++index) {
                run_cuda_sample(
                    source, prepared, matrix.metric,
                    engines[index], result.variants[index]);
            }
        } else {
            for (std::size_t index = engines.size(); index > 0U; --index) {
                run_cuda_sample(
                    source, prepared, matrix.metric,
                    engines[index - 1U], result.variants[index - 1U]);
            }
            run_cpu_sample(source, prepared, matrix.metric, result);
        }
    }

    const auto generic = std::find_if(
        result.variants.begin(), result.variants.end(), [](const auto &value) {
            return value.variant == getnative::CudaKernelVariant::cpp_generic;
        });
    if (generic != result.variants.end()) {
        for (auto &variant : result.variants) {
            variant.generic_comparison_measured = true;
            variant.bit_identical_to_generic = same_bits(
                variant.reference_results, generic->reference_results);
        }
    }
    return result;
}

struct MultiframeCase {
    std::size_t frame_count = 0U;
    getnative::benchmark::Summary cpu_serial_ms;
    getnative::benchmark::Summary cpu_parallel_ms;
    getnative::benchmark::Summary cuda_serial_ms;
    double cpu_serial_frames_per_second = 0.0;
    double cpu_parallel_frames_per_second = 0.0;
    double cuda_frames_per_second = 0.0;
    double cuda_speedup_vs_cpu_serial = 0.0;
    bool correct = false;
};

struct MultiframeMeasurement {
    bool measured = false;
    std::size_t ring_size = 0U;
    std::size_t worker_limit = 0U;
    std::string ring_fingerprint;
    std::vector<MultiframeCase> cases;
};

[[nodiscard]] std::vector<std::vector<float>> make_frame_ring(
    const formal::Matrix &matrix) {
    constexpr std::size_t ring_size = 4U;
    std::vector<std::vector<float>> result(ring_size);
    for (std::size_t frame = 0U; frame < ring_size; ++frame) {
        auto &pixels = result[frame];
        pixels.resize(
            static_cast<std::size_t>(matrix.source_width)
            * static_cast<std::size_t>(matrix.source_height));
        for (std::int32_t row = 0; row < matrix.source_height; ++row) {
            for (std::int32_t column = 0; column < matrix.source_width; ++column) {
                pixels[static_cast<std::size_t>(
                    row * matrix.source_width + column)] = formal::source_value(
                        row + static_cast<std::int32_t>(frame * 3U),
                        column + static_cast<std::int32_t>(frame * 5U));
            }
        }
    }
    return result;
}

[[nodiscard]] std::string frame_ring_fingerprint(
    const std::vector<std::vector<float>> &ring) {
    std::uint64_t hash = 1469598103934665603ULL;
    formal::fnv1a64_append(hash, "prepared-once-frame-ring-v1");
    for (const auto &frame : ring) {
        for (const float value : frame) {
            formal::fnv1a64_append(
                hash, std::bit_cast<std::uint32_t>(value), 4U);
        }
    }
    return formal::hexadecimal_u64(hash);
}

struct LockedRecipe {
    std::shared_ptr<const getnative::AxisPlan> plan;
    std::vector<getnative::CandidateAnalysis> candidates;
};

[[nodiscard]] LockedRecipe make_locked_recipe(
    const formal::Matrix &matrix, const formal::Case &primary) {
    const std::size_t middle = static_cast<std::size_t>(matrix.candidate_count / 2);
    const auto point = formal::candidate_point(matrix, primary, middle);
    getnative::AxisPlanCache cache;
    auto plan = cache.get_or_build({
        matrix.source_height,
        point.native_height,
        point.active_height,
        0.0,
        primary.filter,
        getnative::BorderMode::mirror,
    });
    LockedRecipe result{};
    result.plan = plan;
    result.candidates.push_back({
        "locked-recipe", nullptr, std::move(plan),
        getnative::AnalysisAxes::vertical,
    });
    return result;
}

[[nodiscard]] getnative::ConstImageView frame_view(
    const std::vector<float> &frame, const formal::Matrix &matrix) noexcept {
    return {
        frame.data(), matrix.source_width, matrix.source_height,
        matrix.source_width,
    };
}

[[nodiscard]] std::uint64_t run_cpu_frames_serial(
    const formal::Matrix &matrix,
    const std::vector<std::vector<float>> &ring,
    const LockedRecipe &recipe,
    std::size_t frame_count) {
    std::uint64_t result = 0U;
    getnative::CpuWorkspace workspace;
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        const double metric = getnative::analyze_axis_candidate_f32(
            frame_view(ring[frame % ring.size()], matrix),
            *recipe.plan, getnative::AnalysisAxes::vertical,
            matrix.metric, workspace);
        result ^= std::bit_cast<std::uint64_t>(metric);
    }
    return result;
}

[[nodiscard]] std::uint64_t run_cpu_frames_parallel(
    const formal::Matrix &matrix,
    const std::vector<std::vector<float>> &ring,
    const LockedRecipe &recipe,
    std::size_t frame_count,
    std::size_t worker_limit) {
    std::vector<double> values(frame_count);
    std::atomic<std::size_t> cursor{0U};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const std::size_t workers = std::max<std::size_t>(
        1U, std::min(frame_count, worker_limit));
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t worker = 0U; worker < workers; ++worker) {
        threads.emplace_back([&]() {
            getnative::CpuWorkspace workspace;
            try {
                while (true) {
                    const std::size_t frame = cursor.fetch_add(
                        1U, std::memory_order_relaxed);
                    if (frame >= frame_count) break;
                    values[frame] = getnative::analyze_axis_candidate_f32(
                        frame_view(ring[frame % ring.size()], matrix),
                        *recipe.plan, getnative::AnalysisAxes::vertical,
                        matrix.metric, workspace);
                }
            } catch (...) {
                std::lock_guard lock{failure_mutex};
                if (!failure) failure = std::current_exception();
                cursor.store(frame_count, std::memory_order_relaxed);
            }
        });
    }
    threads.clear();
    if (failure) std::rethrow_exception(failure);
    std::uint64_t result = 0U;
    for (const double value : values) {
        result ^= std::bit_cast<std::uint64_t>(value);
    }
    return result;
}

[[nodiscard]] std::uint64_t run_cuda_frames_serial(
    const formal::Matrix &matrix,
    const std::vector<std::vector<float>> &ring,
    const LockedRecipe &recipe,
    std::size_t frame_count,
    getnative::CudaAnalysisEngine &engine) {
    std::uint64_t result = 0U;
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        const auto values = engine.analyze_axis_batch_f32(
            frame_view(ring[frame % ring.size()], matrix),
            recipe.candidates, matrix.metric);
        result ^= checksum(values);
    }
    return result;
}

[[nodiscard]] bool validate_multiframe_ring(
    const formal::Matrix &matrix,
    const std::vector<std::vector<float>> &ring,
    const LockedRecipe &recipe,
    getnative::CudaAnalysisEngine &engine) {
    for (const auto &frame : ring) {
        getnative::CpuWorkspace workspace;
        const double cpu = getnative::analyze_axis_candidate_f32(
            frame_view(frame, matrix), *recipe.plan,
            getnative::AnalysisAxes::vertical, matrix.metric, workspace);
        const auto gpu = engine.analyze_axis_batch_f32(
            frame_view(frame, matrix), recipe.candidates, matrix.metric);
        if (gpu.size() != 1U || !std::isfinite(gpu.front().error)) return false;
        const double tolerance = std::max(
            1e-7, relative_tolerance() * std::abs(cpu));
        if (std::abs(gpu.front().error - cpu) > tolerance) return false;
    }
    return true;
}

[[nodiscard]] MultiframeMeasurement measure_multiframe(
    const Configuration &config,
    const formal::Matrix &matrix,
    const formal::Case &primary,
    getnative::CudaAnalysisEngine &engine) {
    MultiframeMeasurement result{};
    if (config.skip_multiframe) return result;
    const auto ring = make_frame_ring(matrix);
    const auto recipe = make_locked_recipe(matrix, primary);
    result.measured = true;
    result.ring_size = ring.size();
    result.worker_limit = std::max<std::size_t>(
        1U, std::min<std::size_t>(8U, std::thread::hardware_concurrency()));
    result.ring_fingerprint = frame_ring_fingerprint(ring);
    const bool ring_correct = validate_multiframe_ring(
        matrix, ring, recipe, engine);

    for (const std::size_t frame_count : multiframe_counts) {
        std::vector<double> cpu_serial_raw;
        std::vector<double> cpu_parallel_raw;
        std::vector<double> cuda_raw;
        cpu_serial_raw.reserve(config.multiframe_samples);
        cpu_parallel_raw.reserve(config.multiframe_samples);
        cuda_raw.reserve(config.multiframe_samples);

        (void)run_cpu_frames_serial(matrix, ring, recipe, 1U);
        (void)run_cpu_frames_parallel(
            matrix, ring, recipe, 1U, result.worker_limit);
        (void)run_cuda_frames_serial(matrix, ring, recipe, 1U, engine);

        for (std::size_t sample = 0U;
             sample < config.multiframe_samples; ++sample) {
            const auto cpu_serial = [&]() {
                cpu_serial_raw.push_back(elapsed_ms([&]() {
                    return run_cpu_frames_serial(
                        matrix, ring, recipe, frame_count);
                }));
            };
            const auto cpu_parallel = [&]() {
                cpu_parallel_raw.push_back(elapsed_ms([&]() {
                    return run_cpu_frames_parallel(
                        matrix, ring, recipe, frame_count, result.worker_limit);
                }));
            };
            const auto cuda_serial = [&]() {
                cuda_raw.push_back(elapsed_ms([&]() {
                    return run_cuda_frames_serial(
                        matrix, ring, recipe, frame_count, engine);
                }));
            };
            if ((sample & 1U) == 0U) {
                cpu_serial();
                cpu_parallel();
                cuda_serial();
            } else {
                cuda_serial();
                cpu_parallel();
                cpu_serial();
            }
        }

        MultiframeCase measured{};
        measured.frame_count = frame_count;
        measured.cpu_serial_ms = getnative::benchmark::summarize(
            std::move(cpu_serial_raw));
        measured.cpu_parallel_ms = getnative::benchmark::summarize(
            std::move(cpu_parallel_raw));
        measured.cuda_serial_ms = getnative::benchmark::summarize(
            std::move(cuda_raw));
        measured.cpu_serial_frames_per_second =
            static_cast<double>(frame_count) * 1000.0
            / measured.cpu_serial_ms.median;
        measured.cpu_parallel_frames_per_second =
            static_cast<double>(frame_count) * 1000.0
            / measured.cpu_parallel_ms.median;
        measured.cuda_frames_per_second =
            static_cast<double>(frame_count) * 1000.0
            / measured.cuda_serial_ms.median;
        measured.cuda_speedup_vs_cpu_serial =
            measured.cpu_serial_ms.median / measured.cuda_serial_ms.median;
        measured.correct = ring_correct;
        result.cases.push_back(std::move(measured));
    }
    return result;
}

[[nodiscard]] bool correctness_passes(const Correctness &value) noexcept {
    const std::size_t required_overlap = value.top_k == 0U
        ? 0U : value.top_k - 1U;
    return value.ids_and_order && value.finite && value.within_tolerance
        && value.valley_distance <= 1U
        && value.top_k_overlap >= required_overlap;
}

[[nodiscard]] bool case_passes(const CaseMeasurement &measurement) {
    if (measurement.cpu_execution_raw.empty()
        || measurement.cpu_end_to_end_raw.empty()) {
        return false;
    }
    return std::all_of(
        measurement.variants.begin(), measurement.variants.end(),
        [](const VariantCaseMeasurement &variant) {
            const auto &telemetry = variant.final_telemetry;
            return correctness_passes(variant.correctness)
                && variant.stable_bits && variant.bit_identical_to_generic
                && telemetry.stream_submission_count
                    == telemetry.stream_completion_count
                && telemetry.total_peak_explicit_bytes
                    < 2ULL * 1024ULL * 1024ULL * 1024ULL;
        });
}

void append_phase_json(std::ostream &output, const PhaseSamples &phases) {
    const auto append = [&](std::string_view name,
                            const std::vector<double> &raw,
                            bool first = false) {
        if (!first) output << ',';
        output << getnative::benchmark::json_string(name) << ':';
        getnative::benchmark::append_summary(
            output, getnative::benchmark::summarize(raw));
    };
    output << '{';
    append("backend_wall_ms", phases.backend_wall_ms, true);
    append("plan_pack_ms", phases.plan_pack_ms);
    append("source_upload_ms", phases.source_upload_ms);
    append("plan_upload_ms", phases.plan_upload_ms);
    append("inverse_h_ms", phases.inverse_h_ms);
    append("inverse_v_ms", phases.inverse_v_ms);
    append("first_forward_ms", phases.first_forward_ms);
    append("metric_ms", phases.metric_ms);
    append("gpu_execution_ms", phases.gpu_execution_ms);
    append("partial_readback_ms", phases.partial_readback_ms);
    append("cpu_merge_ms", phases.cpu_merge_ms);
    append("allocation_ms", phases.allocation_ms);
    append("allocation_count", phases.allocation_count);
    append("working_reuse_count", phases.working_reuse_count);
    output << '}';
}

void append_case_identity(
    std::ostream &output, const formal::Matrix &matrix,
    const CaseMeasurement &measurement) {
    const auto &benchmark_case = measurement.benchmark_case;
    const auto first = formal::candidate_point(matrix, benchmark_case, 0U);
    const auto last = formal::candidate_point(
        matrix, benchmark_case,
        static_cast<std::size_t>(matrix.candidate_count - 1));
    output << "\"id\":"
           << getnative::benchmark::json_string(benchmark_case.id)
           << ",\"filter_id\":"
           << getnative::benchmark::json_string(benchmark_case.filter_id)
           << ",\"native_height\":" << benchmark_case.native_height
           << ",\"fractional_scan\":"
           << (benchmark_case.fractional_scan ? "true" : "false")
           << ",\"primary\":" << (benchmark_case.primary ? "true" : "false")
           << ",\"candidate_count\":" << matrix.candidate_count
           << ",\"first_candidate\":{\"native_height\":"
           << first.native_height << ",\"active_height\":"
           << std::setprecision(17) << first.active_height << "}"
           << ",\"last_candidate\":{\"native_height\":"
           << last.native_height << ",\"active_height\":"
           << last.active_height << "}"
           << ",\"candidate_contract_fnv1a64\":"
           << getnative::benchmark::json_string(
                measurement.candidate_fingerprint);
}

[[nodiscard]] std::string make_json(
    const Configuration &config,
    const formal::Matrix &matrix,
    const std::vector<float> &source_pixels,
    const std::vector<formal::Case> &all_cases,
    const std::vector<CaseMeasurement> &measurements,
    const std::vector<EngineMeasurement> &engines,
    const MultiframeMeasurement &multiframe,
    bool formal_matrix_selected,
    bool assertions_pass,
    int argc,
    char **argv) {
    const auto &device = engines.front().engine->device_info();
    const auto telemetry = engines.front().engine->runtime_telemetry();
    const auto cpu = getnative::cpu_dispatch_info();
    const bool matrix_complete = measurements.size() == all_cases.size()
        && std::equal(
            measurements.begin(), measurements.end(), all_cases.begin(),
            [](const CaseMeasurement &actual, const formal::Case &expected) {
                return actual.benchmark_case.id == expected.id;
            });
    const bool multiframe_complete = multiframe.measured
        && multiframe.cases.size() == multiframe_counts.size()
        && std::all_of(
            multiframe.cases.begin(), multiframe.cases.end(),
            [&](const MultiframeCase &value) {
                return value.correct
                    && value.cpu_serial_ms.raw.size() == config.multiframe_samples
                    && value.cpu_parallel_ms.raw.size() == config.multiframe_samples
                    && value.cuda_serial_ms.raw.size() == config.multiframe_samples;
            });
    const bool formal_completion = formal_matrix_selected && matrix_complete
        && config.samples >= 21U && config.multiframe_samples >= 21U
        && multiframe_complete && assertions_pass;

    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_cuda_benchmark", "batch-cache",
        "synthetic-from-metal-kernel-matrix-v1", argc, argv);
    output << ",\"matrix\":{\"path\":"
           << getnative::benchmark::json_string(config.matrix_path.string())
           << ",\"fnv1a64\":"
           << getnative::benchmark::json_string(
                getnative::benchmark::fnv1a64_file(config.matrix_path))
           << ",\"source_width\":" << matrix.source_width
           << ",\"source_height\":" << matrix.source_height
           << ",\"candidate_count\":" << matrix.candidate_count
           << ",\"expected_case_count\":" << formal::matrix_case_count
           << ",\"selected_case_count\":" << measurements.size()
           << ",\"primary_case_id\":"
           << getnative::benchmark::json_string(formal::primary_case_id)
           << ",\"formal_definition\":"
           << (formal::is_formal_matrix(matrix, all_cases) ? "true" : "false")
           << ",\"formal_matrix_selected\":"
           << (formal_matrix_selected ? "true" : "false")
           << ",\"matrix_complete\":" << (matrix_complete ? "true" : "false")
           << ",\"formal_completion\":"
           << (formal_completion ? "true" : "false")
           << ",\"custom_or_diagnostic\":"
           << (formal_matrix_selected ? "false" : "true") << '}';
    output << ",\"source_fixture\":{\"kind\":\"deterministic-synthetic-v1\""
           << ",\"decoded_float32_fnv1a64\":"
           << getnative::benchmark::json_string(
                formal::source_f32_fnv1a64(source_pixels)) << '}';
    output << ",\"candidate_contract\":{\"id\":"
           << getnative::benchmark::json_string(formal::candidate_contract_id)
           << ",\"named_height_formula\":"
              "\"native + (index + 1) / (candidate_count + 1)\""
           << ",\"fractional_scan_formula\":"
              "\"inclusive 800.0..899.9; native=clamp(floor(active),800,899)\"}"
           << ",\"execution\":{\"sample_count\":" << config.samples
           << ",\"warmup_count_per_stage\":1"
           << ",\"sample_order\":\"alternating CPU/variant order; no concurrent benchmark process\""
           << ",\"tile_size\":" << matrix.tile_size
           << ",\"reduction_groups\":" << matrix.reduction_groups
           << ",\"inverse_threads\":" << matrix.inverse_threads << '}';
    output << ",\"cpu_denominator\":{\"isa\":"
           << getnative::benchmark::json_string(
                getnative::cpu_isa_name(cpu.selected))
           << ",\"math_mode\":\"production\",\"selection_reason\":"
           << getnative::benchmark::json_string(cpu.selection_reason)
           << ",\"vendor\":"
           << getnative::benchmark::json_string(
                getnative::cpu_vendor(cpu.snapshot))
           << ",\"family\":" << cpu.snapshot.family
           << ",\"model\":" << cpu.snapshot.model
           << ",\"stepping\":" << cpu.snapshot.stepping
           << ",\"same_invocation\":true}"
           << ",\"device\":{\"ordinal\":" << device.ordinal
           << ",\"name\":" << getnative::benchmark::json_string(device.name)
           << ",\"uuid\":" << getnative::benchmark::json_string(device.uuid)
           << ",\"compute_capability\":"
           << getnative::benchmark::json_string(
                std::to_string(device.compute_capability_major) + "."
                + std::to_string(device.compute_capability_minor))
           << ",\"driver_version\":" << device.driver_version
           << ",\"total_memory_bytes\":" << device.total_memory_bytes << '}';
    output << ",\"math\":{\"path\":\"production\""
           << ",\"relative_tolerance\":"
           << std::setprecision(17) << relative_tolerance()
           << ",\"nvcc_flags\":"
           << getnative::benchmark::json_string(telemetry.module_compile_flags)
           << ",\"toolkit_version\":"
           << getnative::benchmark::json_string(GETNATIVE_CUDA_TOOLKIT_VERSION)
           << ",\"fma_allowed\":true"
           << ",\"multi_math_mode\":false}"
           << ",\"fatbin\":{\"path\":"
           << getnative::benchmark::json_string(config.fatbin_path->string())
           << ",\"fnv1a64\":"
           << getnative::benchmark::json_string(
                getnative::benchmark::fnv1a64_file(*config.fatbin_path)) << '}';

    output << ",\"engine_initialization\":[";
    for (std::size_t index = 0U; index < engines.size(); ++index) {
        if (index != 0U) output << ',';
        const auto &engine = engines[index];
        const auto engine_telemetry = engine.engine->runtime_telemetry();
        output << "{\"variant\":"
               << getnative::benchmark::json_string(
                    getnative::cuda_kernel_variant_name(engine.variant))
               << ",\"cold_constructor_wall_ms\":"
               << engine.cold_constructor_wall_ms
               << ",\"cold_first_prepared_frame_wall_ms\":"
               << engine.cold_first_prepared_frame_wall_ms
               << ",\"cold_module_load_ms\":" << engine.cold_module_load_ms
               << ",\"warm_constructor_wall_ms\":"
               << engine.warm_constructor_wall_ms
               << ",\"warm_module_load_ms\":" << engine.warm_module_load_ms
               << ",\"artifact_name\":"
               << getnative::benchmark::json_string(
                    engine_telemetry.module_artifact_name)
               << ",\"artifact_bytes\":"
               << engine_telemetry.module_artifact_bytes
               << ",\"artifact_fnv1a64\":"
               << getnative::benchmark::json_string(
                    engine_telemetry.module_artifact_hash_fnv1a64)
               << ",\"module_path_provenance\":"
               << getnative::benchmark::json_string(
                    engine_telemetry.module_path_provenance)
               << ",\"ptx_jit_forced\":"
               << (engine_telemetry.ptx_jit_forced ? "true" : "false")
               << ",\"binary_version\":"
               << engine_telemetry.module_binary_version
               << ",\"ptx_version\":" << engine_telemetry.module_ptx_version
               << ",\"maximum_registers_per_thread\":"
               << engine_telemetry.maximum_kernel_register_count
               << ",\"maximum_static_shared_bytes\":"
               << engine_telemetry.maximum_kernel_static_shared_bytes << '}';
    }
    output << ']';

    output << ",\"cases\":[";
    for (std::size_t case_index = 0U;
         case_index < measurements.size(); ++case_index) {
        if (case_index != 0U) output << ',';
        const auto &measurement = measurements[case_index];
        output << '{';
        append_case_identity(output, matrix, measurement);
        output << ",\"planner\":{\"cold_ms\":";
        getnative::benchmark::append_summary(
            output, measurement.planner.cold_ms);
        output << ",\"warm_session_cache_ms\":";
        getnative::benchmark::append_summary(
            output, measurement.planner.warm_session_cache_ms);
        output << "},\"cpu\":{\"complete_execution_ms\":";
        const auto cpu_execution = getnative::benchmark::summarize(
            measurement.cpu_execution_raw);
        const auto cpu_end_to_end = getnative::benchmark::summarize(
            measurement.cpu_end_to_end_raw);
        getnative::benchmark::append_summary(output, cpu_execution);
        output << ",\"end_to_end_ms\":";
        getnative::benchmark::append_summary(output, cpu_end_to_end);
        output << "},\"cuda_variants\":[";
        for (std::size_t variant_index = 0U;
             variant_index < measurement.variants.size(); ++variant_index) {
            if (variant_index != 0U) output << ',';
            const auto &variant = measurement.variants[variant_index];
            const auto backend = getnative::benchmark::summarize(
                variant.phases.backend_wall_ms);
            const auto gpu_execution = getnative::benchmark::summarize(
                variant.phases.gpu_execution_ms);
            const auto end_to_end = getnative::benchmark::summarize(
                variant.end_to_end_raw);
            const auto &variant_telemetry = variant.final_telemetry;
            output << "{\"variant\":"
                   << getnative::benchmark::json_string(
                        getnative::cuda_kernel_variant_name(variant.variant))
                   << ",\"phases\":";
            append_phase_json(output, variant.phases);
            output << ",\"end_to_end_ms\":";
            getnative::benchmark::append_summary(output, end_to_end);
            output << ",\"speedup_vs_same_invocation_cpu_end_to_end\":"
                   << cpu_end_to_end.median / end_to_end.median
                   << ",\"backend_speedup_vs_cpu_complete_execution\":"
                   << cpu_execution.median / backend.median
                   << ",\"gpu_phase_speedup_vs_cpu_complete_execution\":"
                   << cpu_execution.median / gpu_execution.median
                   << ",\"correctness\":{\"maximum_absolute_error\":"
                   << variant.correctness.maximum_absolute_error
                   << ",\"maximum_relative_error\":"
                   << variant.correctness.maximum_relative_error
                   << ",\"valley_distance\":"
                   << variant.correctness.valley_distance
                   << ",\"top_k\":" << variant.correctness.top_k
                   << ",\"top_k_overlap\":"
                   << variant.correctness.top_k_overlap
                   << ",\"ids_and_order\":"
                   << (variant.correctness.ids_and_order ? "true" : "false")
                   << ",\"finite\":"
                   << (variant.correctness.finite ? "true" : "false")
                   << ",\"within_tolerance\":"
                   << (variant.correctness.within_tolerance ? "true" : "false")
                   << ",\"stable_bits_across_samples\":"
                   << (variant.stable_bits ? "true" : "false")
                   << ",\"generic_comparison_measured\":"
                   << (variant.generic_comparison_measured ? "true" : "false")
                   << ",\"bit_identical_to_cpp_generic\":"
                   << (variant.bit_identical_to_generic ? "true" : "false")
                   << "},\"memory\":{\"working_active_bytes\":"
                   << variant_telemetry.working_buffer_active_bytes
                   << ",\"working_retained_bytes\":"
                   << variant_telemetry.working_buffer_retained_bytes
                   << ",\"working_peak_active_bytes\":"
                   << variant_telemetry.working_buffer_peak_active_bytes
                   << ",\"queued_plan_peak_bytes\":"
                   << variant_telemetry.queued_plan_peak_bytes
                   << ",\"total_peak_explicit_bytes\":"
                   << variant_telemetry.total_peak_explicit_bytes
                   << "},\"dispatch\":{\"analyzed_tiles\":"
                   << variant_telemetry.analyzed_tile_count
                   << ",\"generic_tiles\":"
                   << variant_telemetry.generic_tile_count
                   << ",\"specialized_tiles\":"
                   << variant_telemetry.specialized_tile_count
                   << ",\"stream_submissions\":"
                   << variant_telemetry.stream_submission_count
                   << ",\"stream_completions\":"
                   << variant_telemetry.stream_completion_count << "}}";
        }
        output << "]}";
    }
    output << ']';

    output << ",\"prepared_once_multiframe\":{\"measured\":"
           << (multiframe.measured ? "true" : "false")
           << ",\"sample_count\":" << config.multiframe_samples
           << ",\"ring_size\":" << multiframe.ring_size
           << ",\"ring_fingerprint\":"
           << getnative::benchmark::json_string(multiframe.ring_fingerprint)
           << ",\"worker_limit\":" << multiframe.worker_limit
           << ",\"logical_frame_counts\":[1,2,10,100,1000]"
           << ",\"cases\":[";
    for (std::size_t index = 0U; index < multiframe.cases.size(); ++index) {
        if (index != 0U) output << ',';
        const auto &value = multiframe.cases[index];
        output << "{\"frames\":" << value.frame_count
               << ",\"cpu_serial_ms\":";
        getnative::benchmark::append_summary(output, value.cpu_serial_ms);
        output << ",\"cpu_parallel_ms\":";
        getnative::benchmark::append_summary(output, value.cpu_parallel_ms);
        output << ",\"cuda_serial_ms\":";
        getnative::benchmark::append_summary(output, value.cuda_serial_ms);
        output << ",\"cpu_serial_frames_per_second\":"
               << value.cpu_serial_frames_per_second
               << ",\"cpu_parallel_frames_per_second\":"
               << value.cpu_parallel_frames_per_second
               << ",\"cuda_frames_per_second\":"
               << value.cuda_frames_per_second
               << ",\"cuda_speedup_vs_cpu_serial\":"
               << value.cuda_speedup_vs_cpu_serial
               << ",\"correct\":" << (value.correct ? "true" : "false")
               << '}';
    }
    output << "]}"
           << ",\"external_profiling\":{\"compute_sanitizer\":"
              "\"separate-serialized-artifact\",\"nsight_systems\":"
              "\"separate-serialized-artifact\",\"nsight_compute\":"
              "\"separate-serialized-artifact\",\"fabricated_metrics\":false}"
           << ",\"decision\":{\"default_enable_requires_speedup\":3.0"
           << ",\"working_set_limit_bytes\":2147483648"
           << ",\"benchmark_does_not_auto_approve\":true}"
           << ",\"correctness\":{\"assertions_requested\":"
           << (config.assert_correctness ? "true" : "false")
           << ",\"assertions_pass\":" << (assertions_pass ? "true" : "false")
           << "}}\n";
    return output.str();
}

} // namespace

int main(int argc, char **argv) {
    try {
        Configuration config = parse_arguments(argc, argv);
        const formal::Matrix matrix = formal::load_matrix(config.matrix_path);
        const std::vector<formal::Case> all_cases = formal::make_cases(matrix);
        const std::vector<formal::Case> selected_cases = formal::make_cases(
            matrix, config.selected_case);
        const bool formal_matrix_selected = !config.selected_case
            && !config.profile_mode && !config.skip_multiframe
            && formal::is_formal_matrix(matrix, all_cases)
            && selected_cases.size() == formal::matrix_case_count;
        if (formal_matrix_selected
            && (config.samples < 21U || config.multiframe_samples < 21U)) {
            throw std::invalid_argument(
                "formal CUDA benchmark requires at least 21 samples per stage");
        }
        if (config.list_cases) {
            std::cout << "full_case_count=" << all_cases.size()
                      << " selected_case_count=" << selected_cases.size()
                      << " formal_matrix_selected="
                      << (formal_matrix_selected ? "true" : "false") << '\n';
            for (const auto &benchmark_case : selected_cases) {
                std::cout << "case=" << benchmark_case.id
                          << " filter=" << benchmark_case.filter_id
                          << " native_height=" << benchmark_case.native_height
                          << " fractional_scan="
                          << (benchmark_case.fractional_scan ? "true" : "false")
                          << " primary="
                          << (benchmark_case.primary ? "true" : "false") << '\n';
            }
            return EXIT_SUCCESS;
        }

        if (!config.fatbin_path) {
            config.fatbin_path = default_fatbin_path();
        }
        if (config.fatbin_path->empty()
            || !std::filesystem::is_regular_file(*config.fatbin_path)) {
            throw std::runtime_error(
                "selected CUDA fatbin evidence path is unavailable");
        }
        const auto probe = getnative::cuda_runtime_probe();
        if (!probe.device_available) {
            throw std::runtime_error("CUDA device is unavailable: " + probe.reason);
        }

        const std::vector<float> source_pixels = formal::make_source(matrix);
        const getnative::ConstImageView source{
            source_pixels.data(), matrix.source_width, matrix.source_height,
            matrix.source_width,
        };
        const auto primary_iterator = std::find_if(
            all_cases.begin(), all_cases.end(),
            [](const formal::Case &value) { return value.primary; });
        if (primary_iterator == all_cases.end()) {
            throw std::runtime_error("formal primary case is unavailable");
        }
        const formal::PreparedCase primary_prepared = formal::prepare_case(
            matrix, *primary_iterator);
        auto engines = create_engines(
            config, matrix, source, primary_prepared);
        if (engines.empty()) throw std::runtime_error("no CUDA variant selected");

        std::cout << "CUDA benchmark device="
                  << engines.front().engine->device_info().name
                  << " path=production"
                  << " cases=" << selected_cases.size()
                  << " samples=" << config.samples
                  << " formal="
                  << (formal_matrix_selected ? "true" : "false") << '\n';

        std::vector<CaseMeasurement> measurements;
        measurements.reserve(selected_cases.size());
        for (std::size_t index = 0U; index < selected_cases.size(); ++index) {
            const auto &benchmark_case = selected_cases[index];
            std::cout << "measuring case=" << benchmark_case.id
                      << " index=" << index + 1U << '/' << selected_cases.size()
                      << '\n' << std::flush;
            const formal::PreparedCase prepared = formal::prepare_case(
                matrix, benchmark_case);
            measurements.push_back(measure_case(
                config, matrix, source, benchmark_case, prepared, engines));
        }

        MultiframeMeasurement multiframe = measure_multiframe(
            config, matrix, *primary_iterator, *engines.front().engine);
        bool assertions_pass = std::all_of(
            measurements.begin(), measurements.end(), case_passes);
        if (multiframe.measured) {
            assertions_pass = assertions_pass
                && std::all_of(
                    multiframe.cases.begin(), multiframe.cases.end(),
                    [](const MultiframeCase &value) { return value.correct; });
        }
        for (const auto &engine : engines) {
            const auto telemetry = engine.engine->runtime_telemetry();
            assertions_pass = assertions_pass
                && telemetry.module_artifact_name == "getnative_cuda.fatbin"
                && telemetry.module_artifact_hash_fnv1a64
                    == getnative::benchmark::fnv1a64_file(*config.fatbin_path);
        }

        const std::string json = make_json(
            config, matrix, source_pixels, all_cases, measurements, engines,
            multiframe, formal_matrix_selected, assertions_pass, argc, argv);
        std::filesystem::path output_path;
        if (config.json_output) {
            output_path = *config.json_output;
        } else {
            std::filesystem::create_directories(*config.artifact_root);
            output_path = *config.artifact_root / "cuda-benchmark-results.json";
        }
        getnative::benchmark::atomic_write_json(output_path, json);
        std::cout << "artifact=" << output_path.string()
                  << " assertions_pass="
                  << (assertions_pass ? "true" : "false") << '\n';
        if (config.assert_correctness && !assertions_pass) return EXIT_FAILURE;
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "CUDA benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

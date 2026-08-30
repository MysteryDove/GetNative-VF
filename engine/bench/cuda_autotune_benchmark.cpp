#include "benchmark_support.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/cuda_analysis.hpp"
#include "getnative/joining_thread.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>
#endif

#if defined(__linux__)
extern char **environ;
#endif

namespace {

constexpr std::string_view tool_name = "getnative-cuda-autotune";
constexpr std::string_view tool_version = "1.4.0";
constexpr std::string_view workload_contract_version = "descale-launch-v5";
constexpr double minimum_composite_gain = 0.03;
constexpr double maximum_workload_regression = 0.03;
constexpr double minimum_precondition_seconds = 3.0;
constexpr std::size_t screening_minimum_iterations_per_worker = 4U;
constexpr std::size_t confirmation_minimum_iterations_per_worker = 8U;
constexpr std::size_t calibration_probe_iterations_per_worker = 4U;
constexpr std::uint32_t search_seed = 0x474E4154U;
constexpr std::size_t standard_maximum_confirmation_finalists = 4U;
constexpr std::size_t survey_maximum_confirmation_finalists = 2U;
constexpr std::array<std::int32_t, 4> target_compute_capabilities{
    75, 86, 89, 120,
};

constexpr getnative::CudaLaunchPolicy baseline_policy =
    getnative::cuda_default_launch_policy;

enum class RunMode : std::uint8_t {
    survey,
    quick,
    standard,
};

struct Configuration {
    RunMode mode = RunMode::survey;
    bool no_open = false;
    bool no_pause = false;
    bool help = false;
    std::optional<std::int32_t> device_ordinal;
    std::optional<std::filesystem::path> output_path;
};

[[nodiscard]] constexpr std::string_view run_mode_name(RunMode mode) noexcept {
    switch (mode) {
    case RunMode::survey: return "survey";
    case RunMode::quick: return "quick";
    case RunMode::standard: return "standard";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool is_quick(RunMode mode) noexcept {
    return mode == RunMode::quick;
}

[[nodiscard]] constexpr bool is_standard(RunMode mode) noexcept {
    return mode == RunMode::standard;
}

[[nodiscard]] constexpr std::size_t maximum_confirmation_finalists(
    RunMode mode) noexcept {
    if (mode == RunMode::quick) return 1U;
    if (mode == RunMode::survey) return survey_maximum_confirmation_finalists;
    return standard_maximum_confirmation_finalists;
}

[[nodiscard]] constexpr bool target_architecture_supported(
    const getnative::CudaDeviceInfo &device) noexcept {
    const std::int32_t architecture =
        10 * device.compute_capability_major + device.compute_capability_minor;
    return std::find(
               target_compute_capabilities.begin(),
               target_compute_capabilities.end(), architecture)
        != target_compute_capabilities.end();
}

enum class FilterFamily : std::uint8_t {
    bilinear,
    bicubic,
    lanczos3,
    spline64,
    lanczos8,
    mixed_self_test,
};

constexpr std::array<FilterFamily, 5> benchmark_filter_families{
    FilterFamily::bilinear,
    FilterFamily::bicubic,
    FilterFamily::lanczos3,
    FilterFamily::spline64,
    FilterFamily::lanczos8,
};

struct WorkloadSpec {
    std::string id;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t native_width = 0;
    std::int32_t native_height = 0;
    FilterFamily filter_family = FilterFamily::bicubic;
    getnative::AnalysisAxes axes = getnative::AnalysisAxes::both;
    std::size_t candidate_count = 0;
    std::size_t concurrency = 1;
    std::size_t iterations_per_worker = 1;
    double weight = 0.0;
};

struct Fixture {
    WorkloadSpec spec;
    std::size_t iteration_limit_per_worker = 1U;
    std::vector<float> pixels;
    getnative::ConstImageView source;
    std::vector<getnative::CandidateAnalysis> candidates;
    getnative::MetricSpec metric;
};

struct ValidationStats {
    double maximum_absolute_error = 0.0;
    double maximum_tolerance_ratio = 0.0;
    std::size_t comparison_count = 0;
};

struct ProfileTotals {
    std::size_t measured_frames = 0;
    std::size_t kernel_launch_count = 0;
    std::size_t tile_count = 0;
    std::size_t buffer_allocation_count = 0;
    std::size_t plan_cache_hits = 0;
    std::size_t plan_cache_misses = 0;
    std::size_t source_upload_bytes = 0;
    std::size_t plan_upload_bytes = 0;
    std::size_t result_readback_bytes = 0;
    std::size_t peak_working_set_bytes = 0;
    std::size_t workspace_bytes = 0;
    std::size_t pinned_staging_bytes = 0;
    double execution_slot_wait_ms = 0.0;
    double host_pack_ms = 0.0;
    double source_staging_ms = 0.0;
    double source_upload_ms = 0.0;
    double plan_upload_ms = 0.0;
    double source_transpose_ms = 0.0;
    double horizontal_fused_ms = 0.0;
    double inverse_horizontal_ms = 0.0;
    double inverse_vertical_ms = 0.0;
    double forward_intermediate_ms = 0.0;
    double metric_ms = 0.0;
    double kernel_ms = 0.0;
    double result_readback_ms = 0.0;
    double gpu_total_ms = 0.0;
};

struct WorkloadEvaluation {
    WorkloadSpec spec;
    getnative::benchmark::Summary frame_ms;
    double frames_per_second = 0.0;
    double p95_frame_ms = 0.0;
    ValidationStats validation;
    ProfileTotals profile;
    bool has_comparison = false;
    double improvement_vs_baseline = 0.0;
};

struct FamilyComparison {
    FilterFamily family = FilterFamily::bicubic;
    double composite_improvement = 0.0;
    double worst_workload_improvement = 0.0;
    double maximum_relative_mad = 0.0;
    double required_gain = minimum_composite_gain;
    bool decision_admissible = false;
};

struct PolicyEvaluation {
    getnative::CudaLaunchPolicy policy;
    std::string id;
    bool valid = false;
    std::string failure;
    double elapsed_seconds = 0.0;
    std::vector<WorkloadEvaluation> workloads;
    bool has_comparison = false;
    double composite_improvement = 0.0;
    double worst_workload_improvement = 0.0;
    double maximum_relative_mad = 0.0;
    double required_gain = minimum_composite_gain;
    bool decision_admissible = false;
    std::vector<FamilyComparison> family_comparisons;
};

struct SelfTestResult {
    bool passed = false;
    bool repeat_bitwise_stable = false;
    bool resource_gate_passed = false;
    ValidationStats validation;
    getnative::CudaRuntimeTelemetry telemetry;
};

struct Recommendation {
    std::string decision = "retain_baseline";
    std::string selected_policy_id;
    std::string provisional_best_policy_id;
    double composite_improvement = 0.0;
    double worst_workload_improvement = 0.0;
    double required_gain = minimum_composite_gain;
    double baseline_maximum_drift = 0.0;
    std::string confidence = "low";
    std::string rationale;
    bool safe_to_apply = false;
};

struct FamilyRecommendation {
    FilterFamily family = FilterFamily::bicubic;
    std::string decision = "retain_baseline";
    std::string selected_policy_id;
    std::string provisional_best_policy_id;
    double composite_improvement = 0.0;
    double worst_workload_improvement = 0.0;
    double maximum_relative_mad = 0.0;
    double required_gain = minimum_composite_gain;
    std::string headline_workload_id;
    double baseline_frames_per_second = 0.0;
    double selected_frames_per_second = 0.0;
    double provisional_best_frames_per_second = 0.0;
    std::string confidence = "low";
    std::string rationale;
    bool benchmark_winner = false;
    bool production_dispatch_available = false;
};

struct ConfirmationResult {
    PolicyEvaluation baseline_before;
    PolicyEvaluation baseline_after;
    PolicyEvaluation baseline_aggregate;
    std::vector<PolicyEvaluation> finalists;
};

struct DeviceTuningResult {
    getnative::CudaDeviceInfo device;
    std::string status = "failed";
    std::string error;
    double elapsed_seconds = 0.0;
    double precondition_seconds = 0.0;
    SelfTestResult self_test;
    std::vector<PolicyEvaluation> screening;
    ConfirmationResult confirmation;
    Recommendation recommendation;
    std::vector<FamilyRecommendation> family_recommendations;
};

struct RunReport {
    Configuration config;
    getnative::CudaRuntimeProbe probe;
    std::vector<DeviceTuningResult> devices;
    std::string executable_hash_fnv1a64;
    std::string status;
    double elapsed_seconds = 0.0;
};

[[nodiscard]] std::string_view axes_name(getnative::AnalysisAxes axes) noexcept {
    switch (axes) {
    case getnative::AnalysisAxes::horizontal: return "horizontal";
    case getnative::AnalysisAxes::vertical: return "vertical";
    case getnative::AnalysisAxes::both: return "both";
    }
    return "unknown";
}

[[nodiscard]] std::string_view filter_family_name(FilterFamily family) noexcept {
    switch (family) {
    case FilterFamily::bilinear: return "bilinear";
    case FilterFamily::bicubic: return "bicubic";
    case FilterFamily::lanczos3: return "lanczos3";
    case FilterFamily::spline64: return "spline64";
    case FilterFamily::lanczos8: return "lanczos8";
    case FilterFamily::mixed_self_test: return "mixed-self-test";
    }
    return "unknown";
}

[[nodiscard]] std::string_view filter_family_display_name(
    FilterFamily family) noexcept {
    switch (family) {
    case FilterFamily::bilinear: return "Bilinear";
    case FilterFamily::bicubic: return "Bicubic";
    case FilterFamily::lanczos3: return "Lanczos 3";
    case FilterFamily::spline64: return "Spline64";
    case FilterFamily::lanczos8: return "Lanczos 8";
    case FilterFamily::mixed_self_test: return "Mixed self-test";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::int32_t filter_support(
    FilterFamily family) noexcept {
    switch (family) {
    case FilterFamily::bilinear: return 1;
    case FilterFamily::bicubic: return 2;
    case FilterFamily::lanczos3: return 3;
    case FilterFamily::spline64: return 4;
    case FilterFamily::lanczos8: return 8;
    case FilterFamily::mixed_self_test: return 0;
    }
    return 0;
}

[[nodiscard]] constexpr std::string_view filter_topology_name(
    FilterFamily family) noexcept {
    switch (family) {
    case FilterFamily::bilinear: return "B2";
    case FilterFamily::bicubic: return "B4";
    case FilterFamily::lanczos3: return "B6";
    case FilterFamily::spline64: return "B8";
    case FilterFamily::lanczos8: return "B16-wide";
    case FilterFamily::mixed_self_test: return "mixed";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view inverse_implementation_path(
    FilterFamily family) noexcept {
    switch (family) {
    case FilterFamily::bicubic: return "ring-specialized-hbw3";
    case FilterFamily::lanczos3: return "ring-specialized-hbw5";
    case FilterFamily::bilinear:
    case FilterFamily::spline64:
    case FilterFamily::lanczos8: return "runtime-generic";
    case FilterFamily::mixed_self_test: return "mixed";
    }
    return "unknown";
}

[[nodiscard]] constexpr double calibration_target_wave_ms(
    FilterFamily family, RunMode mode, bool confirmation) noexcept {
    if (mode == RunMode::survey) {
        if (confirmation) {
            switch (family) {
            case FilterFamily::bilinear: return 300.0;
            case FilterFamily::bicubic: return 350.0;
            case FilterFamily::lanczos3: return 425.0;
            case FilterFamily::spline64: return 525.0;
            case FilterFamily::lanczos8: return 675.0;
            case FilterFamily::mixed_self_test: return 0.0;
            }
        } else {
            switch (family) {
            case FilterFamily::bilinear: return 100.0;
            case FilterFamily::bicubic: return 100.0;
            case FilterFamily::lanczos3: return 120.0;
            case FilterFamily::spline64: return 140.0;
            case FilterFamily::lanczos8: return 180.0;
            case FilterFamily::mixed_self_test: return 0.0;
            }
        }
    }
    if (mode != RunMode::standard) return 0.0;
    if (confirmation) {
        switch (family) {
        case FilterFamily::bilinear: return 650.0;
        case FilterFamily::bicubic: return 800.0;
        case FilterFamily::lanczos3: return 900.0;
        case FilterFamily::spline64: return 1300.0;
        case FilterFamily::lanczos8: return 1900.0;
        case FilterFamily::mixed_self_test: return 0.0;
        }
    } else {
        switch (family) {
        case FilterFamily::bilinear: return 160.0;
        case FilterFamily::bicubic: return 160.0;
        case FilterFamily::lanczos3: return 180.0;
        case FilterFamily::spline64: return 220.0;
        case FilterFamily::lanczos8: return 300.0;
        case FilterFamily::mixed_self_test: return 0.0;
        }
    }
    return 0.0;
}

[[nodiscard]] std::int32_t parse_ordinal(
    std::string_view text, std::string_view option) {
    std::size_t consumed = 0;
    const long value = std::stol(std::string{text}, &consumed);
    if (consumed != text.size() || value < 0
        || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument(std::string{option}
                                    + " requires a non-negative int32 value");
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result;
    std::optional<RunMode> explicit_mode;
    const auto select_mode = [&](RunMode mode) {
        if (explicit_mode && *explicit_mode != mode) {
            throw std::invalid_argument(
                "--survey, --quick, and --standard are mutually exclusive");
        }
        explicit_mode = mode;
        result.mode = mode;
    };
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next = [&](std::string_view option) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string{option} + " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--survey") {
            select_mode(RunMode::survey);
        } else if (argument == "--quick") {
            select_mode(RunMode::quick);
        } else if (argument == "--standard") {
            select_mode(RunMode::standard);
        } else if (argument == "--no-open") {
            result.no_open = true;
        } else if (argument == "--no-pause") {
            result.no_pause = true;
        } else if (argument == "--device") {
            result.device_ordinal = parse_ordinal(next(argument), argument);
        } else if (argument == "--output") {
            result.output_path = std::filesystem::path{next(argument)};
        } else if (argument == "--help" || argument == "-h") {
            result.help = true;
        } else {
            throw std::invalid_argument("unknown argument: " + std::string{argument});
        }
    }
    return result;
}

void print_help() {
    std::cout
        << "GetNative CUDA Pathfinder " << tool_version << "\n\n"
        << "Double-click with no arguments for the representative survey.\n"
        << "Survey mode samples 14 launch policies and eight workloads across\n"
        << "B2/B4/B6/B8/B16 plus B6 serial and single-axis paths. It reports\n"
        << "observed leaders only; use --standard for production-admissible evidence.\n"
        << "Target GPU architectures: sm_75, sm_86, sm_89, and sm_120.\n"
        << "Options:\n"
        << "  --survey         Representative survey (default; evidence only)\n"
        << "  --quick          Short diagnostic run; never production-applicable\n"
        << "  --standard       Full 36-policy production-admissible protocol\n"
        << "  --device N       Benchmark one CUDA device ordinal\n"
        << "  --output PATH    Write the report to an unused path\n"
        << "  --no-open        Do not reveal the report in Explorer\n"
        << "  --no-pause       Do not wait for Enter before exiting\n"
        << "  --help           Show this help\n";
}

[[nodiscard]] bool same_policy(
    const getnative::CudaLaunchPolicy &left,
    const getnative::CudaLaunchPolicy &right) noexcept {
    return left.inverse_threads == right.inverse_threads
        && left.pixel_threads == right.pixel_threads
        && left.maximum_metric_blocks == right.maximum_metric_blocks
        && left.paired_vertical == right.paired_vertical;
}

[[nodiscard]] std::string policy_id(const getnative::CudaLaunchPolicy &policy) {
    return "i" + std::to_string(policy.inverse_threads)
        + "-p" + std::to_string(policy.pixel_threads)
        + "-m" + std::to_string(policy.maximum_metric_blocks)
        + (policy.paired_vertical ? "-vpair" : "-vsingle");
}

[[nodiscard]] std::vector<getnative::CudaLaunchPolicy> make_policies(
    RunMode mode) {
    if (mode == RunMode::quick) {
        return {
            baseline_policy,
            {32U, 256U, 128U, true},
            {128U, 256U, 128U, true},
            {64U, 128U, 128U, true},
            {64U, 256U, 64U, true},
            {64U, 256U, 128U, false},
        };
    }

    if (mode == RunMode::survey) {
        std::vector<getnative::CudaLaunchPolicy> result{baseline_policy};
        const auto add_unique = [&](getnative::CudaLaunchPolicy policy) {
            if (std::none_of(
                    result.begin(), result.end(), [&](const auto &existing) {
                        return same_policy(existing, policy);
                    })) {
                result.push_back(policy);
            }
        };
        for (const std::uint32_t inverse : {32U, 64U, 128U}) {
            for (const bool paired : {false, true}) {
                add_unique({inverse, 256U, 128U, paired});
            }
        }
        for (const std::uint32_t pixel : {128U, 256U}) {
            for (const std::uint32_t metric : {64U, 128U, 256U}) {
                add_unique({64U, pixel, metric, true});
            }
        }
        add_unique({32U, 128U, 128U, true});
        add_unique({32U, 256U, 64U, true});
        add_unique({64U, 128U, 256U, false});
        if (result.size() != 14U) {
            throw std::logic_error("survey policy set must contain 14 policies");
        }
        std::mt19937 generator(search_seed);
        std::shuffle(result.begin() + 1, result.end(), generator);
        return result;
    }

    std::vector<getnative::CudaLaunchPolicy> result;
    for (const std::uint32_t inverse : {32U, 64U, 128U}) {
        for (const std::uint32_t pixel : {128U, 256U}) {
            for (const std::uint32_t metric : {64U, 128U, 256U}) {
                for (const bool paired : {false, true}) {
                    result.push_back({inverse, pixel, metric, paired});
                }
            }
        }
    }
    const auto baseline = std::find_if(
        result.begin(), result.end(), [](const auto &policy) {
            return same_policy(policy, baseline_policy);
        });
    if (baseline == result.end()) {
        throw std::logic_error("autotune policy space omitted the production baseline");
    }
    result.erase(baseline);
    std::mt19937 generator(search_seed);
    std::shuffle(result.begin(), result.end(), generator);
    result.insert(result.begin(), baseline_policy);
    return result;
}

[[nodiscard]] std::vector<WorkloadSpec> make_survey_workload_specs(
    bool confirmation) {
    const std::string suffix = confirmation ? "-confirm" : "-screen";
    const std::array<std::size_t, 8> iteration_limits = confirmation
        ? std::array<std::size_t, 8>{48U, 48U, 64U, 64U, 64U, 96U, 128U, 128U}
        : std::array<std::size_t, 8>{16U, 16U, 20U, 24U, 32U, 24U, 32U, 32U};
    return {
        {"survey-bilinear-1080p-concurrent4" + suffix,
         1920, 1080, 1280, 720, FilterFamily::bilinear,
         getnative::AnalysisAxes::both, 8U, 4U, iteration_limits[0], 0.08},
        {"survey-bicubic-1080p-concurrent4" + suffix,
         1920, 1080, 1280, 720, FilterFamily::bicubic,
         getnative::AnalysisAxes::both, 8U, 4U, iteration_limits[1], 0.15},
        {"survey-lanczos3-1080p-concurrent4" + suffix,
         1920, 1080, 1280, 720, FilterFamily::lanczos3,
         getnative::AnalysisAxes::both, 8U, 4U, iteration_limits[2], 0.15},
        {"survey-spline64-1080p-concurrent4" + suffix,
         1920, 1080, 1280, 720, FilterFamily::spline64,
         getnative::AnalysisAxes::both, 8U, 4U, iteration_limits[3], 0.08},
        {"survey-lanczos8-1080p-concurrent4" + suffix,
         1920, 1080, 1280, 720, FilterFamily::lanczos8,
         getnative::AnalysisAxes::both, 8U, 4U, iteration_limits[4], 0.08},
        {"survey-lanczos3-1080p-batch64" + suffix,
         1920, 1080, 1280, 720, FilterFamily::lanczos3,
         getnative::AnalysisAxes::both, 64U, 1U, iteration_limits[5], 0.25},
        {"survey-lanczos3-1080p-vertical" + suffix,
         1920, 1080, 1280, 720, FilterFamily::lanczos3,
         getnative::AnalysisAxes::vertical, 48U, 1U, iteration_limits[6], 0.105},
        {"survey-lanczos3-1080p-horizontal" + suffix,
         1920, 1080, 1280, 720, FilterFamily::lanczos3,
         getnative::AnalysisAxes::horizontal, 48U, 1U, iteration_limits[7], 0.105},
    };
}

[[nodiscard]] std::vector<WorkloadSpec> make_screening_workload_specs(
    RunMode mode) {
    if (mode == RunMode::quick) {
        return {
            {"quick-bilinear-runtime", 640, 360, 426, 240,
             FilterFamily::bilinear, getnative::AnalysisAxes::both,
             4U, 2U, 2U, 0.10},
            {"quick-bicubic-vertical", 640, 360, 426, 240,
             FilterFamily::bicubic, getnative::AnalysisAxes::vertical,
             8U, 1U, 1U, 0.25},
            {"quick-lanczos3-concurrent", 960, 540, 640, 360,
             FilterFamily::lanczos3, getnative::AnalysisAxes::both,
             4U, 2U, 2U, 0.25},
            {"quick-spline64-runtime", 640, 360, 426, 240,
             FilterFamily::spline64, getnative::AnalysisAxes::both,
             4U, 2U, 2U, 0.20},
            {"quick-lanczos8-wide", 640, 360, 426, 240,
             FilterFamily::lanczos8, getnative::AnalysisAxes::both,
             2U, 1U, 2U, 0.20},
        };
    }
    if (mode == RunMode::survey) return make_survey_workload_specs(false);
    return {
        {"bilinear-1080p-runtime-screen", 1920, 1080, 1280, 720,
         FilterFamily::bilinear, getnative::AnalysisAxes::both,
         8U, 4U, 12U, 0.10},
        {"bicubic-720p-both-screen", 1280, 720, 854, 480,
         FilterFamily::bicubic, getnative::AnalysisAxes::both,
         24U, 1U, 4U, 0.07},
        {"bicubic-1080p-vertical-screen", 1920, 1080, 1280, 720,
         FilterFamily::bicubic, getnative::AnalysisAxes::vertical,
         48U, 1U, 4U, 0.07},
        {"bicubic-1080p-concurrent-screen", 1920, 1080, 1280, 720,
         FilterFamily::bicubic, getnative::AnalysisAxes::both,
         8U, 4U, 24U, 0.21},
        {"lanczos3-720p-both-screen", 1280, 720, 854, 480,
         FilterFamily::lanczos3, getnative::AnalysisAxes::both,
         24U, 1U, 4U, 0.07},
        {"lanczos3-1080p-vertical-screen", 1920, 1080, 1280, 720,
         FilterFamily::lanczos3, getnative::AnalysisAxes::vertical,
         48U, 1U, 4U, 0.07},
        {"lanczos3-1080p-concurrent-screen", 1920, 1080, 1280, 720,
         FilterFamily::lanczos3, getnative::AnalysisAxes::both,
         8U, 4U, 24U, 0.21},
        {"spline64-1080p-runtime-screen", 1920, 1080, 1280, 720,
         FilterFamily::spline64, getnative::AnalysisAxes::both,
         8U, 4U, 12U, 0.10},
        {"lanczos8-1080p-wide-screen", 1920, 1080, 1280, 720,
         FilterFamily::lanczos8, getnative::AnalysisAxes::both,
         8U, 4U, 12U, 0.10},
    };
}

[[nodiscard]] std::vector<WorkloadSpec> make_confirmation_workload_specs(
    RunMode mode) {
    if (mode == RunMode::quick) {
        return make_screening_workload_specs(RunMode::quick);
    }
    if (mode == RunMode::survey) return make_survey_workload_specs(true);
    return {
        {"bilinear-1080p-both-runtime-hbw1", 1920, 1080, 1280, 720,
         FilterFamily::bilinear, getnative::AnalysisAxes::both,
         8U, 4U, 48U, 0.10},
        {"bicubic-720p-both-serial", 1280, 720, 854, 480,
         FilterFamily::bicubic, getnative::AnalysisAxes::both,
         24U, 1U, 8U, 0.07},
        {"bicubic-1080p-vertical-batch", 1920, 1080, 1280, 720,
         FilterFamily::bicubic, getnative::AnalysisAxes::vertical,
         48U, 1U, 8U, 0.0525},
        {"bicubic-1080p-horizontal-batch", 1920, 1080, 1280, 720,
         FilterFamily::bicubic, getnative::AnalysisAxes::horizontal,
         48U, 1U, 8U, 0.0525},
        {"bicubic-1080p-both-concurrent4", 1920, 1080, 1280, 720,
         FilterFamily::bicubic, getnative::AnalysisAxes::both,
         8U, 4U, 96U, 0.175},
        {"lanczos3-720p-both-serial", 1280, 720, 854, 480,
         FilterFamily::lanczos3, getnative::AnalysisAxes::both,
         24U, 1U, 8U, 0.07},
        {"lanczos3-1080p-vertical-batch", 1920, 1080, 1280, 720,
         FilterFamily::lanczos3, getnative::AnalysisAxes::vertical,
         48U, 1U, 8U, 0.0525},
        {"lanczos3-1080p-horizontal-batch", 1920, 1080, 1280, 720,
         FilterFamily::lanczos3, getnative::AnalysisAxes::horizontal,
         48U, 1U, 8U, 0.0525},
        {"lanczos3-1080p-both-concurrent4", 1920, 1080, 1280, 720,
         FilterFamily::lanczos3, getnative::AnalysisAxes::both,
         8U, 4U, 96U, 0.175},
        {"spline64-1080p-both-runtime-hbw7", 1920, 1080, 1280, 720,
         FilterFamily::spline64, getnative::AnalysisAxes::both,
         8U, 4U, 48U, 0.10},
        {"lanczos8-1080p-both-runtime-hbw15", 1920, 1080, 1280, 720,
         FilterFamily::lanczos8, getnative::AnalysisAxes::both,
         8U, 4U, 48U, 0.10},
    };
}

void validate_workload_contract(
    const std::vector<WorkloadSpec> &workloads,
    std::string_view contract_name) {
    if (workloads.empty()) {
        throw std::logic_error(std::string{contract_name} + " contract is empty");
    }
    double weight_sum = 0.0;
    for (std::size_t index = 0; index < workloads.size(); ++index) {
        const WorkloadSpec &workload = workloads[index];
        if (workload.id.empty() || workload.width <= 0 || workload.height <= 0
            || workload.native_width <= 0 || workload.native_height <= 0
            || workload.candidate_count == 0U || workload.concurrency == 0U
            || workload.iterations_per_worker == 0U || workload.weight <= 0.0) {
            throw std::logic_error(
                std::string{contract_name} + " contains an invalid workload");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (workloads[previous].id == workload.id) {
                throw std::logic_error(
                    std::string{contract_name} + " contains duplicate workload IDs");
            }
        }
        const std::int32_t support = filter_support(workload.filter_family);
        if (support <= 0 || 2 * support - 1 > 15 || 2 * support > 16) {
            throw std::logic_error(
                std::string{contract_name} + " exceeds the CUDA plan boundary");
        }
        weight_sum += workload.weight;
    }
    if (std::abs(weight_sum - 1.0) > 1e-9) {
        throw std::logic_error(
            std::string{contract_name} + " decision weights must sum to one");
    }
    for (const FilterFamily family : benchmark_filter_families) {
        const bool represented = std::any_of(
            workloads.begin(), workloads.end(),
            [family](const WorkloadSpec &workload) {
                return workload.filter_family == family;
            });
        if (!represented) {
            throw std::logic_error(
                std::string{contract_name} + " omitted topology "
                + std::string{filter_topology_name(family)});
        }
    }
}

[[nodiscard]] getnative::Filter workload_filter(FilterFamily family) {
    switch (family) {
    case FilterFamily::bilinear: return getnative::Filter::bilinear();
    case FilterFamily::bicubic: return getnative::Filter::bicubic();
    case FilterFamily::lanczos3: return getnative::Filter::lanczos(3);
    case FilterFamily::spline64: return getnative::Filter::spline64();
    case FilterFamily::lanczos8: return getnative::Filter::lanczos(8);
    case FilterFamily::mixed_self_test:
        throw std::logic_error("mixed self-test filters are constructed explicitly");
    }
    throw std::logic_error("unknown workload filter family");
}

[[nodiscard]] std::shared_ptr<const getnative::AxisPlan> make_plan(
    std::int32_t source_size, std::int32_t destination_size,
    double active_length, double shift, const getnative::Filter &filter) {
    return std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        source_size,
        destination_size,
        active_length,
        shift,
        filter,
        getnative::BorderMode::mirror,
    }));
}

void fill_source(std::vector<float> &pixels,
                 std::int32_t width, std::int32_t height,
                 std::ptrdiff_t stride) {
    pixels.assign(static_cast<std::size_t>(height)
                      * static_cast<std::size_t>(stride),
                  -11.0F);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const double value = 0.43
                + 0.19 * std::sin(0.013 * static_cast<double>(x))
                + 0.17 * std::cos(0.017 * static_cast<double>(y))
                + 0.11 * std::sin(0.007 * static_cast<double>(x + 3 * y));
            pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)
                   + static_cast<std::size_t>(x)] = static_cast<float>(value);
        }
    }
}

[[nodiscard]] Fixture make_fixture(const WorkloadSpec &spec) {
    Fixture result;
    result.spec = spec;
    result.iteration_limit_per_worker = spec.iterations_per_worker;
    fill_source(result.pixels, spec.width, spec.height, spec.width);
    result.source = {
        result.pixels.data(), spec.width, spec.height, spec.width,
    };
    result.candidates.reserve(spec.candidate_count);
    for (std::size_t index = 0; index < spec.candidate_count; ++index) {
        const double centered = static_cast<double>(index % 33U) - 16.0;
        const double horizontal_active = static_cast<double>(spec.native_width)
            - centered / 32.0;
        const double vertical_active = static_cast<double>(spec.native_height)
            + centered / 32.0;
        const double horizontal_shift =
            (static_cast<double>((index * 5U) % 19U) - 9.0) / 32.0;
        const double vertical_shift =
            (static_cast<double>((index * 7U) % 17U) - 8.0) / 32.0;
        const getnative::Filter filter = workload_filter(spec.filter_family);
        std::shared_ptr<const getnative::AxisPlan> horizontal;
        std::shared_ptr<const getnative::AxisPlan> vertical;
        if (spec.axes != getnative::AnalysisAxes::vertical) {
            horizontal = make_plan(
                spec.width, spec.native_width, horizontal_active,
                horizontal_shift, filter);
        }
        if (spec.axes != getnative::AnalysisAxes::horizontal) {
            vertical = make_plan(
                spec.height, spec.native_height, vertical_active,
                vertical_shift, filter);
        }
        const auto validate_topology = [&](const auto &plan) {
            if (plan == nullptr) return;
            const std::int32_t support = filter_support(spec.filter_family);
            if (plan->support != support
                || plan->half_bandwidth != 2 * support - 1
                || plan->forward_width != 2 * support) {
                throw std::logic_error(
                    "generated axis plan no longer matches the workload topology contract");
            }
        };
        validate_topology(horizontal);
        validate_topology(vertical);
        result.candidates.push_back({
            std::to_string(index), std::move(horizontal), std::move(vertical),
            spec.axes,
        });
    }
    result.metric = {8, 8, 8, 8, 0.015F, 1U};
    return result;
}

[[nodiscard]] Fixture make_self_test_fixture() {
    Fixture result;
    result.spec = {
        "self-test", 73, 59, 47, 37, FilterFamily::mixed_self_test,
        getnative::AnalysisAxes::both, 7U, 1U, 1U, 0.0,
    };
    constexpr std::ptrdiff_t stride = 80;
    fill_source(result.pixels, result.spec.width, result.spec.height, stride);
    result.source = {
        result.pixels.data(), result.spec.width, result.spec.height, stride,
    };
    const auto horizontal = make_plan(
        result.spec.width, 47, 47.25, -0.125, getnative::Filter::lanczos(3));
    const auto vertical = make_plan(
        result.spec.height, 37, 37.125, 0.125, getnative::Filter::spline16());
    const auto both_x_a = make_plan(
        result.spec.width, 49, 49.0, 0.0, getnative::Filter::bicubic());
    const auto both_y_a = make_plan(
        result.spec.height, 31, 31.0, 0.0, getnative::Filter::bicubic());
    const auto both_x_b = make_plan(
        result.spec.width, 35, 35.0, 0.0, getnative::Filter::spline36());
    const auto both_y_b = make_plan(
        result.spec.height, 41, 41.0, 0.0, getnative::Filter::spline36());
    const auto both_x_bilinear = make_plan(
        result.spec.width, 45, 45.125, -0.0625,
        getnative::Filter::bilinear());
    const auto both_y_bilinear = make_plan(
        result.spec.height, 33, 33.125, 0.0625,
        getnative::Filter::bilinear());
    const auto both_x_spline64 = make_plan(
        result.spec.width, 43, 43.125, -0.0625,
        getnative::Filter::spline64());
    const auto both_y_spline64 = make_plan(
        result.spec.height, 35, 35.125, 0.0625,
        getnative::Filter::spline64());
    const auto both_x_lanczos8 = make_plan(
        result.spec.width, 39, 39.125, -0.0625,
        getnative::Filter::lanczos(8));
    const auto both_y_lanczos8 = make_plan(
        result.spec.height, 29, 29.125, 0.0625,
        getnative::Filter::lanczos(8));
    result.candidates = {
        {"horizontal", horizontal, nullptr, getnative::AnalysisAxes::horizontal},
        {"vertical", nullptr, vertical, getnative::AnalysisAxes::vertical},
        {"both-a", both_x_a, both_y_a, getnative::AnalysisAxes::both},
        {"both-b", both_x_b, both_y_b, getnative::AnalysisAxes::both},
        {"both-bilinear", both_x_bilinear, both_y_bilinear,
         getnative::AnalysisAxes::both},
        {"both-spline64", both_x_spline64, both_y_spline64,
         getnative::AnalysisAxes::both},
        {"both-lanczos8", both_x_lanczos8, both_y_lanczos8,
         getnative::AnalysisAxes::both},
    };
    result.metric = {3, 2, 2, 3, 0.0F, 1U};
    return result;
}

void validate_results(
    const std::vector<getnative::CandidateResult> &actual,
    const std::vector<getnative::CandidateResult> &reference,
    ValidationStats &stats) {
    if (actual.size() != reference.size()) {
        throw std::runtime_error("CUDA result count changed");
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index].id != reference[index].id) {
            throw std::runtime_error("CUDA result order or identity changed");
        }
        if (!std::isfinite(actual[index].error)) {
            throw std::runtime_error("CUDA result is non-finite");
        }
        const double absolute_error =
            std::abs(actual[index].error - reference[index].error);
        const double tolerance = std::max(
            2e-7, 5e-4 * std::abs(reference[index].error));
        const double tolerance_ratio = absolute_error / tolerance;
        stats.maximum_absolute_error = std::max(
            stats.maximum_absolute_error, absolute_error);
        stats.maximum_tolerance_ratio = std::max(
            stats.maximum_tolerance_ratio, tolerance_ratio);
        ++stats.comparison_count;
        if (absolute_error > tolerance) {
            throw std::runtime_error(
                "CUDA output exceeded the correctness tolerance for candidate "
                + actual[index].id);
        }
    }
}

struct WaveResult {
    double wall_ms = 0.0;
    std::vector<std::vector<getnative::CandidateResult>> results;
};

[[nodiscard]] WaveResult run_wave(
    getnative::CudaAnalysisEngine &engine, const Fixture &fixture) {
    WaveResult result;
    result.results.resize(fixture.spec.concurrency);
    if (fixture.spec.concurrency == 1U) {
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0;
             iteration < fixture.spec.iterations_per_worker; ++iteration) {
            result.results[0] = engine.analyze_axis_batch_f32(
                fixture.source, fixture.candidates, fixture.metric, {},
                getnative::GpuStageProfile::stages);
        }
        const auto end = std::chrono::steady_clock::now();
        result.wall_ms = std::chrono::duration<double, std::milli>(end - begin).count();
        return result;
    }

    std::barrier start_gate(
        static_cast<std::ptrdiff_t>(fixture.spec.concurrency + 1U));
    std::vector<std::exception_ptr> failures(fixture.spec.concurrency);
    std::vector<getnative::JoiningThread> workers;
    workers.reserve(fixture.spec.concurrency);
    for (std::size_t worker = 0; worker < fixture.spec.concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            start_gate.arrive_and_wait();
            try {
                for (std::size_t iteration = 0;
                     iteration < fixture.spec.iterations_per_worker; ++iteration) {
                    result.results[worker] = engine.analyze_axis_batch_f32(
                        fixture.source, fixture.candidates, fixture.metric, {},
                        getnative::GpuStageProfile::stages);
                }
            } catch (...) {
                failures[worker] = std::current_exception();
            }
        });
    }
    const auto begin = std::chrono::steady_clock::now();
    start_gate.arrive_and_wait();
    for (auto &worker : workers) worker.join();
    const auto end = std::chrono::steady_clock::now();
    for (const auto &failure : failures) {
        if (failure) std::rethrow_exception(failure);
    }
    result.wall_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    return result;
}

void calibrate_fixture_iterations(
    const getnative::CudaDeviceInfo &device,
    std::vector<Fixture> &fixtures,
    RunMode mode,
    bool confirmation,
    std::size_t minimum_iterations_per_worker) {
    getnative::CudaAnalysisOptions options;
    options.device_ordinal = device.ordinal;
    options.execution_slots = 1U;
    for (const Fixture &fixture : fixtures) {
        options.execution_slots = std::max(
            options.execution_slots, fixture.spec.concurrency);
    }
    options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
    options.launch_policy = baseline_policy;
    getnative::CudaAnalysisEngine engine(options);

    for (Fixture &fixture : fixtures) {
        const std::size_t probe_iterations = std::min(
            calibration_probe_iterations_per_worker,
            fixture.iteration_limit_per_worker);
        fixture.spec.iterations_per_worker = probe_iterations;
        (void)run_wave(engine, fixture);
        const WaveResult measured = run_wave(engine, fixture);
        if (!std::isfinite(measured.wall_ms) || measured.wall_ms <= 0.0) {
            throw std::runtime_error("workload iteration calibration failed");
        }
        const double target_wave_ms = calibration_target_wave_ms(
            fixture.spec.filter_family, mode, confirmation);
        const double desired = std::ceil(
            target_wave_ms * static_cast<double>(probe_iterations)
            / measured.wall_ms);
        const std::size_t bounded_desired = static_cast<std::size_t>(std::min(
            desired,
            static_cast<double>(fixture.iteration_limit_per_worker)));
        fixture.spec.iterations_per_worker = std::clamp(
            bounded_desired,
            std::min(
                minimum_iterations_per_worker,
                fixture.iteration_limit_per_worker),
            fixture.iteration_limit_per_worker);
    }
}

[[nodiscard]] double percentile95(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("p95 requires samples");
    std::sort(values.begin(), values.end());
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(values.size())));
    return values[std::max<std::size_t>(1U, rank) - 1U];
}

[[nodiscard]] ProfileTotals make_profile_totals(
    const getnative::CudaRuntimeTelemetry &telemetry,
    std::size_t measured_frames, std::size_t peak_working_set_bytes) {
    return {
        measured_frames,
        telemetry.kernel_launch_count,
        telemetry.tile_count,
        telemetry.buffer_allocation_count,
        telemetry.plan_cache_hits,
        telemetry.plan_cache_misses,
        telemetry.source_upload_bytes,
        telemetry.plan_upload_bytes,
        telemetry.result_readback_bytes,
        peak_working_set_bytes,
        telemetry.workspace_bytes,
        telemetry.pinned_staging_bytes,
        telemetry.execution_slot_wait_ms,
        telemetry.host_pack_ms,
        telemetry.source_staging_ms,
        telemetry.source_upload_ms,
        telemetry.plan_upload_ms,
        telemetry.source_transpose_ms,
        telemetry.horizontal_fused_ms,
        telemetry.inverse_horizontal_ms,
        telemetry.inverse_vertical_ms,
        telemetry.forward_intermediate_ms,
        telemetry.metric_ms,
        telemetry.kernel_ms,
        telemetry.result_readback_ms,
        telemetry.gpu_total_ms,
    };
}

[[nodiscard]] ProfileTotals merge_profiles(
    const ProfileTotals &left, const ProfileTotals &right) {
    ProfileTotals result;
    result.measured_frames = left.measured_frames + right.measured_frames;
    result.kernel_launch_count = left.kernel_launch_count + right.kernel_launch_count;
    result.tile_count = left.tile_count + right.tile_count;
    result.buffer_allocation_count =
        left.buffer_allocation_count + right.buffer_allocation_count;
    result.plan_cache_hits = left.plan_cache_hits + right.plan_cache_hits;
    result.plan_cache_misses = left.plan_cache_misses + right.plan_cache_misses;
    result.source_upload_bytes = left.source_upload_bytes + right.source_upload_bytes;
    result.plan_upload_bytes = left.plan_upload_bytes + right.plan_upload_bytes;
    result.result_readback_bytes =
        left.result_readback_bytes + right.result_readback_bytes;
    result.peak_working_set_bytes = std::max(
        left.peak_working_set_bytes, right.peak_working_set_bytes);
    result.workspace_bytes = std::max(left.workspace_bytes, right.workspace_bytes);
    result.pinned_staging_bytes = std::max(
        left.pinned_staging_bytes, right.pinned_staging_bytes);
    result.execution_slot_wait_ms =
        left.execution_slot_wait_ms + right.execution_slot_wait_ms;
    result.host_pack_ms = left.host_pack_ms + right.host_pack_ms;
    result.source_staging_ms = left.source_staging_ms + right.source_staging_ms;
    result.source_upload_ms = left.source_upload_ms + right.source_upload_ms;
    result.plan_upload_ms = left.plan_upload_ms + right.plan_upload_ms;
    result.source_transpose_ms =
        left.source_transpose_ms + right.source_transpose_ms;
    result.horizontal_fused_ms =
        left.horizontal_fused_ms + right.horizontal_fused_ms;
    result.inverse_horizontal_ms =
        left.inverse_horizontal_ms + right.inverse_horizontal_ms;
    result.inverse_vertical_ms =
        left.inverse_vertical_ms + right.inverse_vertical_ms;
    result.forward_intermediate_ms =
        left.forward_intermediate_ms + right.forward_intermediate_ms;
    result.metric_ms = left.metric_ms + right.metric_ms;
    result.kernel_ms = left.kernel_ms + right.kernel_ms;
    result.result_readback_ms =
        left.result_readback_ms + right.result_readback_ms;
    result.gpu_total_ms = left.gpu_total_ms + right.gpu_total_ms;
    return result;
}

[[nodiscard]] PolicyEvaluation evaluate_policy(
    const getnative::CudaDeviceInfo &device,
    const getnative::CudaLaunchPolicy &policy,
    const std::vector<Fixture> &fixtures,
    std::size_t warmup_count,
    std::size_t sample_count,
    const std::vector<std::vector<getnative::CandidateResult>> *references,
    std::vector<std::vector<getnative::CandidateResult>> *captured_references,
    std::string_view phase,
    std::size_t progress_index,
    std::size_t progress_total) {
    PolicyEvaluation evaluation;
    evaluation.policy = policy;
    evaluation.id = policy_id(policy);
    const auto evaluation_begin = std::chrono::steady_clock::now();
    std::cout << '[' << phase << ' ' << progress_index << '/' << progress_total
              << "] " << evaluation.id << std::endl;
    try {
        getnative::CudaAnalysisOptions options;
        options.device_ordinal = device.ordinal;
        options.execution_slots = 1U;
        for (const Fixture &fixture : fixtures) {
            options.execution_slots = std::max(
                options.execution_slots, fixture.spec.concurrency);
        }
        options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
        options.launch_policy = policy;
        getnative::CudaAnalysisEngine engine(options);

        if (captured_references != nullptr) {
            captured_references->clear();
            captured_references->resize(fixtures.size());
        }
        evaluation.workloads.reserve(fixtures.size());
        for (std::size_t workload_index = 0;
             workload_index < fixtures.size(); ++workload_index) {
            const Fixture &fixture = fixtures[workload_index];
            ValidationStats validation;
            const auto validate_wave = [&](const WaveResult &wave) {
                const std::vector<getnative::CandidateResult> *reference = nullptr;
                if (references != nullptr) {
                    reference = &references->at(workload_index);
                } else {
                    auto &captured = captured_references->at(workload_index);
                    if (captured.empty()) captured = wave.results.front();
                    reference = &captured;
                }
                for (const auto &worker_results : wave.results) {
                    validate_results(worker_results, *reference, validation);
                }
            };

            for (std::size_t warmup = 0; warmup < warmup_count; ++warmup) {
                validate_wave(run_wave(engine, fixture));
            }
            engine.reset_analysis_telemetry();
            std::vector<double> frame_samples;
            frame_samples.reserve(sample_count);
            for (std::size_t sample = 0; sample < sample_count; ++sample) {
                const WaveResult wave = run_wave(engine, fixture);
                validate_wave(wave);
                frame_samples.push_back(
                    wave.wall_ms / static_cast<double>(
                        fixture.spec.concurrency
                        * fixture.spec.iterations_per_worker));
            }
            const auto telemetry = engine.runtime_telemetry();
            WorkloadEvaluation workload;
            workload.spec = fixture.spec;
            workload.p95_frame_ms = percentile95(frame_samples);
            workload.frame_ms = getnative::benchmark::summarize(
                std::move(frame_samples));
            workload.frames_per_second = 1000.0 / workload.frame_ms.median;
            workload.validation = validation;
            workload.profile = make_profile_totals(
                telemetry,
                sample_count * fixture.spec.concurrency
                    * fixture.spec.iterations_per_worker,
                engine.peak_working_set_bytes());
            evaluation.workloads.push_back(std::move(workload));
        }
        evaluation.valid = true;
    } catch (const std::exception &error) {
        evaluation.failure = error.what();
    }
    evaluation.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - evaluation_begin).count();
    return evaluation;
}

[[nodiscard]] SelfTestResult run_self_test(
    const getnative::CudaDeviceInfo &device) {
    const Fixture fixture = make_self_test_fixture();
    const auto cpu = getnative::analyze_batch_f32(
        fixture.source, fixture.candidates, fixture.metric, 1U);
    getnative::CudaAnalysisOptions options;
    options.device_ordinal = device.ordinal;
    options.execution_slots = 1U;
    options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
    getnative::CudaAnalysisEngine engine(options);
    const auto first = engine.analyze_axis_batch_f32(
        fixture.source, fixture.candidates, fixture.metric, {},
        getnative::GpuStageProfile::stages);
    SelfTestResult result;
    validate_results(first, cpu, result.validation);
    const auto second = engine.analyze_axis_batch_f32(
        fixture.source, fixture.candidates, fixture.metric, {},
        getnative::GpuStageProfile::stages);
    result.repeat_bitwise_stable = first.size() == second.size();
    for (std::size_t index = 0;
         result.repeat_bitwise_stable && index < first.size(); ++index) {
        result.repeat_bitwise_stable = first[index].id == second[index].id
            && first[index].error == second[index].error;
    }
    result.telemetry = engine.runtime_telemetry();
    result.resource_gate_passed = result.telemetry.kernel_resources.size() == 9U
        && std::all_of(
            result.telemetry.kernel_resources.begin(),
            result.telemetry.kernel_resources.end(),
            [](const getnative::CudaKernelResourceInfo &resource) {
                return resource.local_bytes == 0;
            });
    result.passed = result.repeat_bitwise_stable && result.resource_gate_passed;
    if (!result.passed) {
        throw std::runtime_error(
            "CUDA self-test failed its repeatability or kernel-resource gate");
    }
    return result;
}

[[nodiscard]] double precondition_device(
    const getnative::CudaDeviceInfo &device,
    const Fixture &fixture,
    bool quick) {
    getnative::CudaAnalysisOptions options;
    options.device_ordinal = device.ordinal;
    options.execution_slots = fixture.spec.concurrency;
    options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
    getnative::CudaAnalysisEngine engine(options);

    const auto begin = std::chrono::steady_clock::now();
    std::vector<getnative::CandidateResult> reference;
    ValidationStats validation;
    double elapsed_seconds = 0.0;
    do {
        const WaveResult wave = run_wave(engine, fixture);
        if (reference.empty()) reference = wave.results.front();
        for (const auto &worker_results : wave.results) {
            validate_results(worker_results, reference, validation);
        }
        elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
    } while (!quick && elapsed_seconds < minimum_precondition_seconds);
    return elapsed_seconds;
}

[[nodiscard]] std::optional<FamilyComparison> calculate_family_comparison(
    const PolicyEvaluation &candidate,
    const PolicyEvaluation &baseline,
    FilterFamily family) {
    if (!candidate.valid || !baseline.valid
        || candidate.workloads.size() != baseline.workloads.size()) {
        return std::nullopt;
    }
    FamilyComparison result;
    result.family = family;
    result.worst_workload_improvement =
        std::numeric_limits<double>::infinity();
    double weighted_log_ratio = 0.0;
    double weight_sum = 0.0;
    for (std::size_t index = 0; index < candidate.workloads.size(); ++index) {
        const auto &current = candidate.workloads[index];
        const auto &reference = baseline.workloads[index];
        if (current.spec.id != reference.spec.id) return std::nullopt;
        if (current.spec.filter_family != family) continue;
        if (current.frame_ms.median <= 0.0 || reference.frame_ms.median <= 0.0) {
            return std::nullopt;
        }
        const double throughput_ratio =
            reference.frame_ms.median / current.frame_ms.median;
        result.worst_workload_improvement = std::min(
            result.worst_workload_improvement, throughput_ratio - 1.0);
        weighted_log_ratio += current.spec.weight * std::log(throughput_ratio);
        weight_sum += current.spec.weight;
        result.maximum_relative_mad = std::max({
            result.maximum_relative_mad,
            current.frame_ms.mad / current.frame_ms.median,
            reference.frame_ms.mad / reference.frame_ms.median,
        });
    }
    if (weight_sum <= 0.0) return std::nullopt;
    result.composite_improvement =
        std::exp(weighted_log_ratio / weight_sum) - 1.0;
    return result;
}

[[nodiscard]] const FamilyComparison *find_family_comparison(
    const PolicyEvaluation &evaluation, FilterFamily family) noexcept {
    const auto found = std::find_if(
        evaluation.family_comparisons.begin(),
        evaluation.family_comparisons.end(),
        [family](const FamilyComparison &comparison) {
            return comparison.family == family;
        });
    return found == evaluation.family_comparisons.end() ? nullptr : &*found;
}

[[nodiscard]] FamilyComparison *find_family_comparison(
    PolicyEvaluation &evaluation, FilterFamily family) noexcept {
    const auto found = std::find_if(
        evaluation.family_comparisons.begin(),
        evaluation.family_comparisons.end(),
        [family](const FamilyComparison &comparison) {
            return comparison.family == family;
        });
    return found == evaluation.family_comparisons.end() ? nullptr : &*found;
}

[[nodiscard]] const WorkloadEvaluation *find_family_headline_workload(
    const PolicyEvaluation &evaluation, FilterFamily family) noexcept {
    const WorkloadEvaluation *result = nullptr;
    for (const auto &workload : evaluation.workloads) {
        if (workload.spec.filter_family != family) continue;
        if (result == nullptr || workload.spec.weight > result->spec.weight) {
            result = &workload;
        }
    }
    return result;
}

[[nodiscard]] const WorkloadEvaluation *find_workload(
    const PolicyEvaluation &evaluation, std::string_view id) noexcept {
    const auto found = std::find_if(
        evaluation.workloads.begin(), evaluation.workloads.end(),
        [id](const WorkloadEvaluation &workload) {
            return workload.spec.id == id;
        });
    return found == evaluation.workloads.end() ? nullptr : &*found;
}

void compare_policy_to_baseline(
    PolicyEvaluation &candidate, const PolicyEvaluation &baseline) {
    if (!candidate.valid || !baseline.valid
        || candidate.workloads.size() != baseline.workloads.size()) {
        return;
    }
    double weighted_log_ratio = 0.0;
    double weight_sum = 0.0;
    candidate.worst_workload_improvement =
        std::numeric_limits<double>::infinity();
    candidate.maximum_relative_mad = 0.0;
    for (std::size_t index = 0; index < candidate.workloads.size(); ++index) {
        auto &current = candidate.workloads[index];
        const auto &reference = baseline.workloads[index];
        if (current.spec.id != reference.spec.id
            || current.frame_ms.median <= 0.0
            || reference.frame_ms.median <= 0.0) {
            return;
        }
        const double throughput_ratio =
            reference.frame_ms.median / current.frame_ms.median;
        current.has_comparison = true;
        current.improvement_vs_baseline = throughput_ratio - 1.0;
        candidate.worst_workload_improvement = std::min(
            candidate.worst_workload_improvement,
            current.improvement_vs_baseline);
        weighted_log_ratio += current.spec.weight * std::log(throughput_ratio);
        weight_sum += current.spec.weight;
        candidate.maximum_relative_mad = std::max({
            candidate.maximum_relative_mad,
            current.frame_ms.mad / current.frame_ms.median,
            reference.frame_ms.mad / reference.frame_ms.median,
        });
    }
    if (weight_sum <= 0.0) return;
    candidate.composite_improvement =
        std::exp(weighted_log_ratio / weight_sum) - 1.0;
    candidate.family_comparisons.clear();
    for (const FilterFamily family : benchmark_filter_families) {
        if (auto comparison = calculate_family_comparison(
                candidate, baseline, family)) {
            candidate.family_comparisons.push_back(*comparison);
        }
    }
    candidate.has_comparison = true;
}

void mark_baseline_comparison(PolicyEvaluation &baseline) {
    if (!baseline.valid) return;
    baseline.has_comparison = true;
    baseline.composite_improvement = 0.0;
    baseline.worst_workload_improvement = 0.0;
    baseline.maximum_relative_mad = 0.0;
    baseline.family_comparisons.clear();
    for (auto &workload : baseline.workloads) {
        workload.has_comparison = true;
        workload.improvement_vs_baseline = 0.0;
        if (workload.frame_ms.median > 0.0) {
            baseline.maximum_relative_mad = std::max(
                baseline.maximum_relative_mad,
                workload.frame_ms.mad / workload.frame_ms.median);
        }
    }
    for (const FilterFamily family : benchmark_filter_families) {
        if (const auto comparison = calculate_family_comparison(
                baseline, baseline, family)) {
            baseline.family_comparisons.push_back(*comparison);
        }
    }
}

[[nodiscard]] PolicyEvaluation aggregate_baselines(
    const PolicyEvaluation &before, const PolicyEvaluation &after) {
    if (!before.valid || !after.valid
        || before.workloads.size() != after.workloads.size()) {
        throw std::runtime_error("confirmation baseline aggregation failed");
    }
    PolicyEvaluation result = before;
    result.id = policy_id(baseline_policy);
    result.elapsed_seconds = before.elapsed_seconds + after.elapsed_seconds;
    result.workloads.clear();
    result.workloads.reserve(before.workloads.size());
    for (std::size_t index = 0; index < before.workloads.size(); ++index) {
        const auto &left = before.workloads[index];
        const auto &right = after.workloads[index];
        if (left.spec.id != right.spec.id) {
            throw std::runtime_error("confirmation workload ordering changed");
        }
        WorkloadEvaluation combined = left;
        std::vector<double> raw = left.frame_ms.raw;
        raw.insert(raw.end(), right.frame_ms.raw.begin(), right.frame_ms.raw.end());
        combined.frame_ms = getnative::benchmark::summarize(std::move(raw));
        combined.frames_per_second = 1000.0 / combined.frame_ms.median;
        combined.p95_frame_ms = percentile95(combined.frame_ms.raw);
        combined.validation.maximum_absolute_error = std::max(
            left.validation.maximum_absolute_error,
            right.validation.maximum_absolute_error);
        combined.validation.maximum_tolerance_ratio = std::max(
            left.validation.maximum_tolerance_ratio,
            right.validation.maximum_tolerance_ratio);
        combined.validation.comparison_count =
            left.validation.comparison_count + right.validation.comparison_count;
        combined.profile = merge_profiles(left.profile, right.profile);
        result.workloads.push_back(std::move(combined));
    }
    mark_baseline_comparison(result);
    return result;
}

[[nodiscard]] double maximum_baseline_drift(
    const PolicyEvaluation &before, const PolicyEvaluation &after) {
    double result = 0.0;
    for (std::size_t index = 0; index < before.workloads.size(); ++index) {
        const double initial = before.workloads[index].frame_ms.median;
        const double final = after.workloads[index].frame_ms.median;
        if (initial > 0.0) result = std::max(result, std::abs(final / initial - 1.0));
    }
    return result;
}

[[nodiscard]] Recommendation decide_recommendation(
    RunMode mode, ConfirmationResult &confirmation) {
    Recommendation result;
    result.selected_policy_id = policy_id(baseline_policy);
    result.baseline_maximum_drift = maximum_baseline_drift(
        confirmation.baseline_before, confirmation.baseline_after);

    std::vector<PolicyEvaluation *> ranked;
    for (auto &candidate : confirmation.finalists) {
        compare_policy_to_baseline(candidate, confirmation.baseline_aggregate);
        if (candidate.has_comparison) ranked.push_back(&candidate);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto *left, const auto *right) {
        return left->composite_improvement > right->composite_improvement;
    });
    if (!ranked.empty()) {
        result.provisional_best_policy_id = ranked.front()->id;
        result.composite_improvement = ranked.front()->composite_improvement;
        result.worst_workload_improvement =
            ranked.front()->worst_workload_improvement;
    } else {
        result.provisional_best_policy_id = result.selected_policy_id;
    }

    if (mode == RunMode::quick) {
        result.decision = "diagnostic_only";
        result.rationale =
            "Quick mode is a smoke test and cannot produce a production recommendation.";
        return result;
    }
    if (mode == RunMode::survey) {
        result.decision = "survey_only";
        result.rationale =
            "Survey mode reports representative observations only; run --standard "
            "before promoting a launch policy.";
        return result;
    }

    PolicyEvaluation *winner = nullptr;
    PolicyEvaluation *unstable_candidate = nullptr;
    for (PolicyEvaluation *candidate : ranked) {
        candidate->required_gain = std::max({
            minimum_composite_gain,
            2.0 * candidate->maximum_relative_mad,
            result.baseline_maximum_drift,
        });
        candidate->decision_admissible =
            candidate->composite_improvement > candidate->required_gain
            && candidate->worst_workload_improvement
                >= -maximum_workload_regression;
        if (!candidate->decision_admissible) continue;
        if (candidate->maximum_relative_mad <= 0.03
            && result.baseline_maximum_drift <= 0.05) {
            if (winner == nullptr) winner = candidate;
        } else if (unstable_candidate == nullptr) {
            unstable_candidate = candidate;
        }
    }

    if (winner != nullptr) {
        result.decision = "candidate_recommended";
        result.selected_policy_id = winner->id;
        result.composite_improvement = winner->composite_improvement;
        result.worst_workload_improvement = winner->worst_workload_improvement;
        result.required_gain = winner->required_gain;
        result.safe_to_apply = true;
        result.rationale =
            "The candidate cleared correctness, noise, drift, composite-gain, "
            "and per-workload regression gates.";
    } else if (unstable_candidate != nullptr) {
        result.decision = "inconclusive";
        result.composite_improvement = unstable_candidate->composite_improvement;
        result.worst_workload_improvement =
            unstable_candidate->worst_workload_improvement;
        result.required_gain = unstable_candidate->required_gain;
        result.rationale =
            "A candidate cleared the performance gates, but measurement noise "
            "or baseline drift was too high for automatic adoption.";
    } else {
        result.decision = "retain_baseline";
        result.required_gain = minimum_composite_gain;
        if (!ranked.empty()) {
            result.composite_improvement = ranked.front()->composite_improvement;
            result.worst_workload_improvement =
                ranked.front()->worst_workload_improvement;
            result.required_gain = ranked.front()->required_gain;
        }
        result.rationale =
            "No confirmed candidate cleared every conservative decision gate.";
    }

    double decision_noise = confirmation.baseline_aggregate.maximum_relative_mad;
    if (winner != nullptr) {
        decision_noise = std::max(decision_noise, winner->maximum_relative_mad);
    } else if (unstable_candidate != nullptr) {
        decision_noise = std::max(
            decision_noise, unstable_candidate->maximum_relative_mad);
    }
    if (decision_noise <= 0.01 && result.baseline_maximum_drift <= 0.02) {
        result.confidence = "high";
    } else if (decision_noise <= 0.03
               && result.baseline_maximum_drift <= 0.05) {
        result.confidence = "medium";
    } else {
        result.confidence = "low";
        result.safe_to_apply = false;
        if (result.decision == "candidate_recommended") {
            result.decision = "inconclusive";
            result.selected_policy_id = policy_id(baseline_policy);
            result.rationale =
                "A candidate was faster, but measurement noise or baseline drift "
                "was too high for automatic adoption.";
        }
    }
    return result;
}

[[nodiscard]] std::vector<FamilyRecommendation> decide_family_recommendations(
    RunMode mode,
    ConfirmationResult &confirmation,
    double baseline_drift) {
    std::vector<FamilyRecommendation> result;
    for (const FilterFamily family : benchmark_filter_families) {
        FamilyRecommendation recommendation;
        recommendation.family = family;
        recommendation.selected_policy_id = policy_id(baseline_policy);
        if (const WorkloadEvaluation *headline = find_family_headline_workload(
                confirmation.baseline_aggregate, family)) {
            recommendation.headline_workload_id = headline->spec.id;
            recommendation.baseline_frames_per_second =
                headline->frames_per_second;
            recommendation.selected_frames_per_second =
                headline->frames_per_second;
            recommendation.provisional_best_frames_per_second =
                headline->frames_per_second;
        }
        std::vector<std::pair<PolicyEvaluation *, FamilyComparison *>> ranked;
        for (auto &candidate : confirmation.finalists) {
            if (FamilyComparison *comparison =
                    find_family_comparison(candidate, family)) {
                ranked.emplace_back(&candidate, comparison);
            }
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
            return left.second->composite_improvement
                > right.second->composite_improvement;
        });
        recommendation.provisional_best_policy_id = ranked.empty()
            ? recommendation.selected_policy_id : ranked.front().first->id;
        if (!ranked.empty()) {
            const FamilyComparison &observed = *ranked.front().second;
            recommendation.composite_improvement =
                observed.composite_improvement;
            recommendation.worst_workload_improvement =
                observed.worst_workload_improvement;
            recommendation.maximum_relative_mad = observed.maximum_relative_mad;
            if (const WorkloadEvaluation *headline = find_workload(
                    *ranked.front().first,
                    recommendation.headline_workload_id)) {
                recommendation.provisional_best_frames_per_second =
                    headline->frames_per_second;
            }
        }
        if (mode != RunMode::standard) {
            recommendation.decision = mode == RunMode::quick
                ? "diagnostic_only" : "survey_only";
            recommendation.rationale = mode == RunMode::quick
                ? "Quick mode cannot produce a filter-family recommendation."
                : "Survey mode records an observed family leader only; --standard "
                  "is required for an admissible recommendation.";
            result.push_back(std::move(recommendation));
            continue;
        }

        for (auto &[candidate, comparison] : ranked) {
            comparison->required_gain = std::max({
                minimum_composite_gain,
                2.0 * comparison->maximum_relative_mad,
                baseline_drift,
            });
            comparison->decision_admissible =
                comparison->composite_improvement > comparison->required_gain
                && comparison->worst_workload_improvement
                    >= -maximum_workload_regression;
            if (!comparison->decision_admissible
                || comparison->maximum_relative_mad > 0.03
                || baseline_drift > 0.05) {
                continue;
            }
            recommendation.decision = "benchmark_winner";
            recommendation.selected_policy_id = candidate->id;
            recommendation.composite_improvement =
                comparison->composite_improvement;
            recommendation.worst_workload_improvement =
                comparison->worst_workload_improvement;
            recommendation.maximum_relative_mad =
                comparison->maximum_relative_mad;
            recommendation.required_gain = comparison->required_gain;
            recommendation.benchmark_winner = true;
            recommendation.confidence =
                comparison->maximum_relative_mad <= 0.01
                    && baseline_drift <= 0.02 ? "high" : "medium";
            if (const WorkloadEvaluation *headline = find_workload(
                    *candidate, recommendation.headline_workload_id)) {
                recommendation.selected_frames_per_second =
                    headline->frames_per_second;
            }
            recommendation.rationale =
                "The family leader cleared correctness, noise, drift, gain, "
                "and per-workload regression gates.";
            break;
        }
        if (!recommendation.benchmark_winner && !ranked.empty()) {
            const FamilyComparison &best = *ranked.front().second;
            recommendation.composite_improvement = best.composite_improvement;
            recommendation.worst_workload_improvement =
                best.worst_workload_improvement;
            recommendation.maximum_relative_mad = best.maximum_relative_mad;
            recommendation.required_gain = best.required_gain;
            recommendation.rationale =
                "The observed family leader did not clear every conservative "
                "decision gate; the baseline remains selected.";
        } else if (!recommendation.benchmark_winner) {
            recommendation.rationale =
                "No valid confirmed comparison was available for this family.";
        }
        result.push_back(std::move(recommendation));
    }
    return result;
}

[[nodiscard]] DeviceTuningResult tune_device(
    const getnative::CudaDeviceInfo &device,
    std::vector<Fixture> &screening_fixtures,
    std::vector<Fixture> &confirmation_fixtures,
    const Configuration &config) {
    DeviceTuningResult result;
    result.device = device;
    const auto device_begin = std::chrono::steady_clock::now();
    try {
        std::cout << "\nDevice " << device.ordinal << ": " << device.name
                  << " (sm_" << device.compute_capability_major
                  << device.compute_capability_minor << ")\n"
                  << "Running correctness and artifact self-test..." << std::endl;
        result.self_test = run_self_test(device);
        std::cout << "Safety gates passed: B2/B4/B6/B8/B16 CPU agreement, "
                  << "bitwise repeat, "
                  << result.self_test.telemetry.kernel_resources.size()
                  << " kernels with zero local-memory spill." << std::endl;
        if (!is_quick(config.mode)) {
            std::cout << "Calibrating fixed sample lengths for this GPU..."
                      << std::endl;
            calibrate_fixture_iterations(
                device, screening_fixtures,
                config.mode,
                false,
                screening_minimum_iterations_per_worker);
            calibrate_fixture_iterations(
                device, confirmation_fixtures,
                config.mode,
                true,
                confirmation_minimum_iterations_per_worker);
            std::cout << "Sample lengths locked; concurrency remains unchanged."
                      << std::endl;
        }
        std::cout << "Preconditioning sustained CUDA load..." << std::endl;
        result.precondition_seconds = precondition_device(
            device, confirmation_fixtures.back(), is_quick(config.mode));
        std::cout << "GPU ready after " << std::fixed << std::setprecision(2)
                  << result.precondition_seconds << " s of sustained work.\n"
                  << std::defaultfloat << std::endl;

        const auto policies = make_policies(config.mode);
        const std::size_t screen_warmups = 1U;
        const std::size_t screen_samples = config.mode == RunMode::quick
            ? 1U : (config.mode == RunMode::survey ? 2U : 3U);
        std::vector<std::vector<getnative::CandidateResult>> screening_references;
        result.screening.reserve(policies.size());
        result.screening.push_back(evaluate_policy(
            device, baseline_policy, screening_fixtures,
            screen_warmups, screen_samples,
            nullptr, &screening_references, "screen", 1U, policies.size()));
        if (!result.screening.front().valid) {
            throw std::runtime_error(
                "production baseline failed screening: "
                + result.screening.front().failure);
        }
        mark_baseline_comparison(result.screening.front());
        for (std::size_t index = 1; index < policies.size(); ++index) {
            result.screening.push_back(evaluate_policy(
                device, policies[index], screening_fixtures,
                screen_warmups, screen_samples,
                &screening_references, nullptr,
                "screen", index + 1U, policies.size()));
            compare_policy_to_baseline(
                result.screening.back(), result.screening.front());
        }

        std::vector<const PolicyEvaluation *> ranked;
        for (std::size_t index = 1; index < result.screening.size(); ++index) {
            const auto &candidate = result.screening[index];
            if (candidate.valid && candidate.has_comparison
                && (is_quick(config.mode)
                    || candidate.worst_workload_improvement >= -0.05)) {
                ranked.push_back(&candidate);
            }
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto *left, const auto *right) {
            return left->composite_improvement > right->composite_improvement;
        });
        std::vector<const PolicyEvaluation *> finalists;
        const auto add_finalist = [&](const PolicyEvaluation *candidate) {
            if (candidate == nullptr) return;
            const bool exists = std::any_of(
                finalists.begin(), finalists.end(),
                [candidate](const PolicyEvaluation *selected) {
                    return selected->id == candidate->id;
                });
            if (!exists
                && finalists.size()
                    < maximum_confirmation_finalists(config.mode)) {
                finalists.push_back(candidate);
            }
        };
        const std::size_t global_finalists = std::min<std::size_t>(
            is_quick(config.mode) ? 1U : 2U, ranked.size());
        for (std::size_t index = 0; index < global_finalists; ++index) {
            add_finalist(ranked[index]);
        }
        if (!is_quick(config.mode)) {
            struct FamilyLeader {
                const PolicyEvaluation *candidate = nullptr;
                double improvement = 0.0;
            };
            std::vector<FamilyLeader> family_leaders;
            family_leaders.reserve(benchmark_filter_families.size());
            for (const FilterFamily family : benchmark_filter_families) {
                const PolicyEvaluation *family_best = nullptr;
                for (std::size_t index = 1; index < result.screening.size(); ++index) {
                    const auto &candidate = result.screening[index];
                    const FamilyComparison *comparison =
                        find_family_comparison(candidate, family);
                    if (!candidate.valid || comparison == nullptr
                        || comparison->worst_workload_improvement < -0.05) {
                        continue;
                    }
                    const FamilyComparison *best_comparison = family_best == nullptr
                        ? nullptr : find_family_comparison(*family_best, family);
                    if (best_comparison == nullptr
                        || comparison->composite_improvement
                            > best_comparison->composite_improvement) {
                        family_best = &candidate;
                    }
                }
                if (family_best != nullptr) {
                    const FamilyComparison *comparison =
                        find_family_comparison(*family_best, family);
                    family_leaders.push_back({
                        family_best,
                        comparison == nullptr ? 0.0
                                              : comparison->composite_improvement,
                    });
                }
            }
            std::sort(
                family_leaders.begin(), family_leaders.end(),
                [](const FamilyLeader &left, const FamilyLeader &right) {
                    return left.improvement > right.improvement;
                });
            for (const FamilyLeader &leader : family_leaders) {
                add_finalist(leader.candidate);
            }
        }
        const std::size_t finalist_count = finalists.size();
        std::cout << "\nScreening complete: " << policies.size()
                  << " routes x " << screening_fixtures.size()
                  << " workloads.\nFinalists advancing:";
        if (finalists.empty()) {
            std::cout << " none";
        } else {
            for (const auto *finalist : finalists) {
                std::cout << ' ' << finalist->id;
            }
        }
        std::cout << "\n" << std::endl;

        const std::size_t confirmation_warmups =
            is_quick(config.mode) ? 1U : 2U;
        const std::size_t confirmation_samples = config.mode == RunMode::quick
            ? 3U : (config.mode == RunMode::survey ? 5U : 7U);
        const std::size_t confirmation_total = finalist_count + 2U;
        std::vector<std::vector<getnative::CandidateResult>> confirmation_references;
        result.confirmation.baseline_before = evaluate_policy(
            device, baseline_policy, confirmation_fixtures,
            confirmation_warmups, confirmation_samples,
            nullptr, &confirmation_references,
            "confirm", 1U, confirmation_total);
        if (!result.confirmation.baseline_before.valid) {
            throw std::runtime_error("confirmation baseline-before failed");
        }
        result.confirmation.finalists.reserve(finalist_count);
        for (std::size_t index = 0; index < finalist_count; ++index) {
            result.confirmation.finalists.push_back(evaluate_policy(
                device, finalists[index]->policy, confirmation_fixtures,
                confirmation_warmups, confirmation_samples,
                &confirmation_references, nullptr,
                "confirm", index + 2U, confirmation_total));
        }
        result.confirmation.baseline_after = evaluate_policy(
            device, baseline_policy, confirmation_fixtures,
            confirmation_warmups, confirmation_samples,
            &confirmation_references, nullptr, "confirm", confirmation_total,
            confirmation_total);
        if (!result.confirmation.baseline_after.valid) {
            throw std::runtime_error("confirmation baseline-after failed");
        }
        result.confirmation.baseline_aggregate = aggregate_baselines(
            result.confirmation.baseline_before,
            result.confirmation.baseline_after);
        result.recommendation = decide_recommendation(
            config.mode, result.confirmation);
        result.family_recommendations = decide_family_recommendations(
            config.mode, result.confirmation,
            result.recommendation.baseline_maximum_drift);
        result.status = "complete";
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    result.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - device_begin).count();
    return result;
}

void append_device_info(
    std::ostream &output, const getnative::CudaDeviceInfo &device) {
    output << "{\"ordinal\":" << device.ordinal
           << ",\"name\":" << getnative::benchmark::json_string(device.name)
           << ",\"compute_capability\":\""
           << device.compute_capability_major << '.'
           << device.compute_capability_minor << "\""
           << ",\"target_architecture\":"
           << (target_architecture_supported(device) ? "true" : "false")
           << ",\"driver_version\":" << device.driver_version
           << ",\"total_memory_bytes\":" << device.total_memory_bytes
           << ",\"maximum_threads_per_block\":"
           << device.maximum_threads_per_block
           << ",\"multiprocessor_count\":" << device.multiprocessor_count
           << ",\"maximum_threads_per_multiprocessor\":"
           << device.maximum_threads_per_multiprocessor
           << ",\"registers_per_multiprocessor\":"
           << device.registers_per_multiprocessor
           << ",\"shared_memory_per_block_bytes\":"
           << device.shared_memory_per_block_bytes
           << ",\"shared_memory_per_multiprocessor_bytes\":"
           << device.shared_memory_per_multiprocessor_bytes
           << ",\"warp_size\":" << device.warp_size
           << ",\"clock_rate_khz\":" << device.clock_rate_khz
           << ",\"memory_clock_rate_khz\":" << device.memory_clock_rate_khz
           << ",\"memory_bus_width_bits\":" << device.memory_bus_width_bits
           << ",\"l2_cache_bytes\":" << device.l2_cache_bytes
           << ",\"backend_compatible\":"
           << (device.backend_compatible ? "true" : "false")
           << ",\"incompatibility_reason\":"
           << getnative::benchmark::json_string(device.incompatibility_reason)
           << '}';
}

void append_policy(
    std::ostream &output, const getnative::CudaLaunchPolicy &policy) {
    output << "{\"id\":" << getnative::benchmark::json_string(policy_id(policy))
           << ",\"inverse_threads\":" << policy.inverse_threads
           << ",\"pixel_threads\":" << policy.pixel_threads
           << ",\"maximum_metric_blocks\":" << policy.maximum_metric_blocks
           << ",\"paired_vertical\":"
           << (policy.paired_vertical ? "true" : "false") << '}';
}

void append_validation(std::ostream &output, const ValidationStats &validation) {
    output << "{\"comparison_count\":" << validation.comparison_count
           << ",\"maximum_absolute_error\":" << std::setprecision(17)
           << validation.maximum_absolute_error
           << ",\"maximum_tolerance_ratio\":"
           << validation.maximum_tolerance_ratio << '}';
}

void append_profile(std::ostream &output, const ProfileTotals &profile) {
    const double divisor = static_cast<double>(
        std::max<std::size_t>(1U, profile.measured_frames));
    output << "{\"measured_frames\":" << profile.measured_frames
           << ",\"totals\":{\"kernel_launch_count\":"
           << profile.kernel_launch_count
           << ",\"tile_count\":" << profile.tile_count
           << ",\"buffer_allocation_count\":"
           << profile.buffer_allocation_count
           << ",\"plan_cache_hits\":" << profile.plan_cache_hits
           << ",\"plan_cache_misses\":" << profile.plan_cache_misses
           << ",\"source_upload_bytes\":" << profile.source_upload_bytes
           << ",\"plan_upload_bytes\":" << profile.plan_upload_bytes
           << ",\"result_readback_bytes\":" << profile.result_readback_bytes
           << "},\"memory\":{\"peak_working_set_bytes\":"
           << profile.peak_working_set_bytes
           << ",\"workspace_bytes\":" << profile.workspace_bytes
           << ",\"pinned_staging_bytes\":" << profile.pinned_staging_bytes
           << "},\"per_frame_ms\":{\"execution_slot_wait\":"
           << profile.execution_slot_wait_ms / divisor
           << ",\"host_pack\":" << profile.host_pack_ms / divisor
           << ",\"source_staging\":" << profile.source_staging_ms / divisor
           << ",\"source_upload\":" << profile.source_upload_ms / divisor
           << ",\"plan_upload\":" << profile.plan_upload_ms / divisor
           << ",\"source_transpose\":" << profile.source_transpose_ms / divisor
           << ",\"horizontal_fused\":" << profile.horizontal_fused_ms / divisor
           << ",\"inverse_horizontal\":"
           << profile.inverse_horizontal_ms / divisor
           << ",\"inverse_vertical\":" << profile.inverse_vertical_ms / divisor
           << ",\"forward_intermediate\":"
           << profile.forward_intermediate_ms / divisor
           << ",\"metric\":" << profile.metric_ms / divisor
           << ",\"kernel\":" << profile.kernel_ms / divisor
           << ",\"result_readback\":" << profile.result_readback_ms / divisor
           << ",\"gpu_total\":" << profile.gpu_total_ms / divisor << "}}";
}

void append_workload_definition(
    std::ostream &output, const WorkloadSpec &workload) {
    const std::int32_t support = filter_support(workload.filter_family);
    output << "{\"filter_family\":"
           << getnative::benchmark::json_string(
                  filter_family_name(workload.filter_family))
           << ",\"filter_topology\":"
           << getnative::benchmark::json_string(
                  filter_topology_name(workload.filter_family))
           << ",\"filter_support\":" << support
           << ",\"filter_tap_count\":" << 2 * support
           << ",\"forward_width\":" << 2 * support
           << ",\"inverse_half_bandwidth\":"
           << (support == 0 ? 0 : 2 * support - 1)
           << ",\"inverse_implementation_path\":"
           << getnative::benchmark::json_string(
                  inverse_implementation_path(workload.filter_family))
           << ",\"width\":" << workload.width
           << ",\"height\":" << workload.height
           << ",\"native_width\":" << workload.native_width
           << ",\"native_height\":" << workload.native_height
           << ",\"axes\":"
           << getnative::benchmark::json_string(axes_name(workload.axes))
           << ",\"candidate_count\":" << workload.candidate_count
           << ",\"concurrency\":" << workload.concurrency
           << ",\"iterations_per_worker\":"
           << workload.iterations_per_worker
           << ",\"frames_per_sample\":"
           << workload.concurrency * workload.iterations_per_worker
           << ",\"decision_weight\":" << std::setprecision(17)
           << workload.weight << '}';
}

void append_workload_contract(
    std::ostream &output, const std::vector<WorkloadSpec> &workloads) {
    output << '[';
    for (std::size_t index = 0; index < workloads.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"id\":"
               << getnative::benchmark::json_string(workloads[index].id)
               << ",\"definition\":";
        append_workload_definition(output, workloads[index]);
        output << '}';
    }
    output << ']';
}

void append_filter_family_contract(std::ostream &output) {
    output << '[';
    for (std::size_t index = 0;
         index < benchmark_filter_families.size(); ++index) {
        if (index != 0U) output << ',';
        output << getnative::benchmark::json_string(
            filter_family_name(benchmark_filter_families[index]));
    }
    output << ']';
}

void append_filter_topology_contract(std::ostream &output) {
    output << '[';
    for (std::size_t index = 0;
         index < benchmark_filter_families.size(); ++index) {
        if (index != 0U) output << ',';
        const FilterFamily family = benchmark_filter_families[index];
        const std::int32_t support = filter_support(family);
        output << "{\"representative_filter\":"
               << getnative::benchmark::json_string(filter_family_name(family))
               << ",\"topology\":"
               << getnative::benchmark::json_string(filter_topology_name(family))
               << ",\"support\":" << support
               << ",\"tap_count\":" << 2 * support
               << ",\"forward_width\":" << 2 * support
               << ",\"inverse_half_bandwidth\":" << 2 * support - 1
               << ",\"inverse_implementation_path\":"
               << getnative::benchmark::json_string(
                      inverse_implementation_path(family))
               << '}';
    }
    output << ']';
}

void append_calibration_target_contract(
    std::ostream &output, RunMode mode, bool confirmation) {
    output << '{';
    for (std::size_t index = 0;
         index < benchmark_filter_families.size(); ++index) {
        if (index != 0U) output << ',';
        const FilterFamily family = benchmark_filter_families[index];
        output << getnative::benchmark::json_string(filter_family_name(family))
               << ':' << calibration_target_wave_ms(family, mode, confirmation);
    }
    output << '}';
}

void append_workload_evaluation(
    std::ostream &output, const WorkloadEvaluation &workload) {
    output << "{\"id\":" << getnative::benchmark::json_string(workload.spec.id)
           << ",\"definition\":";
    append_workload_definition(output, workload.spec);
    output << ",\"frame_ms\":";
    getnative::benchmark::append_summary(output, workload.frame_ms);
    output << ",\"p95_frame_ms\":" << workload.p95_frame_ms
           << ",\"frames_per_second\":" << workload.frames_per_second
           << ",\"candidate_analyses_per_second\":"
           << workload.frames_per_second
                  * static_cast<double>(workload.spec.candidate_count)
           << ",\"improvement_vs_baseline\":";
    if (workload.has_comparison) {
        output << workload.improvement_vs_baseline;
    } else {
        output << "null";
    }
    output << ",\"correctness\":";
    append_validation(output, workload.validation);
    output << ",\"profile\":";
    append_profile(output, workload.profile);
    output << '}';
}

void append_policy_evaluation(
    std::ostream &output, const PolicyEvaluation &evaluation) {
    output << "{\"policy\":";
    append_policy(output, evaluation.policy);
    output << ",\"valid\":" << (evaluation.valid ? "true" : "false")
           << ",\"failure\":"
           << getnative::benchmark::json_string(evaluation.failure)
           << ",\"elapsed_seconds\":" << std::setprecision(17)
           << evaluation.elapsed_seconds
           << ",\"comparison\":";
    if (evaluation.has_comparison) {
        output << "{\"composite_improvement\":"
               << evaluation.composite_improvement
               << ",\"worst_workload_improvement\":"
               << evaluation.worst_workload_improvement
               << ",\"maximum_relative_mad\":"
               << evaluation.maximum_relative_mad
               << ",\"required_gain\":" << evaluation.required_gain
               << ",\"decision_admissible\":"
               << (evaluation.decision_admissible ? "true" : "false")
               << ",\"family_comparisons\":[";
        for (std::size_t index = 0;
             index < evaluation.family_comparisons.size(); ++index) {
            if (index != 0U) output << ',';
            const FamilyComparison &comparison =
                evaluation.family_comparisons[index];
            output << "{\"family\":"
                   << getnative::benchmark::json_string(
                          filter_family_name(comparison.family))
                   << ",\"composite_improvement\":"
                   << comparison.composite_improvement
                   << ",\"worst_workload_improvement\":"
                   << comparison.worst_workload_improvement
                   << ",\"maximum_relative_mad\":"
                   << comparison.maximum_relative_mad
                   << ",\"required_gain\":" << comparison.required_gain
                   << ",\"decision_admissible\":"
                   << (comparison.decision_admissible ? "true" : "false")
                   << '}';
        }
        output << "]}";
    } else {
        output << "null";
    }
    output << ",\"workloads\":[";
    for (std::size_t index = 0; index < evaluation.workloads.size(); ++index) {
        if (index != 0U) output << ',';
        append_workload_evaluation(output, evaluation.workloads[index]);
    }
    output << "]}";
}

void append_kernel_resources(
    std::ostream &output,
    const std::vector<getnative::CudaKernelResourceInfo> &resources) {
    output << '[';
    for (std::size_t index = 0; index < resources.size(); ++index) {
        if (index != 0U) output << ',';
        const auto &resource = resources[index];
        output << "{\"name\":"
               << getnative::benchmark::json_string(resource.name)
               << ",\"register_count\":" << resource.register_count
               << ",\"static_shared_bytes\":" << resource.static_shared_bytes
               << ",\"local_bytes\":" << resource.local_bytes
               << ",\"constant_bytes\":" << resource.constant_bytes
               << ",\"binary_version\":" << resource.binary_version
               << ",\"ptx_version\":" << resource.ptx_version << '}';
    }
    output << ']';
}

void append_device_result(
    std::ostream &output, const DeviceTuningResult &result) {
    output << "{\"device\":";
    append_device_info(output, result.device);
    output << ",\"status\":" << getnative::benchmark::json_string(result.status)
           << ",\"error\":" << getnative::benchmark::json_string(result.error)
           << ",\"elapsed_seconds\":" << std::setprecision(17)
           << result.elapsed_seconds
           << ",\"precondition_seconds\":" << result.precondition_seconds
           << ",\"self_test\":{\"passed\":"
           << (result.self_test.passed ? "true" : "false")
           << ",\"repeat_bitwise_stable\":"
           << (result.self_test.repeat_bitwise_stable ? "true" : "false")
           << ",\"resource_gate_passed\":"
           << (result.self_test.resource_gate_passed ? "true" : "false")
           << ",\"correctness\":";
    append_validation(output, result.self_test.validation);
    output << "},\"artifact\":{\"kernel_variant\":"
           << getnative::benchmark::json_string(
                  result.self_test.telemetry.kernel_variant)
           << ",\"stage\":"
           << getnative::benchmark::json_string(
                  result.self_test.telemetry.artifact_stage)
           << ",\"target\":"
           << getnative::benchmark::json_string(
                  result.self_test.telemetry.artifact_target)
           << ",\"name\":"
           << getnative::benchmark::json_string(
                  result.self_test.telemetry.artifact_name)
           << ",\"hash_fnv1a64\":"
           << getnative::benchmark::json_string(
                  result.self_test.telemetry.artifact_hash_fnv1a64)
           << ",\"kernel_resources\":";
    append_kernel_resources(
        output, result.self_test.telemetry.kernel_resources);
    output << "},\"screening\":[";
    for (std::size_t index = 0; index < result.screening.size(); ++index) {
        if (index != 0U) output << ',';
        append_policy_evaluation(output, result.screening[index]);
    }
    output << "],\"confirmation\":{\"baseline_before\":";
    append_policy_evaluation(output, result.confirmation.baseline_before);
    output << ",\"baseline_after\":";
    append_policy_evaluation(output, result.confirmation.baseline_after);
    output << ",\"baseline_aggregate\":";
    append_policy_evaluation(output, result.confirmation.baseline_aggregate);
    output << ",\"finalists\":[";
    for (std::size_t index = 0;
         index < result.confirmation.finalists.size(); ++index) {
        if (index != 0U) output << ',';
        append_policy_evaluation(output, result.confirmation.finalists[index]);
    }
    output << "]},\"family_recommendations\":[";
    for (std::size_t index = 0;
         index < result.family_recommendations.size(); ++index) {
        if (index != 0U) output << ',';
        const FamilyRecommendation &recommendation =
            result.family_recommendations[index];
        output << "{\"family\":"
               << getnative::benchmark::json_string(
                      filter_family_name(recommendation.family))
               << ",\"decision\":"
               << getnative::benchmark::json_string(recommendation.decision)
               << ",\"selected_policy_id\":"
               << getnative::benchmark::json_string(
                      recommendation.selected_policy_id)
               << ",\"provisional_best_policy_id\":"
               << getnative::benchmark::json_string(
                      recommendation.provisional_best_policy_id)
               << ",\"composite_improvement\":"
               << recommendation.composite_improvement
               << ",\"worst_workload_improvement\":"
               << recommendation.worst_workload_improvement
               << ",\"maximum_relative_mad\":"
               << recommendation.maximum_relative_mad
               << ",\"required_gain\":" << recommendation.required_gain
               << ",\"headline_throughput\":{\"workload_id\":"
               << getnative::benchmark::json_string(
                      recommendation.headline_workload_id)
               << ",\"baseline_frames_per_second\":"
               << recommendation.baseline_frames_per_second
               << ",\"selected_frames_per_second\":"
               << recommendation.selected_frames_per_second
               << ",\"provisional_best_frames_per_second\":"
               << recommendation.provisional_best_frames_per_second << '}'
               << ",\"confidence\":"
               << getnative::benchmark::json_string(recommendation.confidence)
               << ",\"benchmark_winner\":"
               << (recommendation.benchmark_winner ? "true" : "false")
               << ",\"production_dispatch_available\":"
               << (recommendation.production_dispatch_available
                       ? "true" : "false")
               << ",\"rationale\":"
               << getnative::benchmark::json_string(recommendation.rationale)
               << '}';
    }
    output << "],\"recommendation\":{\"decision\":"
           << getnative::benchmark::json_string(result.recommendation.decision)
           << ",\"selected_policy_id\":"
           << getnative::benchmark::json_string(
                  result.recommendation.selected_policy_id)
           << ",\"provisional_best_policy_id\":"
           << getnative::benchmark::json_string(
                  result.recommendation.provisional_best_policy_id)
           << ",\"composite_improvement\":"
           << result.recommendation.composite_improvement
           << ",\"worst_workload_improvement\":"
           << result.recommendation.worst_workload_improvement
           << ",\"required_gain\":" << result.recommendation.required_gain
           << ",\"baseline_maximum_drift\":"
           << result.recommendation.baseline_maximum_drift
           << ",\"confidence\":"
           << getnative::benchmark::json_string(result.recommendation.confidence)
           << ",\"safe_to_apply\":"
           << (result.recommendation.safe_to_apply ? "true" : "false")
           << ",\"rationale\":"
           << getnative::benchmark::json_string(result.recommendation.rationale)
           << "}}";
}

[[nodiscard]] std::string make_report_json(const RunReport &report) {
    const auto policies = make_policies(report.config.mode);
    const auto screening_contract =
        make_screening_workload_specs(report.config.mode);
    const auto confirmation_contract =
        make_confirmation_workload_specs(report.config.mode);
    std::ostringstream output;
    output << '{'
           << "\"schema_version\":2"
           << ",\"tool\":" << getnative::benchmark::json_string(tool_name)
           << ",\"tool_version\":"
           << getnative::benchmark::json_string(tool_version)
           << ",\"timestamp_utc\":"
           << getnative::benchmark::json_string(
                  getnative::benchmark::utc_timestamp())
           << ",\"status\":" << getnative::benchmark::json_string(report.status)
           << ",\"run_mode\":"
           << getnative::benchmark::json_string(run_mode_name(report.config.mode))
           << ",\"elapsed_seconds\":" << std::setprecision(17)
           << report.elapsed_seconds
           << ",\"privacy\":{\"network_access\":false"
           << ",\"machine_name_collected\":false"
           << ",\"user_name_collected\":false"
           << ",\"absolute_paths_collected\":false"
           << ",\"gpu_uuid_collected\":false}"
           << ",\"build\":{\"source_identity_sha256\":"
           << getnative::benchmark::json_string(GETNATIVE_BENCHMARK_SOURCE_ID)
           << ",\"build_type\":"
           << getnative::benchmark::json_string(GETNATIVE_BENCHMARK_BUILD_TYPE)
           << ",\"compiler\":"
           << getnative::benchmark::json_string(
                  getnative::benchmark::compiler_identity())
           << ",\"executable_hash_fnv1a64\":"
           << getnative::benchmark::json_string(report.executable_hash_fnv1a64)
           << "},\"protocol\":{\"contract_version\":"
           << getnative::benchmark::json_string(workload_contract_version)
           << ",\"candidate_scope\":\"launch_policy_v1\""
           << ",\"candidate_set_role\":\"benchmark_only\""
           << ",\"candidate_policy_count\":" << policies.size()
           << ",\"challenger_policy_count\":" << policies.size() - 1U
           << ",\"candidate_space_exhaustive\":"
           << (is_standard(report.config.mode) ? "true" : "false")
           << ",\"candidate_sampling_strategy\":"
           << getnative::benchmark::json_string(
                  report.config.mode == RunMode::standard
                      ? "exhaustive_cartesian"
                      : (report.config.mode == RunMode::survey
                             ? "representative_main_effects_plus_known_interactions"
                             : "smoke_subset"))
           << ",\"target_compute_capabilities\":[\"sm_75\",\"sm_86\",\"sm_89\",\"sm_120\"]"
           << ",\"kernel_variant\":\"cpp-generic\""
           << ",\"filter_specific_kernel_variants_included\":false"
           << ",\"filter_kernel_topologies_included\":true"
           << ",\"filter_execution_model\":\"shared_kernel_pipeline_with_filter_specific_axis_plans\""
           << ",\"filter_families\":";
    append_filter_family_contract(output);
    output << ",\"filter_topology_contract\":";
    append_filter_topology_contract(output);
    output << ",\"concurrency_search_included\":false"
           << ",\"concurrency_role\":\"fixed_realistic_workload_dimension\""
           << ",\"iteration_calibration_enabled\":"
           << (is_quick(report.config.mode) ? "false" : "true")
           << ",\"iteration_calibration_policy\":\"baseline_measured_once_then_fixed_across_policies\""
           << ",\"calibration_excluded_from_ranking\":true"
           << ",\"calibration_probe_iterations_per_worker\":"
           << calibration_probe_iterations_per_worker
           << ",\"screening_target_wave_ms_by_filter\":";
    append_calibration_target_contract(output, report.config.mode, false);
    output << ",\"confirmation_target_wave_ms_by_filter\":";
    append_calibration_target_contract(output, report.config.mode, true);
    output
           << ",\"screening_minimum_iterations_per_worker\":"
           << screening_minimum_iterations_per_worker
           << ",\"confirmation_minimum_iterations_per_worker\":"
           << confirmation_minimum_iterations_per_worker
           << ",\"contract_iteration_counts_role\":"
           << getnative::benchmark::json_string(
                  is_quick(report.config.mode)
                      ? "exact" : "precalibration_upper_bound")
           << ",\"optimization_objective\":\"robust_launch_policy_across_filter_topologies\""
           << ",\"timing_scope\":\"steady_state_backend_api_e2e\""
           << ",\"ranking_timer\":\"steady_clock_wall_time\""
           << ",\"sample_unit\":\"wave_wall_time_divided_by_completed_frames\""
           << ",\"frame_definition\":\"one_completed_analyze_axis_batch_f32_call\""
           << ",\"candidate_throughput_rule\":\"FPS_times_candidate_count\""
           << ",\"headline_fps_rule\":\"highest_decision_weight_workload_per_filter_family\""
           << ",\"cuda_events_role\":\"diagnostic_stage_breakdown_not_ranking\""
           << ",\"hardware_performance_counters_used\":false"
           << ",\"timing_includes\":[\"backend_api_call\",\"host_pack\",\"source_staging\",\"H2D\",\"CUDA_pipeline\",\"result_readback\",\"execution_slot_contention\"]"
           << ",\"timing_excludes\":[\"fixture_generation\",\"axis_plan_construction\",\"decode\",\"GUI\",\"file_IO\",\"report_serialization\"]"
           << ",\"screening_warmups\":1"
           << ",\"screening_samples\":"
           << (report.config.mode == RunMode::quick
                   ? 1 : (report.config.mode == RunMode::survey ? 2 : 3))
           << ",\"confirmation_warmups\":"
           << (is_quick(report.config.mode) ? 1 : 2)
           << ",\"confirmation_samples\":"
           << (report.config.mode == RunMode::quick
                   ? 3 : (report.config.mode == RunMode::survey ? 5 : 7))
           << ",\"maximum_confirmation_finalists\":"
           << maximum_confirmation_finalists(report.config.mode)
           << ",\"statistics\":[\"raw_samples\",\"median\",\"MAD\",\"p95\"]"
           << ",\"minimum_precondition_seconds\":"
           << (is_quick(report.config.mode) ? 0.0 : minimum_precondition_seconds)
           << ",\"baseline_bracketing\":true"
           << ",\"search_seed\":" << search_seed
           << ",\"production_configuration_is_modified\":false"
           << ",\"production_dispatch_from_family_winners_available\":false"
           << ",\"candidate_dimensions\":{\"inverse_threads\":[32,64,128]"
           << ",\"pixel_threads\":[128,256]"
           << ",\"maximum_metric_blocks\":[64,128,256]"
           << ",\"paired_vertical\":[false,true]}"
           << ",\"excluded_candidate_dimensions\":[\"concurrency\",\"kernel_implementation\",\"instruction_level_parallelism\",\"inline_ptx\",\"filter_specific_dispatch\"]"
           << ",\"decision_rule\":{\"ranking_metric\":\"weighted_geometric_mean_throughput\""
           << ",\"required_gain\":\"max(0.03,2*maximum_relative_MAD,baseline_maximum_drift)\""
           << ",\"minimum_composite_gain\":" << minimum_composite_gain
           << ",\"maximum_workload_regression\":"
           << maximum_workload_regression
           << ",\"maximum_relative_mad\":0.03"
           << ",\"maximum_baseline_drift\":0.05"
           << ",\"correctness_required\":true"
           << ",\"resource_gate_zero_local_memory_required\":true"
           << ",\"quick_mode_can_recommend\":false"
           << ",\"survey_mode_can_recommend\":false"
           << ",\"standard_mode_can_recommend\":true"
           << ",\"failure_action\":\"retain_baseline\"}"
           << ",\"screening_workload_contract\":";
    append_workload_contract(output, screening_contract);
    output << ",\"confirmation_workload_contract\":";
    append_workload_contract(output, confirmation_contract);
    output << "},\"probe\":{\"driver_loaded\":"
           << (report.probe.driver_loaded ? "true" : "false")
           << ",\"initialized\":"
           << (report.probe.initialized ? "true" : "false")
           << ",\"device_available\":"
           << (report.probe.device_available ? "true" : "false")
           << ",\"reason\":"
           << getnative::benchmark::json_string(report.probe.reason)
           << ",\"devices_seen\":[";
    for (std::size_t index = 0; index < report.probe.devices.size(); ++index) {
        if (index != 0U) output << ',';
        append_device_info(output, report.probe.devices[index]);
    }
    output << "]},\"devices\":[";
    for (std::size_t index = 0; index < report.devices.size(); ++index) {
        if (index != 0U) output << ',';
        append_device_result(output, report.devices[index]);
    }
    output << "]}\n";
    return output.str();
}

[[nodiscard]] std::string compact_utc_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) != 0) {
        throw std::runtime_error("failed to format report timestamp");
    }
#else
    if (gmtime_r(&now, &utc) == nullptr) {
        throw std::runtime_error("failed to format report timestamp");
    }
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return output.str();
}

[[nodiscard]] std::filesystem::path unused_report_path(
    const std::filesystem::path &directory) {
    const std::string stem = std::string{tool_name} + '-'
        + compact_utc_timestamp();
    for (std::size_t suffix = 0; suffix < 1000U; ++suffix) {
        std::string filename = stem;
        if (suffix != 0U) filename += '-' + std::to_string(suffix);
        filename += ".json";
        const std::filesystem::path candidate = directory / filename;
        std::filesystem::path temporary = candidate;
        temporary += ".tmp";
        std::error_code error;
        const bool final_exists = std::filesystem::exists(candidate, error);
        if (error) continue;
        const bool temporary_exists = std::filesystem::exists(temporary, error);
        if (!error && !final_exists && !temporary_exists) return candidate;
    }
    throw std::runtime_error("could not allocate a unique report filename");
}

[[nodiscard]] std::filesystem::path publish_report(
    const Configuration &config,
    const std::filesystem::path &executable,
    std::string_view json) {
    if (config.output_path) {
        getnative::benchmark::atomic_write_json(*config.output_path, json);
        return *config.output_path;
    }

    std::vector<std::filesystem::path> directories;
    directories.push_back(executable.parent_path());
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (!error && current != directories.front()) directories.push_back(current);
    error.clear();
    const auto temporary = std::filesystem::temp_directory_path(error);
    if (!error) directories.push_back(temporary);

    std::string last_error = "no writable report directory was found";
    for (const auto &directory : directories) {
        try {
            const auto output_path = unused_report_path(directory);
            getnative::benchmark::atomic_write_json(output_path, json);
            return output_path;
        } catch (const std::exception &write_error) {
            last_error = write_error.what();
        }
    }
    throw std::runtime_error("failed to publish benchmark report: " + last_error);
}

[[nodiscard]] bool reveal_in_explorer(const std::filesystem::path &report_path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(report_path, error);
    const std::filesystem::path &selected = error ? report_path : absolute;
#if defined(_WIN32)
    std::wstring command = L"explorer.exe /select,\"" + selected.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (created == 0) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#elif defined(__linux__)
    std::string directory = selected.parent_path().string();
    if (directory.empty()) directory = ".";
    char program[] = "xdg-open";
    char *arguments[]{program, directory.data(), nullptr};
    pid_t process = 0;
    if (posix_spawnp(
            &process, program, nullptr, nullptr, arguments, environ) != 0) {
        return false;
    }
    int status = 0;
    while (waitpid(process, &status, 0) == -1) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    (void)selected;
    return false;
#endif
}

[[nodiscard]] std::string percent_text(double ratio) {
    std::ostringstream output;
    output << std::showpos << std::fixed << std::setprecision(2)
           << ratio * 100.0 << '%';
    return output.str();
}

[[nodiscard]] std::string unsigned_percent_text(double ratio) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << ratio * 100.0 << '%';
    return output.str();
}

[[nodiscard]] std::string gibibytes_text(std::size_t bytes) {
    constexpr double bytes_per_gibibyte = 1024.0 * 1024.0 * 1024.0;
    std::ostringstream output;
    output << std::fixed << std::setprecision(1)
           << static_cast<double>(bytes) / bytes_per_gibibyte << " GiB";
    return output.str();
}

[[nodiscard]] std::string fps_text(double frames_per_second) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(
        frames_per_second >= 100.0 ? 1 : 2) << frames_per_second << " FPS";
    return output.str();
}

[[nodiscard]] const getnative::CudaLaunchPolicy *find_policy(
    const DeviceTuningResult &device, std::string_view id) {
    if (id == policy_id(baseline_policy)) return &baseline_policy;
    for (const auto &candidate : device.confirmation.finalists) {
        if (candidate.id == id) return &candidate.policy;
    }
    for (const auto &candidate : device.screening) {
        if (candidate.id == id) return &candidate.policy;
    }
    return nullptr;
}

void print_policy_path(
    const DeviceTuningResult &device, std::string_view policy_id_value) {
    const auto *policy = find_policy(device, policy_id_value);
    if (policy == nullptr) return;
    std::cout << "      inverse=" << policy->inverse_threads << " threads"
              << " | pixel=" << policy->pixel_threads << " threads"
              << " | metric-cap=" << policy->maximum_metric_blocks << " blocks"
              << " | vertical="
              << (policy->paired_vertical ? "paired" : "single") << '\n';
}

void print_family_summary(
    const DeviceTuningResult &device,
    const FamilyRecommendation &recommendation) {
    std::cout << "\n  " << filter_family_display_name(recommendation.family)
              << " descale [" << filter_topology_name(recommendation.family)
              << " | " << inverse_implementation_path(recommendation.family)
              << "]\n";
    if (const WorkloadEvaluation *headline = find_workload(
            device.confirmation.baseline_aggregate,
            recommendation.headline_workload_id)) {
        std::cout << "    Contract: " << headline->spec.width << 'x'
                  << headline->spec.height << " -> "
                  << headline->spec.native_width << 'x'
                  << headline->spec.native_height << " | "
                  << headline->spec.candidate_count << " candidates/frame | "
                  << headline->spec.concurrency << " fixed caller"
                  << (headline->spec.concurrency == 1U ? "" : "s") << '\n';
    }
    if (recommendation.decision == "diagnostic_only"
        || recommendation.decision == "survey_only") {
        std::cout << "    ["
                  << (recommendation.decision == "survey_only"
                          ? "SURVEY" : "DIAGNOSTIC")
                  << "] observed leader: "
                  << recommendation.provisional_best_policy_id
                  << " | "
                  << fps_text(
                         recommendation.provisional_best_frames_per_second)
                  << " vs "
                  << fps_text(recommendation.baseline_frames_per_second)
                  << " baseline | "
                  << percent_text(recommendation.composite_improvement) << '\n'
                  << "      headline: "
                  << recommendation.headline_workload_id << '\n';
        print_policy_path(device, recommendation.provisional_best_policy_id);
        return;
    }
    if (recommendation.benchmark_winner) {
        std::cout << "    [WINNER] " << recommendation.selected_policy_id
                  << " | "
                  << fps_text(recommendation.selected_frames_per_second)
                  << " vs "
                  << fps_text(recommendation.baseline_frames_per_second)
                  << " baseline | composite "
                  << percent_text(recommendation.composite_improvement) << '\n'
                  << "      headline: " << recommendation.headline_workload_id
                  << " | worst case "
                  << percent_text(recommendation.worst_workload_improvement)
                  << " | " << recommendation.confidence << " confidence\n";
        print_policy_path(device, recommendation.selected_policy_id);
        return;
    }
    std::cout << "    [BASELINE RETAINED] no challenger cleared every gate\n";
    if (!recommendation.provisional_best_policy_id.empty()) {
        std::cout << "      observed leader: "
                  << recommendation.provisional_best_policy_id
                  << " | "
                  << fps_text(
                         recommendation.provisional_best_frames_per_second)
                  << " vs "
                  << fps_text(recommendation.baseline_frames_per_second)
                  << " baseline | composite "
                  << percent_text(recommendation.composite_improvement) << '\n'
                  << "      headline: " << recommendation.headline_workload_id
                  << " | worst case "
                  << percent_text(recommendation.worst_workload_improvement)
                  << " | MAD "
                  << unsigned_percent_text(
                         recommendation.maximum_relative_mad) << '\n';
        print_policy_path(device, recommendation.provisional_best_policy_id);
    }
}

void print_device_summary(const DeviceTuningResult &device) {
    constexpr std::string_view rule =
        "==============================================================================";
    std::cout << "\n" << rule << "\n"
              << "  CUDA DESCALE ROUTE MAP // DEVICE " << device.device.ordinal
              << "\n" << rule << "\n"
              << "  GPU          " << device.device.name << '\n'
              << "  Compute      sm_" << device.device.compute_capability_major
              << device.device.compute_capability_minor
              << " | " << device.device.multiprocessor_count << " SMs"
              << " | " << gibibytes_text(device.device.total_memory_bytes) << '\n';
    if (device.status != "complete") {
        std::cout << "  Status       [FAILED] " << device.error << "\n"
                  << rule << '\n';
        return;
    }
    std::int32_t loaded_binary = 0;
    for (const auto &resource :
         device.self_test.telemetry.kernel_resources) {
        loaded_binary = std::max(loaded_binary, resource.binary_version);
    }
    std::cout << "  CUDA image   " << device.self_test.telemetry.artifact_stage
              << " / " << device.self_test.telemetry.kernel_variant;
    if (loaded_binary != 0) std::cout << " / sm_" << loaded_binary;
    std::cout << '\n'
              << "  Validation   [PASS] CPU agreement | bitwise repeat | "
              << device.self_test.telemetry.kernel_resources.size()
              << " kernel resource gates\n"
              << "\n  FILTER ROUTE FINALS";

    for (const auto &recommendation : device.family_recommendations) {
        print_family_summary(device, recommendation);
    }

    std::cout << "\n  OVERALL DECISION\n";
    if (device.recommendation.decision == "candidate_recommended") {
        std::cout << "    [RECOMMENDED] "
                  << device.recommendation.selected_policy_id
                  << " | composite "
                  << percent_text(device.recommendation.composite_improvement)
                  << " | worst case "
                  << percent_text(
                         device.recommendation.worst_workload_improvement)
                  << " | " << device.recommendation.confidence
                  << " confidence\n";
        print_policy_path(device, device.recommendation.selected_policy_id);
    } else if (device.recommendation.decision == "diagnostic_only") {
        std::cout << "    [DIAGNOSTIC ONLY] quick mode never changes the baseline\n";
    } else if (device.recommendation.decision == "survey_only") {
        std::cout << "    [SURVEY ONLY] production baseline "
                  << policy_id(baseline_policy) << " remains selected\n"
                  << "      observed overall leader: "
                  << device.recommendation.provisional_best_policy_id
                  << " | composite "
                  << percent_text(device.recommendation.composite_improvement)
                  << "\n      Run --standard before promoting a policy.\n";
    } else {
        const std::string_view decision_label =
            device.recommendation.decision == "retain_baseline"
            ? "BASELINE RETAINED" : "INCONCLUSIVE";
        std::cout << "    [" << decision_label
                  << "] production baseline " << policy_id(baseline_policy)
                  << " remains selected\n"
                  << "      observed overall leader: "
                  << device.recommendation.provisional_best_policy_id
                  << " | composite "
                  << percent_text(device.recommendation.composite_improvement)
                  << '\n';
    }
    std::cout << "\n  Family routes are benchmark evidence only; this build does not install\n"
              << "  filter-specific production dispatch or modify any configuration.\n"
              << rule << '\n';
}

} // namespace

int main(int argc, char **argv) {
    const bool double_click_mode = argc == 1;
    try {
        const Configuration config = parse_arguments(argc, argv);
        if (config.help) {
            print_help();
            return 0;
        }

        std::cout
            << "==============================================================================\n"
            << "  GETNATIVE CUDA PATHFINDER " << tool_version << '\n'
            << "  B2 + B4 + B6 + B8 + B16 // filter-topology route race\n"
            << "==============================================================================\n"
            << "  Read-only: no network, clock, power, driver, or config changes.\n"
            << "  Mode: " << run_mode_name(config.mode)
            << " | Primary score: fixed-contract backend E2E FPS\n"
            << "  Concurrency is part of each workload; it is measured, not searched.\n"
            << "  Target architectures: sm_75, sm_86, sm_89, sm_120.\n"
            << "  Keep other GPU workloads closed. "
            << (config.mode == RunMode::standard
                    ? "Standard mode takes several minutes.\n"
                    : (config.mode == RunMode::survey
                           ? "Survey mode is representative and evidence-only.\n"
                           : "Quick mode is a short smoke test.\n"))
            << "==============================================================================\n"
            << std::endl;

        const auto run_begin = std::chrono::steady_clock::now();
        const auto executable = getnative::benchmark::executable_path(argv[0]);
        RunReport report;
        report.config = config;
        report.executable_hash_fnv1a64 =
            getnative::benchmark::fnv1a64_file(executable);
        report.probe = getnative::cuda_runtime_probe();

        const auto screening_specs = make_screening_workload_specs(config.mode);
        const auto confirmation_specs =
            make_confirmation_workload_specs(config.mode);
        validate_workload_contract(screening_specs, "screening");
        validate_workload_contract(confirmation_specs, "confirmation");
        std::vector<Fixture> screening_fixtures;
        std::vector<Fixture> confirmation_fixtures;
        bool needs_fixtures = false;
        for (const auto &device : report.probe.devices) {
            if (device.backend_compatible
                && target_architecture_supported(device)
                && (!config.device_ordinal
                    || device.ordinal == *config.device_ordinal)) {
                needs_fixtures = true;
                break;
            }
        }
        if (needs_fixtures) {
            std::cout << "Preparing five filter-topology workload suites..."
                      << std::endl;
            screening_fixtures.reserve(screening_specs.size());
            for (const auto &spec : screening_specs) {
                screening_fixtures.push_back(make_fixture(spec));
            }
            confirmation_fixtures.reserve(confirmation_specs.size());
            for (const auto &spec : confirmation_specs) {
                confirmation_fixtures.push_back(make_fixture(spec));
            }
        }

        for (const auto &device : report.probe.devices) {
            if (!device.backend_compatible
                || !target_architecture_supported(device)) continue;
            if (config.device_ordinal && device.ordinal != *config.device_ordinal) continue;
            report.devices.push_back(tune_device(
                device, screening_fixtures, confirmation_fixtures, config));
            print_device_summary(report.devices.back());
        }

        if (report.devices.empty()) {
            std::cout
                << "\n==============================================================================\n"
                << "  NO COMPATIBLE CUDA DEVICE WAS BENCHMARKED\n"
                << "==============================================================================\n"
                << "  " << report.probe.reason << '\n';
            for (const auto &device : report.probe.devices) {
                std::cout << "  Device " << device.ordinal << ": " << device.name
                          << " - "
                          << (!device.backend_compatible
                                  ? device.incompatibility_reason
                                  : (!target_architecture_supported(device)
                                         ? "outside target set sm_75/sm_86/sm_89/sm_120"
                                         : "not selected"))
                          << '\n';
            }
        }

        const std::size_t complete_count = static_cast<std::size_t>(std::count_if(
            report.devices.begin(), report.devices.end(), [](const auto &device) {
                return device.status == "complete";
            }));
        if (report.devices.empty()) {
            report.status = "no_compatible_device";
        } else if (complete_count == report.devices.size()) {
            report.status = "complete";
        } else if (complete_count != 0U) {
            report.status = "partial";
        } else {
            report.status = "failed";
        }
        report.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_begin).count();

        const std::string json = make_report_json(report);
        const auto report_path = publish_report(config, executable, json);
        std::cout << "\nReport saved:\n" << report_path.string() << "\n\n"
                  << "Send this JSON for analysis. It contains no user name, "
                     "machine name, GPU UUID, or absolute path.\n";
        if (!config.no_open && !reveal_in_explorer(report_path)) {
            std::cout << "Explorer could not be opened automatically.\n";
        }
        if (!config.no_pause && double_click_mode) {
            std::cout << "\nPress Enter to close..." << std::flush;
            std::cin.get();
        }
        return complete_count != 0U ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "\nCUDA AutoTune failed: " << error.what() << '\n';
        if (double_click_mode) {
            std::cerr << "Press Enter to close..." << std::flush;
            std::cin.get();
        }
        return 1;
    }
}

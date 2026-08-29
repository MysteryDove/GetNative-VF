// Memory-oriented dual-shape CPU benchmark (v4).
//
// The frontend arms isolate request generation, cold/warm planning, execution,
// and request-to-execution work. The verification arm uses a bounded,
// decorrelated source ring and persistent worker threads/workspaces. Memory
// snapshots are taken outside owning scopes so baseline and teardown are
// comparable. Formal timing is a separate warmup-plus-raw-samples mode.

#include "benchmark_support.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/candidate_grid.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/cpu_features.hpp"
#include "getnative/filter.hpp"
#include "getnative/joining_thread.hpp"

#include "inverse_columns.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

enum class Suite : std::uint8_t {
    all,
    height_search_typical,
    height_envelope,
    verification,
};

enum class Arm : std::uint8_t {
    all,
    cold,
    warm,
    mixed,
    exec,
    e2e,
};

enum class Cohort : std::uint8_t {
    both,
    structured,
    decorrelated,
};

struct NamedFilter {
    std::string_view id;
    getnative::Filter filter;
};

constexpr NamedFilter k_filters[] = {
    {"bilinear", getnative::Filter::bilinear()},
    {"bicubic-catrom", getnative::Filter::bicubic(0.0, 0.5)},
    {"spline36", getnative::Filter::spline36()},
    {"lanczos3", getnative::Filter::lanczos(3)},
    {"lanczos5", getnative::Filter::lanczos(5)},
    {"lanczos8", getnative::Filter::lanczos(8)},
};

struct ProcessMemorySnapshot {
    bool captured = false;
    std::uint64_t working_set_bytes = 0;
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t page_fault_count = 0;
};

[[nodiscard]] ProcessMemorySnapshot capture_process_memory() {
    ProcessMemorySnapshot snap{};
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters))) {
        snap.captured = true;
        snap.working_set_bytes = counters.WorkingSetSize;
        snap.peak_working_set_bytes = counters.PeakWorkingSetSize;
        snap.private_bytes = counters.PrivateUsage;
        snap.page_fault_count = counters.PageFaultCount;
    }
#endif
    return snap;
}

// Human-readable markers. OutputDebugString is useful to debugger-aware tools,
// but it is not treated as a profiler range API.
void phase_marker(std::string_view name) {
#if defined(_WIN32)
    std::wstring wide(name.begin(), name.end());
    ::OutputDebugStringW((L"GETNATIVE_PHASE " + wide).c_str());
#endif
    std::cout << "PHASE " << name << std::endl;
    std::cout.flush();
}

class MeasurementGate {
  public:
    explicit MeasurementGate(bool enabled) : enabled_(enabled) {}

    void wait(std::string_view measured_region) {
        if (!enabled_ || consumed_)
            return;
        consumed_ = true;
        phase_marker(std::string{measured_region} + "_attach_ready");
        std::cout << "ATTACH_GATE: setup is complete; attach the profiler, then "
                     "press Enter to start the measured region..."
                  << std::endl;
        std::cout.flush();
        std::string line;
        std::getline(std::cin, line);
        phase_marker(std::string{measured_region} + "_attach_released");
    }

  private:
    bool enabled_ = false;
    bool consumed_ = false;
};

[[nodiscard]] std::uint64_t detect_last_level_cache_bytes() {
#if defined(_WIN32)
    DWORD bytes = 0;
    if (::GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return 0;
    }
    std::vector<unsigned char> storage(bytes);
    auto *buffer = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
    if (!::GetLogicalProcessorInformationEx(RelationCache, buffer, &bytes)) {
        return 0;
    }

    BYTE highest_level = 0;
    for (DWORD offset = 0; offset < bytes;) {
        const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(
            storage.data() + offset);
        if (entry->Size == 0 || entry->Size > bytes - offset)
            return 0;
        if (entry->Relationship == RelationCache) {
            highest_level = std::max(highest_level, entry->Cache.Level);
        }
        offset += entry->Size;
    }

    std::uint64_t total = 0;
    for (DWORD offset = 0; offset < bytes;) {
        const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(
            storage.data() + offset);
        if (entry->Size == 0 || entry->Size > bytes - offset)
            return 0;
        if (entry->Relationship == RelationCache && entry->Cache.Level == highest_level) {
            total += static_cast<std::uint64_t>(entry->Cache.CacheSize);
        }
        offset += entry->Size;
    }
    return total;
#else
    return 0;
#endif
}

struct Configuration {
    Suite suite = Suite::all;
    Arm arm = Arm::all;
    Cohort cohort = Cohort::both;
    std::optional<std::string> filter_id; // nullopt = all filters
    std::filesystem::path artifact_root;
    std::int32_t source_width = 1920;
    std::int32_t source_height = 1080;
    std::int32_t locked_native_height = 810;
    std::int32_t candidate_count = 500;
    std::int32_t envelope_height_lo = 360;
    std::int32_t envelope_height_hi = 900;
    std::size_t logical_frames = 1000;
    std::size_t ring_frames = 32;
    std::size_t candidate_workers = 0;
    std::size_t frame_workers = 1;
    std::size_t timing_samples = 1; // formal timing: >=21
    getnative::CpuIsaRequest cpu_isa = getnative::CpuIsaRequest::automatic;
    std::uint64_t seed = 0xA11C0FFEEULL;
    bool include_execution_on_frontend = true;
    bool assert_correctness = false;
    bool list_only = false;
    bool profile_mode = false;
    bool attach_gate = false;
    bool require_cache_pressure = false;
    bool suite_explicit = false;
    bool arm_explicit = false;
    bool cohort_explicit = false;
    std::uint64_t detected_llc_bytes = 0;
    double ring_to_llc_ratio = 0.0;
    bool ring_pressure_target_met = false;
};

[[nodiscard]] Suite parse_suite(std::string_view value) {
    if (value == "all")
        return Suite::all;
    if (value == "height-search-typical" || value == "h500-typical") {
        return Suite::height_search_typical;
    }
    if (value == "height-envelope" || value == "h500-envelope") {
        return Suite::height_envelope;
    }
    if (value == "verification" || value == "v1000-steady") {
        return Suite::verification;
    }
    throw std::invalid_argument("unknown --suite");
}

[[nodiscard]] Arm parse_arm(std::string_view value) {
    if (value == "all")
        return Arm::all;
    if (value == "cold")
        return Arm::cold;
    if (value == "warm")
        return Arm::warm;
    if (value == "mixed")
        return Arm::mixed;
    if (value == "exec")
        return Arm::exec;
    if (value == "e2e")
        return Arm::e2e;
    throw std::invalid_argument("unknown --arm (all|cold|warm|mixed|exec|e2e)");
}

[[nodiscard]] Cohort parse_cohort(std::string_view value) {
    if (value == "both")
        return Cohort::both;
    if (value == "structured")
        return Cohort::structured;
    if (value == "decorrelated")
        return Cohort::decorrelated;
    throw std::invalid_argument("unknown --cohort (both|structured|decorrelated)");
}

[[nodiscard]] std::string suite_name(Suite suite) {
    switch (suite) {
    case Suite::all:
        return "all";
    case Suite::height_search_typical:
        return "height-search-typical";
    case Suite::height_envelope:
        return "height-envelope";
    case Suite::verification:
        return "verification";
    }
    return "all";
}

[[nodiscard]] std::string arm_name(Arm arm) {
    switch (arm) {
    case Arm::all:
        return "all";
    case Arm::cold:
        return "cold";
    case Arm::warm:
        return "warm";
    case Arm::mixed:
        return "mixed";
    case Arm::exec:
        return "exec";
    case Arm::e2e:
        return "e2e";
    }
    return "all";
}

[[nodiscard]] std::string cohort_name(Cohort cohort) {
    switch (cohort) {
    case Cohort::both:
        return "both";
    case Cohort::structured:
        return "structured";
    case Cohort::decorrelated:
        return "decorrelated";
    }
    return "both";
}

[[nodiscard]] const NamedFilter *find_filter(std::string_view id) {
    for (const NamedFilter &filter : k_filters) {
        if (filter.id == id)
            return &filter;
    }
    return nullptr;
}

[[nodiscard]] std::vector<const NamedFilter *> selected_filters(const Configuration &config) {
    std::vector<const NamedFilter *> result;
    if (config.filter_id) {
        const NamedFilter *found = find_filter(*config.filter_id);
        if (!found) {
            throw std::invalid_argument("unknown --filter id (bilinear|bicubic-catrom|spline36|"
                                        "lanczos3|lanczos5|lanczos8)");
        }
        result.push_back(found);
        return result;
    }
    for (const NamedFilter &filter : k_filters)
        result.push_back(&filter);
    return result;
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration config{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        auto need = [&](const char *flag) -> const char * {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(flag) + " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--suite") {
            config.suite = parse_suite(need("--suite"));
            config.suite_explicit = true;
        } else if (argument == "--arm") {
            config.arm = parse_arm(need("--arm"));
            config.arm_explicit = true;
        } else if (argument == "--cohort") {
            config.cohort = parse_cohort(need("--cohort"));
            config.cohort_explicit = true;
        } else if (argument == "--filter") {
            config.filter_id = std::string{need("--filter")};
        } else if (argument == "--artifact-root") {
            config.artifact_root = need("--artifact-root");
        } else if (argument == "--source-width") {
            config.source_width = std::stoi(need("--source-width"));
        } else if (argument == "--source-height") {
            config.source_height = std::stoi(need("--source-height"));
        } else if (argument == "--locked-native-height") {
            config.locked_native_height = std::stoi(need("--locked-native-height"));
        } else if (argument == "--candidates") {
            config.candidate_count = std::stoi(need("--candidates"));
        } else if (argument == "--envelope-height-lo") {
            config.envelope_height_lo = std::stoi(need("--envelope-height-lo"));
        } else if (argument == "--envelope-height-hi") {
            config.envelope_height_hi = std::stoi(need("--envelope-height-hi"));
        } else if (argument == "--logical-frames") {
            config.logical_frames = static_cast<std::size_t>(std::stoull(need("--logical-frames")));
        } else if (argument == "--ring-frames") {
            config.ring_frames = static_cast<std::size_t>(std::stoull(need("--ring-frames")));
        } else if (argument == "--candidate-workers") {
            config.candidate_workers =
                static_cast<std::size_t>(std::stoull(need("--candidate-workers")));
        } else if (argument == "--frame-workers") {
            config.frame_workers = static_cast<std::size_t>(std::stoull(need("--frame-workers")));
        } else if (argument == "--timing-samples") {
            config.timing_samples = static_cast<std::size_t>(std::stoull(need("--timing-samples")));
        } else if (argument == "--cpu-isa") {
            const auto parsed = getnative::parse_cpu_isa_request(need("--cpu-isa"));
            if (!parsed)
                throw std::invalid_argument("unknown --cpu-isa");
            config.cpu_isa = *parsed;
        } else if (argument == "--seed") {
            config.seed = std::stoull(need("--seed"));
        } else if (argument == "--no-frontend-execution") {
            config.include_execution_on_frontend = false;
        } else if (argument == "--profile-mode") {
            config.profile_mode = true;
        } else if (argument == "--attach-gate") {
            config.attach_gate = true;
        } else if (argument == "--require-cache-pressure") {
            config.require_cache_pressure = true;
        } else if (argument == "--assert") {
            config.assert_correctness = true;
        } else if (argument == "--list") {
            config.list_only = true;
        } else if (argument == "--help") {
            std::cout
                << "usage: getnative_cpu_memory_profile_benchmark --artifact-root PATH\n"
                   "  [--suite all|height-search-typical|height-envelope|verification]\n"
                   "  [--filter ID] [--cohort both|structured|decorrelated]\n"
                   "  [--arm all|cold|warm|mixed|exec|e2e]\n"
                   "  [--candidates N] [--logical-frames N] [--ring-frames N]\n"
                   "  [--frame-workers N] [--candidate-workers N]\n"
                   "  [--timing-samples N]  # formal timing: >=21\n"
                   "  [--profile-mode] [--attach-gate] [--require-cache-pressure]\n"
                   "  [--cpu-isa ...] [--assert]\n"
                   "\n"
                   "profile-mode: defaults to verification/exec/decorrelated only when\n"
                   "  suite/arm/cohort were not explicitly supplied.\n"
                   "Direct profiler launch attributes the whole process.\n"
                   "Attach capture: one --filter + suite + non-all arm (+ one verify cohort).\n"
                   "Formal timing: separate run with --timing-samples 21 (no profiler).\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + std::string{argument});
        }
    }

    // Profile defaults apply only to omitted arguments; explicit choices win.
    if (config.profile_mode) {
        if (!config.suite_explicit) {
            config.suite = Suite::verification;
        }
        if (config.suite == Suite::verification && !config.cohort_explicit) {
            config.cohort = Cohort::decorrelated;
        }
        if (config.suite == Suite::verification && !config.arm_explicit) {
            config.arm = Arm::exec;
        }
        if (config.include_execution_on_frontend && config.arm != Arm::exec &&
            config.arm != Arm::e2e && config.arm != Arm::all &&
            (config.suite == Suite::height_search_typical ||
             config.suite == Suite::height_envelope)) {
            // cold/warm/mixed planner arms do not need frontend execution.
            if (config.arm == Arm::cold || config.arm == Arm::warm || config.arm == Arm::mixed) {
                config.include_execution_on_frontend = false;
            }
        }
    }

    if (!config.list_only && config.artifact_root.empty()) {
        throw std::invalid_argument("--artifact-root is required");
    }
    if (config.source_width < 1 || config.source_height < 1 || config.candidate_count < 1 ||
        config.locked_native_height < 1) {
        throw std::invalid_argument("invalid candidate/native settings");
    }
    if (config.envelope_height_lo < 1 || config.envelope_height_hi < config.envelope_height_lo) {
        throw std::invalid_argument("invalid envelope height range");
    }
    if (config.logical_frames < 1 || config.ring_frames < 1) {
        throw std::invalid_argument("invalid frame/ring settings");
    }
    if (config.ring_frames > config.logical_frames) {
        config.ring_frames = config.logical_frames;
    }
    if (config.frame_workers < 1) {
        throw std::invalid_argument("--frame-workers must be >= 1");
    }
    if (config.timing_samples < 1) {
        throw std::invalid_argument("--timing-samples must be >= 1");
    }

    const bool verification_only = config.suite == Suite::verification;
    if (verification_only && config.arm != Arm::all && config.arm != Arm::exec) {
        throw std::invalid_argument("verification supports --arm exec only (or legacy --arm all)");
    }
    const bool capture_mode = config.profile_mode || config.attach_gate;
    if (capture_mode && !config.list_only) {
        if (!config.filter_id) {
            throw std::invalid_argument("profile/attach capture requires one explicit --filter");
        }
        if (config.suite == Suite::all) {
            throw std::invalid_argument(
                "profile/attach capture requires one suite, not --suite all");
        }
        if (verification_only && config.arm != Arm::exec) {
            throw std::invalid_argument("profile/attach verification requires --arm exec");
        }
        if (!verification_only && (!config.arm_explicit || config.arm == Arm::all)) {
            throw std::invalid_argument(
                "frontend profile/attach capture requires one explicit non-all arm");
        }
        if (config.attach_gate && verification_only && config.cohort == Cohort::both) {
            throw std::invalid_argument("--attach-gate verification requires one cohort, not both");
        }
        if (config.timing_samples != 1) {
            throw std::invalid_argument("profile/attach capture requires --timing-samples 1");
        }
    }
    if (config.timing_samples > 1) {
        if (!verification_only || !config.filter_id) {
            throw std::invalid_argument(
                "multi-sample timing requires verification and one --filter");
        }
        if (config.arm != Arm::exec) {
            throw std::invalid_argument("multi-sample timing requires --arm exec");
        }
        if (config.attach_gate || config.profile_mode) {
            throw std::invalid_argument("multi-sample timing cannot run with profiler/attach mode");
        }
    }
    if (config.suite == Suite::height_envelope &&
        (config.arm == Arm::exec || config.arm == Arm::e2e)) {
        throw std::invalid_argument(
            "height-envelope is planner-only; exec/e2e belongs to height-search-typical");
    }
    const bool runs_verification =
        config.suite == Suite::verification ||
        (config.suite == Suite::all && (config.arm == Arm::all || config.arm == Arm::exec));
    if (config.require_cache_pressure && !runs_verification) {
        throw std::invalid_argument(
            "--require-cache-pressure applies only to a verification execution");
    }

    config.detected_llc_bytes = detect_last_level_cache_bytes();
    const long double frame_bytes = static_cast<long double>(config.source_width) *
                                    static_cast<long double>(config.source_height) * sizeof(float);
    const long double ring_bytes = frame_bytes * static_cast<long double>(config.ring_frames);
    if (config.detected_llc_bytes != 0) {
        config.ring_to_llc_ratio =
            static_cast<double>(ring_bytes / static_cast<long double>(config.detected_llc_bytes));
        config.ring_pressure_target_met = config.ring_to_llc_ratio >= 2.0;
    }
    if (config.require_cache_pressure &&
        (!config.ring_pressure_target_met || config.detected_llc_bytes == 0)) {
        throw std::invalid_argument(
            "source ring does not meet the required 2x detected-LLC target");
    }
    return config;
}

[[nodiscard]] double elapsed_ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t &state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] std::string precise_decimal(double value) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

[[nodiscard]] std::vector<getnative::AxisPlanRequest>
make_typical_requests(const Configuration &config, const getnative::Filter &filter) {
    const double step = 1.0 / (static_cast<double>(config.candidate_count) + 1.0);
    const getnative::CandidateGridSpec grid{
        precise_decimal(static_cast<double>(config.locked_native_height) + step),
        precise_decimal(step),
        static_cast<std::size_t>(config.candidate_count),
    };
    const auto candidates =
        getnative::generate_candidates(grid, getnative::GridSemantics::index_multiplication);

    std::vector<getnative::AxisPlanRequest> requests;
    requests.reserve(candidates.size());
    for (const getnative::Candidate &candidate : candidates) {
        requests.push_back(getnative::AxisPlanRequest{
            config.source_height,
            config.locked_native_height,
            candidate.value,
            0.0,
            filter,
            getnative::BorderMode::mirror,
        });
    }
    return requests;
}

[[nodiscard]] std::vector<getnative::AxisPlanRequest>
make_envelope_requests(const Configuration &config, const getnative::Filter &filter) {
    const std::int64_t available_heights = static_cast<std::int64_t>(config.envelope_height_hi) -
                                           static_cast<std::int64_t>(config.envelope_height_lo) + 1;
    if (static_cast<std::int64_t>(config.candidate_count) > available_heights) {
        throw std::invalid_argument(
            "height-envelope requires candidate_count <= distinct height count");
    }
    std::vector<std::int32_t> heights;
    heights.reserve(static_cast<std::size_t>(config.candidate_count));
    const double step =
        config.candidate_count == 1
            ? 0.0
            : static_cast<double>(static_cast<std::int64_t>(config.envelope_height_hi) -
                                  static_cast<std::int64_t>(config.envelope_height_lo)) /
                  static_cast<double>(config.candidate_count - 1);
    const getnative::CandidateGridSpec grid{
        std::to_string(config.envelope_height_lo),
        precise_decimal(step),
        static_cast<std::size_t>(config.candidate_count),
    };
    const auto candidates =
        getnative::generate_candidates(grid, getnative::GridSemantics::index_multiplication);
    for (const getnative::Candidate &candidate : candidates) {
        heights.push_back(static_cast<std::int32_t>(std::lround(candidate.value)));
    }
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    if (heights.size() != static_cast<std::size_t>(config.candidate_count) ||
        heights.front() < config.envelope_height_lo || heights.back() > config.envelope_height_hi) {
        throw std::runtime_error(
            "candidate grid did not produce the requested distinct envelope heights");
    }
    std::vector<getnative::AxisPlanRequest> requests;
    requests.reserve(heights.size());
    for (const std::int32_t native : heights) {
        requests.push_back(getnative::AxisPlanRequest{
            config.source_height,
            native,
            static_cast<double>(native),
            0.0,
            filter,
            getnative::BorderMode::mirror,
        });
    }
    return requests;
}

void fill_structured_frame(std::vector<float> &pixels, std::int32_t width, std::int32_t height,
                           std::int32_t stride, std::uint64_t frame_index) {
    for (std::int32_t row = 0; row < height; ++row) {
        float *line = pixels.data() + static_cast<std::ptrdiff_t>(row) * stride;
        for (std::int32_t column = 0; column < width; ++column) {
            const float gx = static_cast<float>(column) / static_cast<float>(width);
            const float gy = static_cast<float>(row) / static_cast<float>(height);
            const float edge =
                ((column + static_cast<std::int32_t>(frame_index)) % 64 < 2) ? 0.35F : 0.0F;
            const float texture = 0.08F * std::sin(0.073F * static_cast<float>(row) +
                                                   0.091F * static_cast<float>(column) +
                                                   0.01F * static_cast<float>(frame_index));
            line[column] = 0.25F + 0.45F * gx + 0.20F * gy + edge + texture;
        }
    }
}

void fill_decorrelated_frame(std::vector<float> &pixels, std::int32_t width, std::int32_t height,
                             std::int32_t stride, std::uint64_t seed, std::uint64_t frame_index) {
    std::uint64_t state = seed ^ (0xD1B54A32D192ED03ULL * (frame_index + 1U));
    for (std::int32_t row = 0; row < height; ++row) {
        float *line = pixels.data() + static_cast<std::ptrdiff_t>(row) * stride;
        for (std::int32_t column = 0; column < width; ++column) {
            const std::uint32_t bits = static_cast<std::uint32_t>(splitmix64(state));
            line[column] = static_cast<float>(bits & 0xFFFFFFu) * (1.0F / 16777216.0F);
        }
    }
}

struct SourceRing {
    std::vector<std::vector<float>> frames;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::size_t prime_step = 1;

    [[nodiscard]] std::size_t physical_count() const { return frames.size(); }
    [[nodiscard]] std::uint64_t physical_bytes() const {
        return static_cast<std::uint64_t>(frames.size()) * static_cast<std::uint64_t>(width) *
               static_cast<std::uint64_t>(height) * sizeof(float);
    }
    [[nodiscard]] std::size_t map_logical(std::size_t logical_index) const {
        if (frames.empty())
            return 0;
        return (logical_index * prime_step) % frames.size();
    }
    [[nodiscard]] getnative::ConstImageView view(std::size_t logical_index) const {
        const std::size_t physical = map_logical(logical_index);
        return getnative::ConstImageView{
            frames[physical].data(),
            width,
            height,
            width,
        };
    }
};

[[nodiscard]] std::size_t choose_prime_step(std::size_t ring) {
    static constexpr std::size_t primes[] = {
        3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
    };
    for (const std::size_t prime : primes) {
        if (prime < ring && (ring % prime) != 0)
            return prime;
    }
    return 1;
}

[[nodiscard]] SourceRing make_source_ring(const Configuration &config, bool decorrelated) {
    SourceRing ring{};
    ring.width = config.source_width;
    ring.height = config.source_height;
    ring.frames.resize(config.ring_frames);
    ring.prime_step = choose_prime_step(config.ring_frames);
    const std::size_t elements =
        static_cast<std::size_t>(config.source_width)
        * static_cast<std::size_t>(config.source_height);
    for (std::size_t index = 0; index < config.ring_frames; ++index) {
        ring.frames[index].assign(elements, 0.0F);
        if (decorrelated) {
            fill_decorrelated_frame(ring.frames[index], config.source_width, config.source_height,
                                    config.source_width, config.seed, index);
        } else {
            fill_structured_frame(ring.frames[index], config.source_width, config.source_height,
                                  config.source_width, index);
        }
    }
    return ring;
}

// Statistics only: this type never retains plan handles.
struct PlanBatchStats {
    double wall_ms = 0.0;
    std::size_t unique_key_count = 0;
    std::size_t physical_build_count = 0;
    std::size_t published_plan_count = 0;
    std::size_t ready_hit_count = 0;
    std::size_t peak_active_builds = 0;
    std::size_t effective_worker_count = 0;
    std::size_t resident_entries = 0;
    std::size_t resident_bytes = 0;
    double mean_plan_bytes = 0.0;
    double p95_plan_bytes = 0.0;
    std::size_t returned_plan_count = 0;
};

struct PlanBatchLive {
    PlanBatchStats stats{};
    // Transient handles for execution only; must be cleared after use.
    std::vector<std::shared_ptr<const getnative::AxisPlan>> plans;
};

[[nodiscard]] PlanBatchLive
measure_plan_batch(getnative::AxisPlanCache &cache,
                   const std::vector<getnative::AxisPlanRequest> &requests, std::size_t workers,
                   bool retain_plans_for_exec) {
    PlanBatchLive live{};
    const auto begin = Clock::now();
    auto batch = cache.get_or_build_batch(requests, workers);
    live.stats.wall_ms = elapsed_ms(begin, Clock::now());
    live.stats.unique_key_count = batch.unique_key_count;
    live.stats.physical_build_count = batch.physical_build_count;
    live.stats.published_plan_count = batch.published_plan_count;
    live.stats.ready_hit_count = batch.ready_hit_count;
    live.stats.peak_active_builds = batch.peak_active_builds;
    live.stats.effective_worker_count = batch.effective_worker_count;
    live.stats.resident_entries = batch.resident_entry_count;
    live.stats.resident_bytes = batch.resident_bytes;
    live.stats.returned_plan_count = batch.plans.size();

    std::vector<double> sizes;
    sizes.reserve(batch.plans.size());
    double sum = 0.0;
    for (const auto &plan : batch.plans) {
        const double bytes = static_cast<double>(getnative::axis_plan_storage_bytes(*plan));
        sizes.push_back(bytes);
        sum += bytes;
    }
    if (!sizes.empty()) {
        live.stats.mean_plan_bytes = sum / static_cast<double>(sizes.size());
        std::sort(sizes.begin(), sizes.end());
        const std::size_t p95_index =
            static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(sizes.size()))) - 1U;
        live.stats.p95_plan_bytes = sizes[std::min(p95_index, sizes.size() - 1U)];
    }
    if (retain_plans_for_exec) {
        live.plans = std::move(batch.plans);
    } else {
        batch.plans.clear(); // drop overflow refs immediately
    }
    return live;
}

struct ExecutionStats {
    double wall_ms = 0.0;
    double checksum = 0.0;
    bool finite = true;
    std::size_t result_count = 0;
    std::size_t nonzero_error_count = 0;
    double first_error = 0.0;
    double last_error = 0.0;
    bool sampled_scalar_reference_matches = false;
    // Ordered fingerprint: xor of bit patterns in candidate index order.
    std::uint64_t ordered_fingerprint = 0;
};

[[nodiscard]] ExecutionStats
run_candidate_execution(const Configuration &config, const getnative::ConstImageView &source,
                        const std::vector<std::shared_ptr<const getnative::AxisPlan>> &plans) {
    ExecutionStats stats{};
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(plans.size());
    for (std::size_t index = 0; index < plans.size(); ++index) {
        candidates.push_back(getnative::CandidateAnalysis{
            std::to_string(index),
            nullptr,
            plans[index],
            getnative::AnalysisAxes::vertical,
        });
    }
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    const auto policy = getnative::detail::column_dispatch_policy(config.cpu_isa);

    const auto begin = Clock::now();
    const auto results = getnative::detail::analyze_batch_with_column_policy_f32(
        source, candidates, metric, policy, config.candidate_workers, 0);
    stats.wall_ms = elapsed_ms(begin, Clock::now());
    stats.result_count = results.size();
    if (!results.empty()) {
        stats.first_error = results.front().error;
        stats.last_error = results.back().error;
    }
    // Results retain input order per public API.
    for (std::size_t index = 0; index < results.size(); ++index) {
        const double error = results[index].error;
        if (!std::isfinite(error))
            stats.finite = false;
        if (error != 0.0)
            ++stats.nonzero_error_count;
        stats.checksum += error;
        const auto bits = std::bit_cast<std::uint64_t>(error);
        stats.ordered_fingerprint ^= bits + 0x9E3779B97F4A7C15ULL +
                                     (stats.ordered_fingerprint << 6U) +
                                     (stats.ordered_fingerprint >> 2U);
        stats.ordered_fingerprint += static_cast<std::uint64_t>(index);
    }
    return stats;
}

[[nodiscard]] bool approximately_equal(double actual, double reference) {
    const double scale = std::max(std::abs(actual), std::abs(reference));
    return std::abs(actual - reference) <= 1.0e-7 + 1.0e-5 * scale;
}

void validate_candidate_execution_sample(
    const getnative::ConstImageView &source,
    const std::vector<std::shared_ptr<const getnative::AxisPlan>> &plans, ExecutionStats &stats) {
    if (plans.empty() || stats.result_count != plans.size())
        return;
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    const auto scalar_policy =
        getnative::detail::column_dispatch_policy(getnative::CpuIsaRequest::scalar);
    getnative::CpuWorkspace workspace;
    const double first = getnative::detail::analyze_axis_candidate_with_column_policy_f32(
        source, *plans.front(), getnative::AnalysisAxes::vertical, metric, workspace,
        scalar_policy);
    const double last = plans.size() == 1
                            ? first
                            : getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                                  source, *plans.back(), getnative::AnalysisAxes::vertical, metric,
                                  workspace, scalar_policy);
    stats.sampled_scalar_reference_matches = approximately_equal(stats.first_error, first) &&
                                             approximately_equal(stats.last_error, last);
}

struct FrontendFilterReport {
    std::string filter_id;
    std::string shape;
    double request_generation_ms = 0.0;
    PlanBatchStats cold{};
    PlanBatchStats warm{};
    ExecutionStats execution{};
    ExecutionStats e2e_execution{};
    double cold_plan_plus_exec_oneshot_ms = 0.0;
    double request_plan_exec_oneshot_ms = 0.0;
    double planner_only_ms = 0.0;
    double execution_only_ms = 0.0;
    ProcessMemorySnapshot memory_baseline{};
    ProcessMemorySnapshot memory_after_cold{};
    ProcessMemorySnapshot memory_after_warm{};
    ProcessMemorySnapshot memory_after_exec{};
    ProcessMemorySnapshot memory_after_e2e{};
    ProcessMemorySnapshot memory_teardown{};
};

struct MixedPressureReport {
    PlanBatchStats cold{};
    PlanBatchStats warm{};
    ProcessMemorySnapshot memory_baseline{};
    ProcessMemorySnapshot memory_after_cold{};
    ProcessMemorySnapshot memory_after_warm{};
    ProcessMemorySnapshot memory_teardown{};
    std::size_t total_requests = 0;
    std::size_t cache_maximum_entries = 0;
    std::size_t cache_maximum_resident_bytes = 0;
    bool admission_limited = false;
    bool is_cache_pressure_benchmark = false;
    std::string_view note = "cache pressure not evaluated";
};

struct FramePhaseStats {
    double wall_ms = 0.0;
    std::size_t frames = 0;
    double checksum = 0.0;
};

struct VerificationFilterReport {
    std::string filter_id;
    std::string cohort;
    double plan_prepare_ms = 0.0;
    bool memory_checkpoints_enabled = false;
    FramePhaseStats cold_first{};
    FramePhaseStats warmup_2_10{};
    FramePhaseStats steady_11_100{};
    FramePhaseStats steady_101_end{};
    double analyze_total_ms = 0.0;
    double ordered_checksum = 0.0;
    std::uint64_t ordered_fingerprint = 0;
    bool finite = true;
    std::size_t processed_frames = 0;
    std::size_t effective_frame_workers = 0;
    std::size_t nonzero_error_count = 0;
    double scalar_reference_error = 0.0;
    double scalar_reference_abs_difference = 0.0;
    bool scalar_reference_matches = false;
    std::size_t peak_workspace_elements_per_worker = 0;
    std::size_t aggregate_workspace_elements = 0;
    std::size_t ring_frames = 0;
    std::size_t logical_frames = 0;
    std::size_t prime_step = 0;
    std::uint64_t source_physical_bytes = 0;
    std::uint64_t source_physical_total_bytes_reported = 0;
    ProcessMemorySnapshot memory_before_plan{};
    ProcessMemorySnapshot memory_after_plan{};
    ProcessMemorySnapshot memory_at_frame_1{};
    ProcessMemorySnapshot memory_at_frame_10{};
    ProcessMemorySnapshot memory_at_frame_100{};
    ProcessMemorySnapshot memory_at_frame_1000{};
    ProcessMemorySnapshot memory_at_final_frame{};
    std::size_t memory_at_final_frame_index = 0;
    ProcessMemorySnapshot memory_after_filter_teardown{};
    double timing_warmup_ms = 0.0;
    double timing_warmup_checksum = 0.0;
    std::uint64_t timing_warmup_fingerprint = 0;
    bool timing_warmup_performed = false;
    bool timing_warmup_valid = true;
    bool timing_warmup_matches_retained = true;
    bool timing_samples_valid = true;
    bool timing_fingerprints_consistent = true;
    bool timing_checksums_consistent = true;
    bool formal_timing_eligible = false;
    std::vector<double> analyze_total_samples_ms;
    std::vector<double> timing_sample_checksums;
    std::vector<std::uint64_t> timing_sample_fingerprints;
};

struct VerificationMemoryLifecycle {
    ProcessMemorySnapshot before_ring{};
    ProcessMemorySnapshot after_ring_prepare{};
    ProcessMemorySnapshot after_ring_teardown{};
};

[[nodiscard]] bool want_arm(const Configuration &config, Arm arm) {
    return config.arm == Arm::all || config.arm == arm;
}

[[nodiscard]] FrontendFilterReport
run_height_search_filter(const Configuration &config, const NamedFilter &named, bool envelope,
                         const getnative::ConstImageView *structured_source_or_null,
                         MeasurementGate &measurement_gate) {
    FrontendFilterReport report{};
    report.filter_id = std::string{named.id};
    report.shape = envelope ? "envelope" : "typical";
    report.memory_baseline = capture_process_memory();

    const bool run_cold =
        config.arm == Arm::cold || config.arm == Arm::warm || config.arm == Arm::all;
    const bool run_warm = config.arm == Arm::warm || config.arm == Arm::all;
    const bool run_exec =
        !envelope && (config.arm == Arm::exec ||
                      (config.arm == Arm::all && config.include_execution_on_frontend));
    const bool run_e2e =
        !envelope && (config.arm == Arm::e2e ||
                      (config.arm == Arm::all && config.include_execution_on_frontend));

    {
        std::vector<getnative::AxisPlanRequest> requests;
        if (run_cold || run_warm || run_exec) {
            phase_marker((envelope ? "envelope_gen_" : "typical_gen_") + report.filter_id);
            const auto gen_begin = Clock::now();
            requests = envelope ? make_envelope_requests(config, named.filter)
                                : make_typical_requests(config, named.filter);
            report.request_generation_ms = elapsed_ms(gen_begin, Clock::now());
        }

        if (run_cold || run_warm || run_exec) {
            getnative::AxisPlanCache cache;
            PlanBatchLive cold_live{};
            const bool retain_for_exec = run_exec;

            if (config.arm == Arm::cold) {
                measurement_gate.wait(envelope ? "envelope_cold" : "typical_cold");
            }
            phase_marker((envelope ? "envelope_cold_" : "typical_cold_") + report.filter_id +
                         "_begin");
            cold_live =
                measure_plan_batch(cache, requests, config.candidate_workers, retain_for_exec);
            phase_marker((envelope ? "envelope_cold_" : "typical_cold_") + report.filter_id +
                         "_end");
            report.cold = cold_live.stats;
            if (run_cold)
                report.planner_only_ms = report.cold.wall_ms;
            report.memory_after_cold = capture_process_memory();

            if (run_warm) {
                if (config.arm == Arm::warm) {
                    measurement_gate.wait(envelope ? "envelope_warm" : "typical_warm");
                }
                phase_marker((envelope ? "envelope_warm_" : "typical_warm_") + report.filter_id +
                             "_begin");
                auto warm_live =
                    measure_plan_batch(cache, requests, config.candidate_workers, false);
                phase_marker((envelope ? "envelope_warm_" : "typical_warm_") + report.filter_id +
                             "_end");
                report.warm = warm_live.stats;
                report.memory_after_warm = capture_process_memory();
            }

            if (run_exec) {
                if (structured_source_or_null == nullptr || cold_live.plans.empty()) {
                    throw std::logic_error("typical exec arm is missing source/plans");
                }
                if (config.arm == Arm::exec) {
                    measurement_gate.wait("typical_exec");
                }
                phase_marker("typical_exec_" + report.filter_id + "_begin");
                report.execution =
                    run_candidate_execution(config, *structured_source_or_null, cold_live.plans);
                phase_marker("typical_exec_" + report.filter_id + "_end");
                report.execution_only_ms = report.execution.wall_ms;
                validate_candidate_execution_sample(*structured_source_or_null, cold_live.plans,
                                                    report.execution);
                report.memory_after_exec = capture_process_memory();
            }
            cold_live.plans.clear();
        }

        if (run_e2e) {
            if (structured_source_or_null == nullptr) {
                throw std::logic_error("typical e2e arm is missing source");
            }
            if (config.arm == Arm::e2e) {
                measurement_gate.wait("typical_e2e");
            }
            phase_marker("typical_e2e_" + report.filter_id + "_begin");
            const auto total_begin = Clock::now();
            const auto generation_begin = Clock::now();
            auto e2e_requests = make_typical_requests(config, named.filter);
            const double generation_ms = elapsed_ms(generation_begin, Clock::now());
            const auto plan_exec_begin = Clock::now();
            ExecutionStats e2e_execution{};
            PlanBatchStats e2e_plan_stats{};
            {
                getnative::AxisPlanCache e2e_cache;
                auto e2e_live =
                    measure_plan_batch(e2e_cache, e2e_requests, config.candidate_workers, true);
                e2e_plan_stats = e2e_live.stats;
                e2e_execution =
                    run_candidate_execution(config, *structured_source_or_null, e2e_live.plans);
                report.cold_plan_plus_exec_oneshot_ms = elapsed_ms(plan_exec_begin, Clock::now());
                report.request_plan_exec_oneshot_ms = elapsed_ms(total_begin, Clock::now());
                phase_marker("typical_e2e_" + report.filter_id + "_end");
                validate_candidate_execution_sample(*structured_source_or_null, e2e_live.plans,
                                                    e2e_execution);
                report.memory_after_e2e = capture_process_memory();
            }
            report.e2e_execution = e2e_execution;
            if (config.arm == Arm::e2e) {
                report.request_generation_ms = generation_ms;
                report.cold = e2e_plan_stats;
                report.planner_only_ms = e2e_plan_stats.wall_ms;
                report.execution = e2e_execution;
                report.execution_only_ms = e2e_execution.wall_ms;
            }
        }
    }

    report.memory_teardown = capture_process_memory();
    return report;
}

[[nodiscard]] MixedPressureReport
run_mixed_cache_pressure(const Configuration &config, bool envelope,
                         const std::vector<const NamedFilter *> &filters,
                         MeasurementGate &measurement_gate) {
    MixedPressureReport report{};
    report.memory_baseline = capture_process_memory();
    {
        std::vector<std::vector<getnative::AxisPlanRequest>> per_filter;
        per_filter.reserve(filters.size());
        for (const NamedFilter *named : filters) {
            per_filter.push_back(envelope ? make_envelope_requests(config, named->filter)
                                          : make_typical_requests(config, named->filter));
        }
        std::vector<getnative::AxisPlanRequest> mixed;
        mixed.reserve(static_cast<std::size_t>(config.candidate_count) * filters.size());
        for (std::size_t index = 0; index < static_cast<std::size_t>(config.candidate_count);
             ++index) {
            for (std::size_t filter = 0; filter < per_filter.size(); ++filter) {
                if (index < per_filter[filter].size()) {
                    mixed.push_back(per_filter[filter][index]);
                }
            }
        }
        report.total_requests = mixed.size();

        {
            getnative::AxisPlanCache cache;
            const getnative::AxisPlanCacheLimits limits = cache.limits();
            report.cache_maximum_entries = limits.maximum_entries;
            report.cache_maximum_resident_bytes = limits.maximum_resident_bytes;
            measurement_gate.wait(envelope ? "envelope_mixed" : "typical_mixed");
            phase_marker(envelope ? "envelope_mixed_cold_begin" : "typical_mixed_cold_begin");
            auto cold = measure_plan_batch(cache, mixed, config.candidate_workers, false);
            report.cold = cold.stats;
            report.memory_after_cold = capture_process_memory();
            phase_marker(envelope ? "envelope_mixed_cold_end" : "typical_mixed_cold_end");
            phase_marker(envelope ? "envelope_mixed_warm_begin" : "typical_mixed_warm_begin");
            auto warm = measure_plan_batch(cache, mixed, config.candidate_workers, false);
            report.warm = warm.stats;
            report.memory_after_warm = capture_process_memory();
            phase_marker(envelope ? "envelope_mixed_warm_end" : "typical_mixed_warm_end");
        }
        report.admission_limited = report.cold.published_plan_count < report.cold.unique_key_count;
        report.is_cache_pressure_benchmark = report.admission_limited ||
                                             report.warm.physical_build_count != 0 ||
                                             report.warm.ready_hit_count != report.total_requests;
        report.note =
            report.is_cache_pressure_benchmark
                ? "Observed cache admission pressure; warm rebuilds are expected overflow work"
                : "Selected request set did not exercise AxisPlanCache admission limits";
    }
    report.memory_teardown = capture_process_memory();
    return report;
}

// One persistent pool for the complete logical stream. Checkpoint barriers wake
// the existing workers; they do not recreate threads or workspaces.
class PersistentFrameExecutor {
  public:
    std::size_t worker_count = 1;
    std::vector<getnative::CpuWorkspace> workspaces;
    std::vector<std::size_t> peak_elements_per_worker;

    explicit PersistentFrameExecutor(std::size_t workers)
        : worker_count(std::max<std::size_t>(1, workers)), workspaces(worker_count),
          peak_elements_per_worker(worker_count, 0) {
        if (worker_count > 1) {
            threads_.reserve(worker_count);
            try {
                for (std::size_t worker = 0; worker < worker_count; ++worker) {
                    threads_.emplace_back([this, worker] { worker_loop(worker); });
                }
            } catch (...) {
                {
                    const std::scoped_lock lock(job_mutex_);
                    stopping_ = true;
                }
                job_ready_.notify_all();
                throw;
            }
            std::unique_lock lock(job_mutex_);
            workers_ready_.wait(lock, [&] { return ready_workers_ == worker_count; });
        }
    }

    ~PersistentFrameExecutor() {
        if (threads_.empty())
            return;
        {
            const std::scoped_lock lock(job_mutex_);
            stopping_ = true;
        }
        job_ready_.notify_all();
        threads_.clear();
    }

    PersistentFrameExecutor(const PersistentFrameExecutor &) = delete;
    PersistentFrameExecutor &operator=(const PersistentFrameExecutor &) = delete;

    [[nodiscard]] std::size_t aggregate_peak_elements() const {
        std::size_t sum = 0;
        for (const std::size_t peak : peak_elements_per_worker)
            sum += peak;
        return sum;
    }

    [[nodiscard]] std::size_t max_peak_elements() const {
        return peak_elements_per_worker.empty()
                   ? 0
                   : *std::max_element(peak_elements_per_worker.begin(),
                                       peak_elements_per_worker.end());
    }

    [[nodiscard]] std::size_t processed_frames() const noexcept {
        return processed_frames_.load(std::memory_order_relaxed);
    }

    // Fills errors[begin..end) in logical frame index order (each slot written once).
    void run_frames(std::size_t begin, std::size_t end, const SourceRing &ring,
                    const getnative::AxisPlan &plan, const getnative::MetricSpec &metric,
                    getnative::detail::ColumnDispatchPolicy policy, std::vector<double> &errors) {
        if (begin >= end)
            return;
        if (worker_count <= 1) {
            for (std::size_t frame = begin; frame < end; ++frame) {
                errors[frame] = getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                    ring.view(frame), plan, getnative::AnalysisAxes::vertical, metric,
                    workspaces[0], policy);
                peak_elements_per_worker[0] =
                    std::max(peak_elements_per_worker[0], workspaces[0].peak_elements());
                processed_frames_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        std::unique_lock lock(job_mutex_);
        job_end_ = end;
        job_ring_ = &ring;
        job_plan_ = &plan;
        job_metric_ = &metric;
        job_policy_ = policy;
        job_errors_ = &errors;
        cursor_.store(begin, std::memory_order_relaxed);
        completed_workers_ = 0;
        failure_ = nullptr;
        ++job_generation_;
        lock.unlock();
        job_ready_.notify_all();

        lock.lock();
        job_finished_.wait(lock, [&] { return completed_workers_ == worker_count; });
        const std::exception_ptr failure = failure_;
        job_ring_ = nullptr;
        job_plan_ = nullptr;
        job_metric_ = nullptr;
        job_errors_ = nullptr;
        lock.unlock();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

  private:
    void worker_loop(std::size_t worker) {
        {
            const std::scoped_lock lock(job_mutex_);
            ++ready_workers_;
            if (ready_workers_ == worker_count)
                workers_ready_.notify_one();
        }
        std::uint64_t observed_generation = 0;
        while (true) {
            std::unique_lock lock(job_mutex_);
            job_ready_.wait(lock,
                            [&] { return stopping_ || job_generation_ != observed_generation; });
            if (stopping_)
                return;

            const std::uint64_t generation = job_generation_;
            const std::size_t end = job_end_;
            const SourceRing *ring = job_ring_;
            const getnative::AxisPlan *plan = job_plan_;
            const getnative::MetricSpec *metric = job_metric_;
            const auto policy = job_policy_;
            std::vector<double> *errors = job_errors_;
            lock.unlock();

            try {
                while (true) {
                    const std::size_t frame = cursor_.fetch_add(1, std::memory_order_relaxed);
                    if (frame >= end)
                        break;
                    (*errors)[frame] =
                        getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                            ring->view(frame), *plan, getnative::AnalysisAxes::vertical, *metric,
                            workspaces[worker], policy);
                    peak_elements_per_worker[worker] = std::max(peak_elements_per_worker[worker],
                                                                workspaces[worker].peak_elements());
                    processed_frames_.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                const std::scoped_lock failure_lock(job_mutex_);
                if (!failure_)
                    failure_ = std::current_exception();
                cursor_.store(end, std::memory_order_relaxed);
            }

            lock.lock();
            observed_generation = generation;
            ++completed_workers_;
            if (completed_workers_ == worker_count) {
                job_finished_.notify_one();
            }
        }
    }

    std::mutex job_mutex_;
    std::condition_variable job_ready_;
    std::condition_variable job_finished_;
    std::condition_variable workers_ready_;
    std::vector<getnative::JoiningThread> threads_;
    std::atomic_size_t cursor_{0};
    std::atomic_size_t processed_frames_{0};
    std::size_t job_end_ = 0;
    std::size_t completed_workers_ = 0;
    std::size_t ready_workers_ = 0;
    std::uint64_t job_generation_ = 0;
    const SourceRing *job_ring_ = nullptr;
    const getnative::AxisPlan *job_plan_ = nullptr;
    const getnative::MetricSpec *job_metric_ = nullptr;
    getnative::detail::ColumnDispatchPolicy job_policy_ =
        getnative::detail::ColumnDispatchPolicy::automatic;
    std::vector<double> *job_errors_ = nullptr;
    std::exception_ptr failure_;
    bool stopping_ = false;
};

[[nodiscard]] FramePhaseStats summarize_range(const std::vector<double> &errors, std::size_t begin,
                                              std::size_t end, double wall_ms) {
    FramePhaseStats phase{};
    if (begin >= end || begin >= errors.size())
        return phase;
    end = std::min(end, errors.size());
    phase.frames = end - begin;
    phase.wall_ms = wall_ms;
    for (std::size_t frame = begin; frame < end; ++frame) {
        phase.checksum += errors[frame];
    }
    return phase;
}

[[nodiscard]] VerificationFilterReport
run_verification_filter(const Configuration &config, const NamedFilter &named,
                        const SourceRing &ring, std::string_view cohort_name,
                        std::uint64_t reported_source_total_bytes,
                        MeasurementGate &measurement_gate, bool capture_memory_checkpoints) {
    VerificationFilterReport report{};
    report.filter_id = std::string{named.id};
    report.cohort = std::string{cohort_name};
    report.ring_frames = ring.physical_count();
    report.logical_frames = config.logical_frames;
    report.prime_step = ring.prime_step;
    report.source_physical_bytes = ring.physical_bytes();
    report.source_physical_total_bytes_reported = reported_source_total_bytes;
    report.memory_checkpoints_enabled = capture_memory_checkpoints;
    report.memory_before_plan = capture_process_memory();

    {
        phase_marker("verify_plan_" + report.filter_id + "_" + report.cohort);
        const getnative::AxisPlanRequest request{
            config.source_height,
            config.locked_native_height,
            static_cast<double>(config.locked_native_height),
            0.0,
            named.filter,
            getnative::BorderMode::mirror,
        };
        getnative::AxisPlanCache session_cache;
        const auto plan_begin = Clock::now();
        const auto plan = session_cache.get_or_build(request);
        report.plan_prepare_ms = elapsed_ms(plan_begin, Clock::now());
        report.memory_after_plan = capture_process_memory();

        const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
        const auto policy = getnative::detail::column_dispatch_policy(config.cpu_isa);
        (void)getnative::require_cpu_isa(config.cpu_isa);

        const std::size_t workers =
            std::min(config.frame_workers, std::max<std::size_t>(1, config.logical_frames));
        report.effective_frame_workers = workers;
        PersistentFrameExecutor executor(workers);
        std::vector<double> errors(config.logical_frames, 0.0);

        measurement_gate.wait("verification_exec");
        phase_marker("verify_frames_" + report.filter_id + "_" + report.cohort + "_begin");

        auto run_segment = [&](std::size_t begin, std::size_t end) -> double {
            end = std::min(end, config.logical_frames);
            if (begin >= end)
                return 0.0;
            const auto wall_begin = Clock::now();
            executor.run_frames(begin, end, ring, *plan, metric, policy, errors);
            return elapsed_ms(wall_begin, Clock::now());
        };

        double w0 = 0.0;
        double w2 = 0.0;
        double w3 = 0.0;
        double w4 = 0.0;
        if (capture_memory_checkpoints) {
            w0 = run_segment(0, 1);
            report.memory_at_frame_1 = capture_process_memory();

            w2 = run_segment(1, 10);
            if (config.logical_frames >= 10) {
                report.memory_at_frame_10 = capture_process_memory();
            }

            w3 = run_segment(10, 100);
            if (config.logical_frames >= 100) {
                report.memory_at_frame_100 = capture_process_memory();
            }

            w4 = run_segment(100, 1000);
            if (config.logical_frames >= 1000) {
                report.memory_at_frame_1000 = capture_process_memory();
            }
            w4 += run_segment(1000, config.logical_frames);
        } else {
            const auto wall_begin = Clock::now();
            executor.run_frames(0, config.logical_frames, ring, *plan, metric, policy, errors);
            report.analyze_total_ms = elapsed_ms(wall_begin, Clock::now());
        }
        phase_marker("verify_frames_" + report.filter_id + "_" + report.cohort + "_end");

        report.memory_at_final_frame = capture_process_memory();
        report.memory_at_final_frame_index = config.logical_frames;
        report.cold_first = summarize_range(errors, 0, 1, w0);
        report.warmup_2_10 = summarize_range(errors, 1, 10, w2);
        report.steady_11_100 = summarize_range(errors, 10, 100, w3);
        report.steady_101_end = summarize_range(errors, 100, config.logical_frames, w4);
        if (capture_memory_checkpoints) {
            report.analyze_total_ms = w0 + w2 + w3 + w4;
        }

        report.ordered_checksum = 0.0;
        report.ordered_fingerprint = 0;
        report.finite = true;
        for (std::size_t frame = 0; frame < errors.size(); ++frame) {
            const double error = errors[frame];
            if (!std::isfinite(error))
                report.finite = false;
            if (error != 0.0)
                ++report.nonzero_error_count;
            report.ordered_checksum += error;
            const auto bits = std::bit_cast<std::uint64_t>(error);
            report.ordered_fingerprint ^= bits + 0x9E3779B97F4A7C15ULL +
                                          (report.ordered_fingerprint << 6U) +
                                          (report.ordered_fingerprint >> 2U);
            report.ordered_fingerprint += static_cast<std::uint64_t>(frame);
        }
        report.processed_frames = executor.processed_frames();
        report.peak_workspace_elements_per_worker = executor.max_peak_elements();
        report.aggregate_workspace_elements = executor.aggregate_peak_elements();

        phase_marker("verify_reference_" + report.filter_id + "_" + report.cohort + "_begin");
        getnative::CpuWorkspace scalar_workspace;
        const auto scalar_policy =
            getnative::detail::column_dispatch_policy(getnative::CpuIsaRequest::scalar);
        report.scalar_reference_error =
            getnative::detail::analyze_axis_candidate_with_column_policy_f32(
                ring.view(0), *plan, getnative::AnalysisAxes::vertical, metric, scalar_workspace,
                scalar_policy);
        report.scalar_reference_abs_difference =
            std::abs(errors.front() - report.scalar_reference_error);
        report.scalar_reference_matches =
            approximately_equal(errors.front(), report.scalar_reference_error);
        phase_marker("verify_reference_" + report.filter_id + "_" + report.cohort + "_end");
    }

    report.memory_after_filter_teardown = capture_process_memory();
    return report;
}

[[nodiscard]] VerificationFilterReport
run_verification_case(const Configuration &config, const NamedFilter &named, const SourceRing &ring,
                      std::string_view cohort_name, std::uint64_t reported_source_total_bytes,
                      MeasurementGate &measurement_gate) {
    if (config.timing_samples == 1) {
        auto report = run_verification_filter(config, named, ring, cohort_name,
                                              reported_source_total_bytes, measurement_gate, true);
        report.analyze_total_samples_ms.push_back(report.analyze_total_ms);
        report.timing_sample_checksums.push_back(report.ordered_checksum);
        report.timing_sample_fingerprints.push_back(report.ordered_fingerprint);
        return report;
    }

    MeasurementGate disabled_gate(false);
    phase_marker("timing_warmup_begin");
    const auto warmup = run_verification_filter(config, named, ring, cohort_name,
                                                reported_source_total_bytes, disabled_gate, false);
    phase_marker("timing_warmup_end");
    const bool warmup_valid = warmup.finite && warmup.processed_frames == config.logical_frames &&
                              warmup.scalar_reference_matches &&
                              (warmup.cohort != "decorrelated" || warmup.nonzero_error_count > 0);

    VerificationFilterReport report{};
    for (std::size_t sample = 0; sample < config.timing_samples; ++sample) {
        phase_marker("timing_sample_" + std::to_string(sample) + "_begin");
        auto current = run_verification_filter(config, named, ring, cohort_name,
                                               reported_source_total_bytes, disabled_gate, false);
        phase_marker("timing_sample_" + std::to_string(sample) + "_end");
        const double sample_ms = current.analyze_total_ms;
        const double sample_checksum = current.ordered_checksum;
        const std::uint64_t sample_fingerprint = current.ordered_fingerprint;
        const bool sample_valid =
            current.finite && current.processed_frames == config.logical_frames &&
            current.scalar_reference_matches &&
            (current.cohort != "decorrelated" || current.nonzero_error_count > 0);
        if (sample == 0) {
            report = std::move(current);
            report.analyze_total_samples_ms.clear();
            report.timing_sample_checksums.clear();
            report.timing_sample_fingerprints.clear();
            report.timing_warmup_ms = warmup.analyze_total_ms;
            report.timing_warmup_checksum = warmup.ordered_checksum;
            report.timing_warmup_fingerprint = warmup.ordered_fingerprint;
            report.timing_warmup_performed = true;
            report.timing_warmup_valid = warmup_valid;
            report.timing_warmup_matches_retained =
                warmup.ordered_fingerprint == sample_fingerprint &&
                std::bit_cast<std::uint64_t>(warmup.ordered_checksum) ==
                    std::bit_cast<std::uint64_t>(sample_checksum);
            report.timing_samples_valid = sample_valid;
        } else {
            report.timing_fingerprints_consistent =
                report.timing_fingerprints_consistent &&
                current.ordered_fingerprint == report.ordered_fingerprint;
            report.timing_checksums_consistent =
                report.timing_checksums_consistent &&
                std::bit_cast<std::uint64_t>(current.ordered_checksum) ==
                    std::bit_cast<std::uint64_t>(report.ordered_checksum);
            report.finite = report.finite && current.finite;
            report.timing_samples_valid = report.timing_samples_valid && sample_valid;
            report.scalar_reference_matches =
                report.scalar_reference_matches && current.scalar_reference_matches;
        }
        report.analyze_total_samples_ms.push_back(sample_ms);
        report.timing_sample_checksums.push_back(sample_checksum);
        report.timing_sample_fingerprints.push_back(sample_fingerprint);
    }
    report.memory_after_filter_teardown = capture_process_memory();
    const bool raw_sample_counts_match =
        report.analyze_total_samples_ms.size() == config.timing_samples &&
        report.timing_sample_checksums.size() == config.timing_samples &&
        report.timing_sample_fingerprints.size() == config.timing_samples;
    report.formal_timing_eligible =
        config.timing_samples >= 21 && !config.profile_mode && !config.attach_gate &&
        report.timing_warmup_performed && report.timing_warmup_valid &&
        report.timing_warmup_matches_retained && report.timing_samples_valid &&
        report.timing_fingerprints_consistent && report.timing_checksums_consistent &&
        raw_sample_counts_match;
    return report;
}

void append_plan_stats(std::ostream &output, const PlanBatchStats &stats) {
    output << "{\"wall_ms\":" << stats.wall_ms << ",\"unique_key_count\":" << stats.unique_key_count
           << ",\"physical_build_count\":" << stats.physical_build_count
           << ",\"published_plan_count\":" << stats.published_plan_count
           << ",\"ready_hit_count\":" << stats.ready_hit_count
           << ",\"peak_active_builds\":" << stats.peak_active_builds
           << ",\"effective_worker_count\":" << stats.effective_worker_count
           << ",\"resident_entries\":" << stats.resident_entries
           << ",\"resident_bytes\":" << stats.resident_bytes
           << ",\"mean_plan_bytes\":" << stats.mean_plan_bytes
           << ",\"p95_plan_bytes\":" << stats.p95_plan_bytes
           << ",\"returned_plan_count\":" << stats.returned_plan_count
           << ",\"retains_plan_handles\":false}";
}

void append_execution_stats(std::ostream &output, const ExecutionStats &stats) {
    output << "{\"wall_ms\":" << stats.wall_ms << ",\"checksum\":" << stats.checksum
           << ",\"ordered_fingerprint\":" << stats.ordered_fingerprint
           << ",\"nonzero_error_count\":" << stats.nonzero_error_count
           << ",\"first_error\":" << stats.first_error << ",\"last_error\":" << stats.last_error
           << ",\"sampled_scalar_reference_matches\":"
           << (stats.sampled_scalar_reference_matches ? "true" : "false")
           << ",\"finite\":" << (stats.finite ? "true" : "false")
           << ",\"result_count\":" << stats.result_count << '}';
}

void append_memory(std::ostream &output, const ProcessMemorySnapshot &snap) {
    output << "{\"captured\":" << (snap.captured ? "true" : "false")
           << ",\"working_set_bytes\":" << snap.working_set_bytes
           << ",\"peak_working_set_bytes\":" << snap.peak_working_set_bytes
           << ",\"private_bytes\":" << snap.private_bytes
           << ",\"page_fault_count\":" << snap.page_fault_count << '}';
}

void append_frame_phase(std::ostream &output, const FramePhaseStats &phase) {
    output << "{\"frames\":" << phase.frames << ",\"wall_ms\":" << phase.wall_ms
           << ",\"checksum\":" << phase.checksum << '}';
}

void append_summary(std::ostream &output, const std::vector<double> &raw) {
    if (raw.empty()) {
        output << "null";
        return;
    }
    const auto summary = getnative::benchmark::summarize(raw);
    output << "{\"sample_count\":" << summary.raw.size() << ",\"median\":" << summary.median
           << ",\"mad\":" << summary.mad << ",\"minimum\":" << summary.minimum
           << ",\"maximum\":" << summary.maximum << ",\"raw\":[";
    for (std::size_t index = 0; index < summary.raw.size(); ++index) {
        if (index != 0)
            output << ',';
        output << summary.raw[index];
    }
    output << "]}";
}

struct BenchmarkArtifact {
    std::string json;
    bool assertions_pass = false;
};

[[nodiscard]] BenchmarkArtifact
make_json(const Configuration &config, const getnative::CpuDispatchInfo &dispatch,
          const std::vector<FrontendFilterReport> &typical,
          const std::vector<FrontendFilterReport> &envelope,
          const std::optional<MixedPressureReport> &typical_mixed,
          const std::optional<MixedPressureReport> &envelope_mixed,
          const std::vector<VerificationFilterReport> &verification,
          double structured_ring_prepare_ms, double decorrelated_ring_prepare_ms,
          std::uint64_t source_physical_total_bytes,
          const VerificationMemoryLifecycle &verification_memory, int argc, char **argv) {
    std::ostringstream output;
    output << std::setprecision(17) << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_cpu_memory_profile_benchmark", "memory-profile-v4",
        "scoped-lifecycle-persistent-pool-raw-timing-v4", argc, argv);

    output << ",\"profile_contract\":{"
              "\"version\":4,"
              "\"plan_handles\":\"stats never retain shared_ptr; exec releases immediately\","
              "\"profile_mode\":\"explicit arguments win; direct launch attributes the whole "
              "process\","
              "\"attach_gate\":\"single filter/suite/arm; wait occurs immediately before the "
              "measured region\","
              "\"verification_source\":\"one ring per process by default in profile-mode "
              "(decorrelated)\","
              "\"source_physical_total_bytes_field\":\"pixel payload bytes for source rings "
              "actually created\","
              "\"frame_executor\":\"persistent worker threads and workspaces across checkpoint "
              "barriers\","
              "\"memory_lifecycle\":\"before ring, after ring prepare, per-filter "
              "active/checkpoints/teardown, after ring teardown\","
              "\"checksum\":\"logical-frame-index order plus sampled scalar reference\","
              "\"timing_samples\":\"warmup plus raw retained samples; use >=21 without profiler\""
              "}";

    const double ring_one_mib = static_cast<double>(config.ring_frames) *
                                static_cast<double>(config.source_width) *
                                static_cast<double>(config.source_height) *
                                static_cast<double>(sizeof(float)) / (1024.0 * 1024.0);

    output << ",\"configuration\":{"
           << "\"suite\":" << getnative::benchmark::json_string(suite_name(config.suite))
           << ",\"arm\":" << getnative::benchmark::json_string(arm_name(config.arm))
           << ",\"cohort\":" << getnative::benchmark::json_string(cohort_name(config.cohort))
           << ",\"filter\":"
           << (config.filter_id ? getnative::benchmark::json_string(*config.filter_id)
                                : std::string{"null"})
           << ",\"source_width\":" << config.source_width
           << ",\"source_height\":" << config.source_height
           << ",\"locked_native_height\":" << config.locked_native_height
           << ",\"candidate_count\":" << config.candidate_count
           << ",\"logical_frames\":" << config.logical_frames
           << ",\"ring_frames\":" << config.ring_frames
           << ",\"ring_one_approx_mib\":" << ring_one_mib
           << ",\"detected_llc_bytes\":" << config.detected_llc_bytes
           << ",\"ring_to_llc_ratio\":" << config.ring_to_llc_ratio
           << ",\"ring_pressure_target_multiple\":2"
           << ",\"ring_pressure_target_met\":"
           << (config.ring_pressure_target_met ? "true" : "false")
           << ",\"cache_pressure_required\":" << (config.require_cache_pressure ? "true" : "false")
           << ",\"source_physical_total_bytes\":" << source_physical_total_bytes
           << ",\"source_physical_total_mib\":"
           << (static_cast<double>(source_physical_total_bytes) / (1024.0 * 1024.0))
           << ",\"candidate_workers\":" << config.candidate_workers
           << ",\"frame_workers\":" << config.frame_workers
           << ",\"timing_samples\":" << config.timing_samples
           << ",\"include_execution_on_frontend\":"
           << (config.include_execution_on_frontend ? "true" : "false")
           << ",\"profile_mode\":" << (config.profile_mode ? "true" : "false")
           << ",\"attach_gate\":" << (config.attach_gate ? "true" : "false")
           << ",\"seed\":" << config.seed << '}';

    output << ",\"dispatch\":{"
           << "\"requested\":"
           << getnative::benchmark::json_string(getnative::cpu_isa_request_name(dispatch.request))
           << ",\"selected\":"
           << getnative::benchmark::json_string(getnative::cpu_isa_name(dispatch.selected))
           << ",\"math_mode\":" << getnative::benchmark::json_string(dispatch.math_mode)
           << ",\"selection_reason\":"
           << getnative::benchmark::json_string(dispatch.selection_reason) << '}';

    auto append_frontend = [&](const char *key, const std::vector<FrontendFilterReport> &items) {
        output << ",\"" << key << "\":[";
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (index != 0)
                output << ',';
            const auto &item = items[index];
            output << "{\"filter_id\":" << getnative::benchmark::json_string(item.filter_id)
                   << ",\"shape\":" << getnative::benchmark::json_string(item.shape)
                   << ",\"request_generation_ms\":" << item.request_generation_ms
                   << ",\"planner_only_ms\":" << item.planner_only_ms
                   << ",\"execution_only_ms\":" << item.execution_only_ms
                   << ",\"cold_plan_plus_exec_oneshot_ms\":" << item.cold_plan_plus_exec_oneshot_ms
                   << ",\"request_plan_exec_oneshot_ms\":" << item.request_plan_exec_oneshot_ms
                   << ",\"cold\":";
            append_plan_stats(output, item.cold);
            output << ",\"warm\":";
            append_plan_stats(output, item.warm);
            output << ",\"execution\":";
            append_execution_stats(output, item.execution);
            output << ",\"e2e_execution\":";
            append_execution_stats(output, item.e2e_execution);
            output << ",\"memory_baseline\":";
            append_memory(output, item.memory_baseline);
            output << ",\"memory_after_cold\":";
            append_memory(output, item.memory_after_cold);
            output << ",\"memory_after_warm\":";
            append_memory(output, item.memory_after_warm);
            output << ",\"memory_after_exec\":";
            append_memory(output, item.memory_after_exec);
            output << ",\"memory_after_e2e\":";
            append_memory(output, item.memory_after_e2e);
            output << ",\"memory_teardown\":";
            append_memory(output, item.memory_teardown);
            output << '}';
        }
        output << ']';
    };
    append_frontend("height_search_typical", typical);
    append_frontend("height_envelope", envelope);

    auto append_mixed = [&](const char *key, const std::optional<MixedPressureReport> &mixed) {
        output << ",\"" << key << "\":";
        if (!mixed) {
            output << "null";
            return;
        }
        output << "{\"is_cache_pressure_benchmark\":"
               << (mixed->is_cache_pressure_benchmark ? "true" : "false")
               << ",\"admission_limited\":" << (mixed->admission_limited ? "true" : "false")
               << ",\"total_requests\":" << mixed->total_requests
               << ",\"cache_maximum_entries\":" << mixed->cache_maximum_entries
               << ",\"cache_maximum_resident_bytes\":" << mixed->cache_maximum_resident_bytes
               << ",\"note\":" << getnative::benchmark::json_string(mixed->note)
               << ",\"memory_baseline\":";
        append_memory(output, mixed->memory_baseline);
        output << ",\"cold\":";
        append_plan_stats(output, mixed->cold);
        output << ",\"warm\":";
        append_plan_stats(output, mixed->warm);
        output << ",\"memory_after_cold\":";
        append_memory(output, mixed->memory_after_cold);
        output << ",\"memory_after_warm\":";
        append_memory(output, mixed->memory_after_warm);
        output << ",\"memory_teardown\":";
        append_memory(output, mixed->memory_teardown);
        output << '}';
    };
    append_mixed("typical_mixed_cache_pressure", typical_mixed);
    append_mixed("envelope_mixed_cache_pressure", envelope_mixed);

    output << ",\"verification\":{"
           << "\"structured_ring_prepare_ms\":" << structured_ring_prepare_ms
           << ",\"decorrelated_ring_prepare_ms\":" << decorrelated_ring_prepare_ms
           << ",\"source_physical_total_bytes\":" << source_physical_total_bytes
           << ",\"source_physical_total_mib\":"
           << (static_cast<double>(source_physical_total_bytes) / (1024.0 * 1024.0))
           << ",\"memory_lifecycle\":{\"before_ring\":";
    append_memory(output, verification_memory.before_ring);
    output << ",\"after_ring_prepare\":";
    append_memory(output, verification_memory.after_ring_prepare);
    output << ",\"after_ring_teardown\":";
    append_memory(output, verification_memory.after_ring_teardown);
    output << "},\"filters\":[";
    for (std::size_t index = 0; index < verification.size(); ++index) {
        if (index != 0)
            output << ',';
        const auto &item = verification[index];
        output << "{\"filter_id\":" << getnative::benchmark::json_string(item.filter_id)
               << ",\"cohort\":" << getnative::benchmark::json_string(item.cohort)
               << ",\"plan_prepare_ms\":" << item.plan_prepare_ms
               << ",\"ring_frames\":" << item.ring_frames
               << ",\"logical_frames\":" << item.logical_frames
               << ",\"prime_step\":" << item.prime_step
               << ",\"source_physical_bytes\":" << item.source_physical_bytes
               << ",\"source_physical_total_bytes_reported\":"
               << item.source_physical_total_bytes_reported
               << ",\"requested_frame_workers\":" << config.frame_workers
               << ",\"effective_frame_workers\":" << item.effective_frame_workers
               << ",\"memory_checkpoints_enabled\":"
               << (item.memory_checkpoints_enabled ? "true" : "false")
               << ",\"peak_workspace_elements_per_worker\":"
               << item.peak_workspace_elements_per_worker
               << ",\"aggregate_workspace_elements\":" << item.aggregate_workspace_elements
               << ",\"processed_frames\":" << item.processed_frames
               << ",\"nonzero_error_count\":" << item.nonzero_error_count << ",\"cold_first\":";
        append_frame_phase(output, item.cold_first);
        output << ",\"warmup_2_10\":";
        append_frame_phase(output, item.warmup_2_10);
        output << ",\"steady_11_100\":";
        append_frame_phase(output, item.steady_11_100);
        output << ",\"steady_101_end\":";
        append_frame_phase(output, item.steady_101_end);
        output << ",\"analyze_total_ms\":" << item.analyze_total_ms
               << ",\"ordered_checksum\":" << item.ordered_checksum
               << ",\"ordered_fingerprint\":" << item.ordered_fingerprint
               << ",\"finite\":" << (item.finite ? "true" : "false")
               << ",\"scalar_reference_error\":" << item.scalar_reference_error
               << ",\"scalar_reference_abs_difference\":" << item.scalar_reference_abs_difference
               << ",\"scalar_reference_matches\":"
               << (item.scalar_reference_matches ? "true" : "false")
               << ",\"timing_warmup_ms\":" << item.timing_warmup_ms
               << ",\"timing_warmup_checksum\":" << item.timing_warmup_checksum
               << ",\"timing_warmup_fingerprint\":" << item.timing_warmup_fingerprint
               << ",\"timing_warmup_performed\":"
               << (item.timing_warmup_performed ? "true" : "false")
               << ",\"timing_warmup_valid\":" << (item.timing_warmup_valid ? "true" : "false")
               << ",\"timing_warmup_matches_retained\":"
               << (item.timing_warmup_matches_retained ? "true" : "false")
               << ",\"timing_samples_valid\":" << (item.timing_samples_valid ? "true" : "false")
               << ",\"formal_timing_eligible\":" << (item.formal_timing_eligible ? "true" : "false")
               << ",\"timing_fingerprints_consistent\":"
               << (item.timing_fingerprints_consistent ? "true" : "false")
               << ",\"timing_checksums_consistent\":"
               << (item.timing_checksums_consistent ? "true" : "false")
               << ",\"analyze_total_summary\":";
        append_summary(output, item.analyze_total_samples_ms);
        output << ",\"timing_sample_checksums\":[";
        for (std::size_t sample = 0; sample < item.timing_sample_checksums.size(); ++sample) {
            if (sample != 0)
                output << ',';
            output << item.timing_sample_checksums[sample];
        }
        output << "],\"timing_sample_fingerprints\":[";
        for (std::size_t sample = 0; sample < item.timing_sample_fingerprints.size(); ++sample) {
            if (sample != 0)
                output << ',';
            output << item.timing_sample_fingerprints[sample];
        }
        output << "],\"memory_before_plan\":";
        append_memory(output, item.memory_before_plan);
        output << ",\"memory_after_plan\":";
        append_memory(output, item.memory_after_plan);
        output << ",\"memory_at_frame_1\":";
        append_memory(output, item.memory_at_frame_1);
        output << ",\"memory_at_frame_10\":";
        append_memory(output, item.memory_at_frame_10);
        output << ",\"memory_at_frame_100\":";
        append_memory(output, item.memory_at_frame_100);
        output << ",\"memory_at_frame_1000\":";
        append_memory(output, item.memory_at_frame_1000);
        output << ",\"memory_at_final_frame\":";
        append_memory(output, item.memory_at_final_frame);
        output << ",\"memory_at_final_frame_index\":" << item.memory_at_final_frame_index;
        output << ",\"memory_after_filter_teardown\":";
        append_memory(output, item.memory_after_filter_teardown);
        output << '}';
    }
    output << "]}";

    bool ok = true;
    const std::size_t expected_candidates = static_cast<std::size_t>(config.candidate_count);
    auto validate_frontend_plans = [&](const auto &items) {
        for (const auto &item : items) {
            ok = ok && item.cold.unique_key_count == expected_candidates &&
                 item.cold.physical_build_count == expected_candidates &&
                 item.cold.returned_plan_count == expected_candidates;
            if (config.arm == Arm::warm || config.arm == Arm::all) {
                ok = ok && item.warm.ready_hit_count == expected_candidates &&
                     item.warm.physical_build_count == 0 &&
                     item.warm.returned_plan_count == expected_candidates;
            }
        }
    };
    validate_frontend_plans(typical);
    validate_frontend_plans(envelope);

    const auto execution_passes = [&](const ExecutionStats &execution) {
        return execution.finite && execution.result_count == expected_candidates &&
               execution.sampled_scalar_reference_matches;
    };
    for (const auto &item : typical) {
        const bool expects_exec = config.arm == Arm::exec ||
                                  (config.arm == Arm::all && config.include_execution_on_frontend);
        const bool expects_e2e = config.arm == Arm::e2e ||
                                 (config.arm == Arm::all && config.include_execution_on_frontend);
        if (expects_exec)
            ok = ok && execution_passes(item.execution);
        if (expects_e2e)
            ok = ok && execution_passes(item.e2e_execution);
    }

    auto validate_mixed = [&](const std::optional<MixedPressureReport> &mixed) {
        if (!mixed)
            return;
        const bool observed_pressure = mixed->admission_limited ||
                                       mixed->warm.physical_build_count != 0 ||
                                       mixed->warm.ready_hit_count != mixed->total_requests;
        ok = ok && mixed->total_requests != 0 &&
             mixed->cold.returned_plan_count == mixed->total_requests &&
             mixed->warm.returned_plan_count == mixed->total_requests &&
             mixed->is_cache_pressure_benchmark == observed_pressure;
    };
    validate_mixed(typical_mixed);
    validate_mixed(envelope_mixed);

#if defined(_WIN32)
    auto validate_frontend_memory = [&](const auto &items, bool envelope_shape) {
        for (const auto &item : items) {
            ok = ok && item.memory_baseline.captured && item.memory_teardown.captured;
            if (config.arm == Arm::e2e) {
                ok = ok && item.memory_after_e2e.captured;
            } else {
                ok = ok && item.memory_after_cold.captured;
            }
            if (config.arm == Arm::warm || config.arm == Arm::all) {
                ok = ok && item.memory_after_warm.captured;
            }
            if (!envelope_shape &&
                (config.arm == Arm::exec ||
                 (config.arm == Arm::all && config.include_execution_on_frontend))) {
                ok = ok && item.memory_after_exec.captured;
            }
            if (!envelope_shape && config.arm == Arm::all && config.include_execution_on_frontend) {
                ok = ok && item.memory_after_e2e.captured;
            }
        }
    };
    validate_frontend_memory(typical, false);
    validate_frontend_memory(envelope, true);
    auto validate_mixed_memory = [&](const std::optional<MixedPressureReport> &mixed) {
        if (!mixed)
            return;
        ok = ok && mixed->memory_baseline.captured && mixed->memory_after_cold.captured &&
             mixed->memory_after_warm.captured && mixed->memory_teardown.captured;
    };
    validate_mixed_memory(typical_mixed);
    validate_mixed_memory(envelope_mixed);
    if (!verification.empty()) {
        ok = ok && verification_memory.before_ring.captured &&
             verification_memory.after_ring_prepare.captured &&
             verification_memory.after_ring_teardown.captured;
    }
#endif

    for (const auto &item : verification) {
        const bool raw_sample_counts_match =
            item.analyze_total_samples_ms.size() == config.timing_samples &&
            item.timing_sample_checksums.size() == config.timing_samples &&
            item.timing_sample_fingerprints.size() == config.timing_samples;
        ok = ok && item.finite && item.logical_frames == config.logical_frames &&
             item.processed_frames == config.logical_frames && item.scalar_reference_matches &&
             item.timing_samples_valid && item.timing_fingerprints_consistent &&
             item.timing_checksums_consistent && raw_sample_counts_match;
        if (config.timing_samples > 1) {
            ok = ok && item.timing_warmup_performed && item.timing_warmup_valid &&
                 item.timing_warmup_matches_retained;
        }
        if (config.timing_samples >= 21) {
            ok = ok && item.formal_timing_eligible;
        }
        if (item.cohort == "decorrelated") {
            ok = ok && item.nonzero_error_count > 0;
        }
#if defined(_WIN32)
        ok = ok && item.memory_before_plan.captured && item.memory_after_plan.captured &&
             item.memory_at_final_frame.captured && item.memory_after_filter_teardown.captured;
        if (config.timing_samples == 1) {
            ok = ok && item.memory_at_frame_1.captured;
            if (config.logical_frames >= 10) {
                ok = ok && item.memory_at_frame_10.captured;
            }
            if (config.logical_frames >= 100) {
                ok = ok && item.memory_at_frame_100.captured;
            }
            if (config.logical_frames >= 1000) {
                ok = ok && item.memory_at_frame_1000.captured;
            }
        }
#endif
    }
    if (config.require_cache_pressure) {
        ok = ok && config.ring_pressure_target_met;
    }
    output << ",\"correctness\":{"
              "\"assertions_pass\":"
           << (ok ? "true" : "false")
           << ",\"checks\":"
              "\"frontend_plan_counts,warm_hits,frontend_sampled_scalar_reference,"
              "finite,processed_count,decorrelated_nonzero,verification_sampled_scalar_reference,"
              "timing_warmup_and_raw_consistency,windows_memory_snapshots; "
              "sampled reference is not a full numeric oracle\""
           << ",\"mixed_cache_pressure_exempt_from_full_warm_hits\":true}";
    output << '}';
    return {output.str(), ok};
}

} // namespace

int main(int argc, char **argv) {
    try {
        Configuration config = parse_arguments(argc, argv);
        const auto filters = selected_filters(config);

        if (config.list_only) {
            std::cout << "suite=" << suite_name(config.suite) << " arm=" << arm_name(config.arm)
                      << " cohort=" << cohort_name(config.cohort)
                      << " profile_mode=" << (config.profile_mode ? "true" : "false")
                      << "\nfilters=";
            for (std::size_t index = 0; index < filters.size(); ++index) {
                if (index != 0)
                    std::cout << ',';
                std::cout << filters[index]->id;
            }
            std::cout << "\ncandidates=" << config.candidate_count
                      << " locked_native_height=" << config.locked_native_height
                      << "\nlogical_frames=" << config.logical_frames
                      << " ring_frames=" << config.ring_frames
                      << "\nframe_workers=" << config.frame_workers
                      << " timing_samples=" << config.timing_samples << '\n';
            return EXIT_SUCCESS;
        }

        std::filesystem::create_directories(config.artifact_root);
        const std::filesystem::path artifact_path =
            config.artifact_root / "cpu-memory-profile-results.json";
        getnative::benchmark::validate_json_output_path(artifact_path);

        MeasurementGate measurement_gate(config.attach_gate);

        const getnative::CpuDispatchInfo dispatch = getnative::cpu_dispatch_info(config.cpu_isa);
        if (!dispatch.request_available) {
            (void)getnative::require_cpu_isa(config.cpu_isa);
        }

        std::cout << "cpu_memory_profile v4 suite=" << suite_name(config.suite)
                  << " arm=" << arm_name(config.arm) << " cohort=" << cohort_name(config.cohort)
                  << " profile_mode=" << (config.profile_mode ? "true" : "false")
                  << " isa=" << getnative::cpu_isa_name(dispatch.selected) << std::endl;

        const bool run_typical =
            config.suite == Suite::all || config.suite == Suite::height_search_typical;
        const bool run_envelope =
            (config.suite == Suite::all || config.suite == Suite::height_envelope) &&
            config.arm != Arm::exec && config.arm != Arm::e2e;
        const bool run_verification =
            config.suite == Suite::verification ||
            (config.suite == Suite::all && (config.arm == Arm::all || config.arm == Arm::exec));

        // The frontend source is not part of planner-only arms.
        std::optional<std::vector<float>> structured_pixels;
        std::optional<getnative::ConstImageView> structured_source;
        const bool need_frontend_source =
            run_typical && (config.arm == Arm::exec || config.arm == Arm::e2e ||
                            (config.arm == Arm::all && config.include_execution_on_frontend));
        if (need_frontend_source) {
            phase_marker("frontend_source_fill");
            structured_pixels.emplace(
                static_cast<std::size_t>(config.source_width)
                    * static_cast<std::size_t>(config.source_height), 0.0F);
            fill_structured_frame(*structured_pixels, config.source_width, config.source_height,
                                  config.source_width, 0);
            structured_source.emplace(structured_pixels->data(), config.source_width,
                                      config.source_height, config.source_width);
        }

        std::vector<FrontendFilterReport> typical;
        std::vector<FrontendFilterReport> envelope;
        std::optional<MixedPressureReport> typical_mixed;
        std::optional<MixedPressureReport> envelope_mixed;
        std::vector<VerificationFilterReport> verification;
        VerificationMemoryLifecycle verification_memory{};
        double structured_ring_prepare_ms = 0.0;
        double decorrelated_ring_prepare_ms = 0.0;
        std::uint64_t source_physical_total_bytes = 0;

        // arm=mixed runs only the interleaved cache-pressure path.
        if (run_typical && config.arm != Arm::mixed) {
            phase_marker("height_search_typical_begin");
            for (const NamedFilter *filter : filters) {
                std::cout << "typical filter=" << filter->id << std::endl;
                typical.push_back(run_height_search_filter(
                    config, *filter, false, structured_source ? &*structured_source : nullptr,
                    measurement_gate));
                const auto &item = typical.back();
                std::cout << "  request_gen_ms=" << item.request_generation_ms
                          << " cold_ms=" << item.cold.wall_ms << " warm_ms=" << item.warm.wall_ms
                          << " exec_ms=" << item.execution.wall_ms
                          << " request_plan_exec_ms=" << item.request_plan_exec_oneshot_ms
                          << std::endl;
            }
            phase_marker("height_search_typical_end");
        }
        if (run_typical && (want_arm(config, Arm::mixed) || want_arm(config, Arm::all))) {
            typical_mixed = run_mixed_cache_pressure(config, false, filters, measurement_gate);
            std::cout << "typical mixed requests=" << typical_mixed->total_requests
                      << " cold_builds=" << typical_mixed->cold.physical_build_count
                      << " warm_hits=" << typical_mixed->warm.ready_hit_count
                      << " warm_builds=" << typical_mixed->warm.physical_build_count << std::endl;
        }

        if (run_envelope && config.arm != Arm::mixed) {
            phase_marker("height_envelope_begin");
            for (const NamedFilter *filter : filters) {
                std::cout << "envelope filter=" << filter->id << std::endl;
                envelope.push_back(run_height_search_filter(
                    config, *filter, true, structured_source ? &*structured_source : nullptr,
                    measurement_gate));
            }
            phase_marker("height_envelope_end");
        }
        if (run_envelope && (want_arm(config, Arm::mixed) || want_arm(config, Arm::all))) {
            envelope_mixed = run_mixed_cache_pressure(config, true, filters, measurement_gate);
        }

        // Drop frontend source before verification to avoid double-counting.
        structured_source.reset();
        structured_pixels.reset();

        if (run_verification) {
            phase_marker("verification_begin");
            verification_memory.before_ring = capture_process_memory();
            const bool want_structured =
                config.cohort == Cohort::both || config.cohort == Cohort::structured;
            const bool want_decorrelated =
                config.cohort == Cohort::both || config.cohort == Cohort::decorrelated;

            std::optional<SourceRing> structured_ring;
            std::optional<SourceRing> decorrelated_ring;
            if (want_structured) {
                phase_marker("ring_prepare_structured");
                const auto begin = Clock::now();
                structured_ring = make_source_ring(config, false);
                structured_ring_prepare_ms = elapsed_ms(begin, Clock::now());
                source_physical_total_bytes += structured_ring->physical_bytes();
            }
            if (want_decorrelated) {
                phase_marker("ring_prepare_decorrelated");
                const auto begin = Clock::now();
                decorrelated_ring = make_source_ring(config, true);
                decorrelated_ring_prepare_ms = elapsed_ms(begin, Clock::now());
                source_physical_total_bytes += decorrelated_ring->physical_bytes();
            }
            std::cout << "source_physical_total_mib="
                      << (static_cast<double>(source_physical_total_bytes) / (1024.0 * 1024.0))
                      << std::endl;
            verification_memory.after_ring_prepare = capture_process_memory();

            for (const NamedFilter *filter : filters) {
                if (structured_ring) {
                    std::cout << "verify structured filter=" << filter->id << std::endl;
                    auto report =
                        run_verification_case(config, *filter, *structured_ring, "structured",
                                              source_physical_total_bytes, measurement_gate);
                    verification.push_back(std::move(report));
                }
                if (decorrelated_ring) {
                    std::cout << "verify decorrelated filter=" << filter->id << std::endl;
                    auto report =
                        run_verification_case(config, *filter, *decorrelated_ring, "decorrelated",
                                              source_physical_total_bytes, measurement_gate);
                    verification.push_back(std::move(report));
                }
            }
            // Explicit ring teardown before final JSON.
            structured_ring.reset();
            decorrelated_ring.reset();
            verification_memory.after_ring_teardown = capture_process_memory();
            phase_marker("verification_end");
        }

        const BenchmarkArtifact artifact =
            make_json(config, dispatch, typical, envelope, typical_mixed, envelope_mixed,
                      verification, structured_ring_prepare_ms, decorrelated_ring_prepare_ms,
                      source_physical_total_bytes, verification_memory, argc, argv);
        getnative::benchmark::atomic_write_json(artifact_path, artifact.json);

        std::cout << "artifact=" << artifact_path.string()
                  << " assertions_pass=" << (artifact.assertions_pass ? "true" : "false")
                  << " source_physical_total_bytes=" << source_physical_total_bytes << std::endl;
        phase_marker("done");
        if (config.assert_correctness && !artifact.assertions_pass) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "cpu memory profile benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

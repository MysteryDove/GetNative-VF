#include "benchmark_support.hpp"

#include "getnative/metal_analysis.hpp"

#include "getnative/filter.hpp"

#include "axis_planner.hpp"
#include "axis_plan_key.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
    bool compare_planner_modes = false;
    bool compare_cross_call_cache = false;
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
    std::size_t unique_key_count = 0;
    std::size_t physical_build_count = 0;
    std::size_t ready_hit_count = 0;
    std::size_t published_plan_count = 0;
    std::size_t resident_entry_count = 0;
    std::size_t resident_bytes = 0;
    std::size_t peak_active_builds = 0;
    std::size_t effective_worker_count = 0;
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
    std::size_t unique_key_count = 0;
    std::size_t physical_build_count = 0;
    std::size_t ready_hit_count = 0;
    std::size_t published_plan_count = 0;
    std::size_t resident_entry_count = 0;
    std::size_t resident_bytes = 0;
    std::size_t peak_active_builds = 0;
    std::size_t effective_worker_count = 0;
};

struct PairedMeasurements {
    Measurements serial;
    Measurements batch;
    std::vector<bool> serial_first;
    getnative::benchmark::Summary plan_delta;
    getnative::benchmark::Summary plan_speedup;
    getnative::benchmark::Summary metal_delta;
    getnative::benchmark::Summary metal_total_delta;
};

struct RetainedCacheFixture {
    getnative::AxisPlanCache cache;
    std::vector<std::shared_ptr<const getnative::AxisPlan>> expected_plans;
    double setup_only_cold_publish_ms = 0.0;
    std::size_t setup_physical_build_count = 0;
    std::size_t setup_published_plan_count = 0;
    std::size_t retained_plan_bytes = 0;
};

struct CachePairedMeasurements {
    Measurements cold_batch;
    Measurements warm_cache;
    std::vector<bool> cold_first;
    getnative::benchmark::Summary setup_only_cold_publish_ms;
    getnative::AxisPlanCacheLimits cache_limits;
    std::size_t setup_physical_build_count = 0;
    std::size_t setup_published_plan_count = 0;
    std::size_t retained_plan_bytes = 0;
    getnative::benchmark::Summary plan_delta;
    getnative::benchmark::Summary plan_speedup;
    getnative::benchmark::Summary cpu_delta;
    getnative::benchmark::Summary cpu_total_delta;
    getnative::benchmark::Summary controlled_cpu_total_delta;
    getnative::benchmark::Summary metal_delta;
    getnative::benchmark::Summary metal_total_delta;
    getnative::benchmark::Summary controlled_metal_total_delta;
};

// Same-binary copy of the cache behavior used before bounded admission.
class LegacyAxisPlanCache {
public:
    [[nodiscard]] std::shared_ptr<const getnative::AxisPlan> get_or_build(
        const getnative::AxisPlanRequest &request) {
        const getnative::detail::PlanKey key = getnative::detail::plan_key(request);
        {
            const std::scoped_lock lock(mutex_);
            if (const auto found = plans_.find(key); found != plans_.end()) {
                return found->second;
            }
        }
        auto candidate = std::make_shared<const getnative::AxisPlan>(
            getnative::build_axis_plan(request));
        const std::scoped_lock lock(mutex_);
        const auto [position, inserted] = plans_.emplace(key, candidate);
        return inserted ? std::move(candidate) : position->second;
    }

    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock lock(mutex_);
        return plans_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<
        getnative::detail::PlanKey,
        std::shared_ptr<const getnative::AxisPlan>,
        getnative::detail::PlanKeyHash> plans_;
};

enum class PlannerPath : std::uint8_t {
    serial,
    batch,
    session_cache_cold,
    session_cache_warm,
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
    bool planner_mode_explicit = false;
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
        } else if (argument == "--native-height" && index + 1 < argc) {
            const std::size_t native_height = parse_size(argv[++index]);
            if (native_height > static_cast<std::size_t>(
                                    std::numeric_limits<std::int32_t>::max())) {
                throw std::invalid_argument("native height exceeds int32 range");
            }
            result.native_height = static_cast<std::int32_t>(native_height);
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
            planner_mode_explicit = true;
        } else if (argument == "--compare-planner-modes") {
            result.compare_planner_modes = true;
        } else if (argument == "--compare-cross-call-cache"
                   || argument == "--compare-session-cache") {
            result.compare_cross_call_cache = true;
        } else if (argument == "--json-out" && index + 1 < argc) {
            result.json_output = std::filesystem::path{argv[++index]};
        } else {
            throw std::invalid_argument(
                "usage: getnative_metal_benchmark [--full] [--candidates N] "
                "[--native-height N] "
                "[--tile-size N] [--reduction-groups N] [--inverse-threads N] "
                "[--samples N] [--planner-mode serial | --compare-planner-modes | "
                "--compare-session-cache] "
                "[--json-out PATH] [--assert] "
                "[--profile-split-kernels] "
                "[--kernel bilinear|bicubic-catrom|bicubic-mitchell|"
                "spline16|spline36|spline64|lanczos1..lanczos8]");
        }
    }
    const unsigned selected_modes = static_cast<unsigned>(planner_mode_explicit)
        + static_cast<unsigned>(result.compare_planner_modes)
        + static_cast<unsigned>(result.compare_cross_call_cache);
    if (selected_modes > 1U) {
        throw std::invalid_argument(
            "planner comparison modes are mutually exclusive");
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
    if (name == "lanczos1") return getnative::Filter::lanczos(1);
    if (name == "lanczos2") return getnative::Filter::lanczos(2);
    if (name == "lanczos3") return getnative::Filter::lanczos(3);
    if (name == "lanczos4") return getnative::Filter::lanczos(4);
    if (name == "lanczos5") return getnative::Filter::lanczos(5);
    if (name == "lanczos6") return getnative::Filter::lanczos(6);
    if (name == "lanczos7") return getnative::Filter::lanczos(7);
    if (name == "lanczos8") return getnative::Filter::lanczos(8);
    throw std::invalid_argument(
        "unsupported benchmark kernel");
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

void prewarm_retained_cache(
    const BenchmarkFixture &fixture,
    RetainedCacheFixture &retained) {
    const auto start = Clock::now();
    auto result = retained.cache.get_or_build_batch(fixture.requests);
    retained.setup_only_cold_publish_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    retained.setup_physical_build_count = result.physical_build_count;
    retained.setup_published_plan_count = result.published_plan_count;
    retained.retained_plan_bytes = result.resident_bytes;
    retained.expected_plans = std::move(result.plans);
    const getnative::AxisPlanCacheLimits limits = retained.cache.limits();
    std::size_t predicted_resident_count = 0U;
    std::size_t predicted_resident_bytes = 0U;
    for (auto &plan : retained.expected_plans) {
        const std::size_t plan_bytes = getnative::axis_plan_storage_bytes(*plan);
        const bool admitted = predicted_resident_count < limits.maximum_entries
            && plan_bytes <= limits.maximum_resident_bytes - predicted_resident_bytes;
        if (admitted) {
            ++predicted_resident_count;
            predicted_resident_bytes += plan_bytes;
        } else {
            plan.reset();
        }
    }
    if (result.unique_key_count != fixture.requests.size()
        || retained.expected_plans.size() != fixture.requests.size()
        || result.ready_hit_count != 0U
        || retained.setup_physical_build_count != result.unique_key_count
        || retained.setup_published_plan_count != result.resident_entry_count
        || predicted_resident_count != result.resident_entry_count
        || predicted_resident_bytes != result.resident_bytes
        || retained.cache.size() != result.resident_entry_count
        || retained.cache.resident_bytes() != result.resident_bytes
        || result.published_plan_count > result.unique_key_count) {
        throw std::runtime_error("production session-cache prewarm invariant changed");
    }
}

[[nodiscard]] Sample run_sample(
    const Configuration &config,
    const BenchmarkFixture &fixture,
    getnative::MetalAnalysisEngine &metal,
    const getnative::MetricSpec &metric,
    PlannerPath planner_path,
    RetainedCacheFixture *retained = nullptr) {
    const getnative::ConstImageView view{
        fixture.source.data(), config.width, config.height, config.width,
    };
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(fixture.requests.size());
    std::unique_ptr<getnative::AxisPlanCache> cache;
    std::unique_ptr<LegacyAxisPlanCache> legacy_cache;
    if (planner_path == PlannerPath::serial) {
        legacy_cache = std::make_unique<LegacyAxisPlanCache>();
    } else if (planner_path == PlannerPath::session_cache_cold) {
        cache = std::make_unique<getnative::AxisPlanCache>();
    }
    getnative::detail::AxisPlanBatchResult batch_result;
    getnative::AxisPlanCacheBatchResult cache_batch_result;

    const auto plan_start = Clock::now();
    if (planner_path == PlannerPath::batch) {
        batch_result = getnative::detail::build_axis_plans(fixture.requests);
        for (std::size_t index = 0; index < fixture.requests.size(); ++index) {
            candidates.push_back({
                fixture.candidate_ids[index], nullptr, batch_result.plans[index],
                getnative::AnalysisAxes::vertical,
            });
        }
    } else if (planner_path == PlannerPath::serial) {
        for (std::size_t index = 0; index < fixture.requests.size(); ++index) {
            candidates.push_back({
                fixture.candidate_ids[index], nullptr,
                legacy_cache->get_or_build(fixture.requests[index]),
                getnative::AnalysisAxes::vertical,
            });
        }
    } else if (planner_path == PlannerPath::session_cache_cold) {
        cache_batch_result = cache->get_or_build_batch(fixture.requests);
        for (std::size_t index = 0; index < fixture.requests.size(); ++index) {
            candidates.push_back({
                fixture.candidate_ids[index], nullptr, cache_batch_result.plans[index],
                getnative::AnalysisAxes::vertical,
            });
        }
    } else {
        if (retained == nullptr
            || retained->expected_plans.size() != fixture.requests.size()) {
            throw std::invalid_argument("retained cache fixture is not initialized");
        }
        cache_batch_result = retained->cache.get_or_build_batch(fixture.requests);
        std::size_t reused_plan_count = 0U;
        for (std::size_t index = 0; index < fixture.requests.size(); ++index) {
            auto plan = cache_batch_result.plans[index];
            if (plan.get() == retained->expected_plans[index].get()) {
                ++reused_plan_count;
            }
            candidates.push_back({
                fixture.candidate_ids[index], nullptr, std::move(plan),
                getnative::AnalysisAxes::vertical,
            });
        }
        if (reused_plan_count != cache_batch_result.ready_hit_count) {
            throw std::runtime_error(
                "cross-call cache ready-hit pointers did not match telemetry");
        }
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
    const bool batch_planner = planner_path == PlannerPath::batch;
    const bool serial_planner = planner_path == PlannerPath::serial;
    const bool session_cache = planner_path == PlannerPath::session_cache_cold
        || planner_path == PlannerPath::session_cache_warm;
    const std::size_t unique_key_count = batch_planner
        ? batch_result.unique_key_count
        : serial_planner ? legacy_cache->size() : cache_batch_result.unique_key_count;
    const std::size_t physical_build_count = batch_planner
        ? batch_result.physical_build_count
        : serial_planner ? legacy_cache->size() : cache_batch_result.physical_build_count;
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
        unique_key_count,
        physical_build_count,
        session_cache ? cache_batch_result.ready_hit_count : 0U,
        session_cache ? cache_batch_result.published_plan_count : 0U,
        session_cache ? cache_batch_result.resident_entry_count : 0U,
        session_cache ? cache_batch_result.resident_bytes : 0U,
        batch_planner ? batch_result.peak_active_builds
                      : session_cache ? cache_batch_result.peak_active_builds : 1U,
        batch_planner ? batch_result.effective_worker_count
                      : session_cache ? cache_batch_result.effective_worker_count : 1U,
    };
}

[[nodiscard]] Measurements summarize_samples(
    const std::vector<Sample> &samples,
    bool warmup_within_tolerance) {
    std::vector<double> plan_samples;
    std::vector<double> cpu_samples;
    std::vector<double> metal_samples;
    std::vector<double> cpu_total_samples;
    std::vector<double> metal_total_samples;
    std::vector<double> share_samples;
    std::vector<double> error_samples;
    std::vector<double> valley_samples;
    for (auto *values : {&plan_samples, &cpu_samples, &metal_samples, &cpu_total_samples,
                         &metal_total_samples, &share_samples, &error_samples, &valley_samples}) {
        values->reserve(samples.size());
    }

    bool within_tolerance = warmup_within_tolerance;
    std::size_t peak_active_builds = 0U;
    const std::size_t effective_worker_count = samples.front().effective_worker_count;
    const std::size_t ready_hit_count = samples.front().ready_hit_count;
    const std::size_t published_plan_count = samples.front().published_plan_count;
    const std::size_t resident_entry_count = samples.front().resident_entry_count;
    const std::size_t resident_bytes = samples.front().resident_bytes;
    for (const Sample &sample : samples) {
        plan_samples.push_back(sample.plan_ms);
        cpu_samples.push_back(sample.cpu_ms);
        metal_samples.push_back(sample.metal_ms);
        cpu_total_samples.push_back(sample.cpu_total_ms);
        metal_total_samples.push_back(sample.metal_total_ms);
        share_samples.push_back(sample.plan_share);
        error_samples.push_back(sample.maximum_error);
        valley_samples.push_back(sample.valley_distance);
        within_tolerance = within_tolerance && sample.within_tolerance;
        peak_active_builds = std::max(peak_active_builds, sample.peak_active_builds);
        if (sample.unique_key_count != samples.front().unique_key_count
            || sample.physical_build_count != samples.front().physical_build_count
            || sample.ready_hit_count != ready_hit_count
            || sample.published_plan_count != published_plan_count
            || sample.resident_entry_count != resident_entry_count
            || sample.resident_bytes != resident_bytes
            || sample.effective_worker_count != effective_worker_count) {
            throw std::runtime_error("Metal paired planner counts changed between samples");
        }
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
        samples.front().unique_key_count,
        samples.front().physical_build_count,
        ready_hit_count,
        published_plan_count,
        resident_entry_count,
        resident_bytes,
        peak_active_builds,
        effective_worker_count,
    };
}

void validate_planner_sample(const Sample &sample, std::size_t expected_unique_count) {
    if (sample.unique_key_count != expected_unique_count
        || sample.physical_build_count != expected_unique_count
        || sample.ready_hit_count != 0U
        || sample.peak_active_builds > sample.effective_worker_count) {
        throw std::runtime_error(
            "Metal staged benchmark planner invariant changed: expected_unique="
            + std::to_string(expected_unique_count)
            + " unique=" + std::to_string(sample.unique_key_count)
            + " physical=" + std::to_string(sample.physical_build_count)
            + " ready=" + std::to_string(sample.ready_hit_count)
            + " peak=" + std::to_string(sample.peak_active_builds)
            + " workers=" + std::to_string(sample.effective_worker_count));
    }
}

[[nodiscard]] bool build_activity_is_valid(const Sample &sample) {
    if (sample.physical_build_count == 0U) {
        return sample.peak_active_builds == 0U
            && sample.effective_worker_count == 0U;
    }
    return sample.peak_active_builds > 0U
        && sample.effective_worker_count > 0U
        && sample.peak_active_builds <= sample.effective_worker_count;
}

void validate_warm_cache_sample(
    const Sample &sample,
    std::size_t expected_unique_count,
    std::size_t expected_resident_count,
    std::size_t expected_resident_bytes) {
    if (expected_resident_count > expected_unique_count) {
        throw std::invalid_argument("resident plan count exceeds unique plan count");
    }
    if (sample.unique_key_count != expected_unique_count
        || sample.physical_build_count != expected_unique_count - expected_resident_count
        || sample.ready_hit_count != expected_resident_count
        || sample.published_plan_count != 0U
        || sample.resident_entry_count != expected_resident_count
        || sample.resident_bytes != expected_resident_bytes
        || !build_activity_is_valid(sample)) {
        throw std::runtime_error("Metal cross-call cache benchmark invariant changed");
    }
}

void validate_cold_session_cache_sample(
    const Sample &sample,
    std::size_t expected_unique_count,
    std::size_t expected_resident_count,
    std::size_t expected_resident_bytes) {
    if (sample.unique_key_count != expected_unique_count
        || sample.physical_build_count != expected_unique_count
        || sample.ready_hit_count != 0U
        || sample.published_plan_count != expected_resident_count
        || sample.resident_entry_count != expected_resident_count
        || sample.resident_bytes != expected_resident_bytes
        || !build_activity_is_valid(sample)) {
        throw std::runtime_error("Metal cold session-cache invariant changed");
    }
}

[[nodiscard]] Measurements measure(
    const Configuration &config,
    const BenchmarkFixture &fixture,
    getnative::MetalAnalysisEngine &metal,
    const getnative::MetricSpec &metric,
    bool batch_planner) {
    const PlannerPath planner_path = batch_planner
        ? PlannerPath::batch : PlannerPath::serial;
    const Sample warmup = run_sample(config, fixture, metal, metric, planner_path);
    const std::size_t expected_unique_count = warmup.unique_key_count;
    validate_planner_sample(warmup, expected_unique_count);
    std::vector<Sample> samples;
    samples.reserve(config.samples);
    for (std::size_t index = 0; index < config.samples; ++index) {
        Sample sample = run_sample(config, fixture, metal, metric, planner_path);
        validate_planner_sample(sample, expected_unique_count);
        samples.push_back(std::move(sample));
    }
    return summarize_samples(samples, warmup.within_tolerance);
}

[[nodiscard]] PairedMeasurements measure_pairs(
    const Configuration &config,
    const BenchmarkFixture &fixture,
    getnative::MetalAnalysisEngine &metal,
    const getnative::MetricSpec &metric) {
    const Sample serial_warmup = run_sample(
        config, fixture, metal, metric, PlannerPath::serial);
    const Sample batch_warmup = run_sample(
        config, fixture, metal, metric, PlannerPath::batch);
    const std::size_t expected_unique_count = serial_warmup.unique_key_count;
    validate_planner_sample(serial_warmup, expected_unique_count);
    validate_planner_sample(batch_warmup, expected_unique_count);
    if (serial_warmup.unique_key_count != batch_warmup.unique_key_count
        || serial_warmup.maximum_error != batch_warmup.maximum_error
        || serial_warmup.valley_distance != batch_warmup.valley_distance) {
        throw std::runtime_error("Metal paired warmup changed correctness output");
    }

    std::vector<Sample> serial_samples;
    std::vector<Sample> batch_samples;
    std::vector<bool> serial_first;
    std::vector<double> plan_deltas;
    std::vector<double> plan_speedups;
    std::vector<double> metal_deltas;
    std::vector<double> metal_total_deltas;
    serial_samples.reserve(config.samples);
    batch_samples.reserve(config.samples);
    serial_first.reserve(config.samples);
    plan_deltas.reserve(config.samples);
    plan_speedups.reserve(config.samples);
    metal_deltas.reserve(config.samples);
    metal_total_deltas.reserve(config.samples);

    for (std::size_t sample_index = 0; sample_index < config.samples; ++sample_index) {
        const bool run_serial_first = (sample_index & 1U) == 0U;
        Sample serial;
        Sample batch;
        if (run_serial_first) {
            serial = run_sample(config, fixture, metal, metric, PlannerPath::serial);
            batch = run_sample(config, fixture, metal, metric, PlannerPath::batch);
        } else {
            batch = run_sample(config, fixture, metal, metric, PlannerPath::batch);
            serial = run_sample(config, fixture, metal, metric, PlannerPath::serial);
        }
        validate_planner_sample(serial, expected_unique_count);
        validate_planner_sample(batch, expected_unique_count);
        if (serial.maximum_error != batch.maximum_error
            || serial.valley_distance != batch.valley_distance) {
            throw std::runtime_error("Metal paired sample changed correctness output");
        }
        serial_first.push_back(run_serial_first);
        plan_deltas.push_back((batch.plan_ms - serial.plan_ms) / serial.plan_ms);
        plan_speedups.push_back(serial.plan_ms / batch.plan_ms);
        metal_deltas.push_back((batch.metal_ms - serial.metal_ms) / serial.metal_ms);
        metal_total_deltas.push_back(
            (batch.metal_total_ms - serial.metal_total_ms) / serial.metal_total_ms);
        serial_samples.push_back(std::move(serial));
        batch_samples.push_back(std::move(batch));
    }

    return {
        summarize_samples(serial_samples, serial_warmup.within_tolerance),
        summarize_samples(batch_samples, batch_warmup.within_tolerance),
        std::move(serial_first),
        getnative::benchmark::summarize(std::move(plan_deltas)),
        getnative::benchmark::summarize(std::move(plan_speedups)),
        getnative::benchmark::summarize(std::move(metal_deltas)),
        getnative::benchmark::summarize(std::move(metal_total_deltas)),
    };
}

[[nodiscard]] CachePairedMeasurements measure_cache_pairs(
    const Configuration &config,
    const BenchmarkFixture &fixture,
    getnative::MetalAnalysisEngine &metal,
    const getnative::MetricSpec &metric) {
    const Sample cold_warmup = run_sample(
        config, fixture, metal, metric, PlannerPath::session_cache_cold);
    RetainedCacheFixture warmup_retained;
    prewarm_retained_cache(fixture, warmup_retained);
    const Sample warm_warmup = run_sample(
        config, fixture, metal, metric, PlannerPath::session_cache_warm, &warmup_retained);
    const std::size_t expected_unique_count = cold_warmup.unique_key_count;
    const std::size_t expected_resident_count =
        warmup_retained.setup_published_plan_count;
    const std::size_t expected_resident_bytes = warmup_retained.retained_plan_bytes;
    validate_cold_session_cache_sample(
        cold_warmup, expected_unique_count,
        expected_resident_count, expected_resident_bytes);
    validate_warm_cache_sample(
        warm_warmup, expected_unique_count,
        expected_resident_count, expected_resident_bytes);
    if (cold_warmup.maximum_error != warm_warmup.maximum_error
        || cold_warmup.valley_distance != warm_warmup.valley_distance) {
        throw std::runtime_error("Metal cache paired warmup changed correctness output");
    }

    std::vector<Sample> cold_samples;
    std::vector<Sample> warm_samples;
    std::vector<bool> cold_first;
    std::vector<double> plan_deltas;
    std::vector<double> plan_speedups;
    std::vector<double> cpu_deltas;
    std::vector<double> cpu_total_deltas;
    std::vector<double> controlled_cpu_total_deltas;
    std::vector<double> metal_deltas;
    std::vector<double> metal_total_deltas;
    std::vector<double> controlled_metal_total_deltas;
    std::vector<double> prewarm_samples;
    for (auto *values : {&plan_deltas, &plan_speedups,
                         &cpu_deltas, &cpu_total_deltas,
                         &controlled_cpu_total_deltas, &metal_deltas,
                         &metal_total_deltas, &controlled_metal_total_deltas,
                         &prewarm_samples}) {
        values->reserve(config.samples);
    }
    cold_samples.reserve(config.samples);
    warm_samples.reserve(config.samples);
    cold_first.reserve(config.samples);
    const std::size_t retained_plan_bytes = expected_resident_bytes;

    const auto run_warm = [&] {
        RetainedCacheFixture retained;
        prewarm_retained_cache(fixture, retained);
        prewarm_samples.push_back(retained.setup_only_cold_publish_ms);
        if (retained.setup_physical_build_count != expected_unique_count
            || retained.setup_published_plan_count != expected_resident_count
            || retained.retained_plan_bytes != retained_plan_bytes) {
            throw std::runtime_error(
                "retained cache setup counts changed between samples");
        }
        return run_sample(
            config, fixture, metal, metric, PlannerPath::session_cache_warm, &retained);
    };

    for (std::size_t sample_index = 0; sample_index < config.samples; ++sample_index) {
        const bool run_cold_first = (sample_index & 1U) == 0U;
        Sample cold;
        Sample warm;
        if (run_cold_first) {
            cold = run_sample(
                config, fixture, metal, metric, PlannerPath::session_cache_cold);
            warm = run_warm();
        } else {
            warm = run_warm();
            cold = run_sample(
                config, fixture, metal, metric, PlannerPath::session_cache_cold);
        }
        validate_cold_session_cache_sample(
            cold, expected_unique_count,
            expected_resident_count, retained_plan_bytes);
        validate_warm_cache_sample(
            warm, expected_unique_count,
            expected_resident_count, retained_plan_bytes);
        if (cold.maximum_error != warm.maximum_error
            || cold.valley_distance != warm.valley_distance) {
            throw std::runtime_error("Metal cache paired sample changed correctness output");
        }
        cold_first.push_back(run_cold_first);
        plan_deltas.push_back((warm.plan_ms - cold.plan_ms) / cold.plan_ms);
        plan_speedups.push_back(cold.plan_ms / warm.plan_ms);
        cpu_deltas.push_back((warm.cpu_ms - cold.cpu_ms) / cold.cpu_ms);
        cpu_total_deltas.push_back(
            (warm.cpu_total_ms - cold.cpu_total_ms) / cold.cpu_total_ms);
        const double cpu_execution_control = (cold.cpu_ms + warm.cpu_ms) / 2.0;
        const double controlled_cold_cpu_total = cold.plan_ms + cpu_execution_control;
        const double controlled_warm_cpu_total = warm.plan_ms + cpu_execution_control;
        controlled_cpu_total_deltas.push_back(
            (controlled_warm_cpu_total - controlled_cold_cpu_total)
            / controlled_cold_cpu_total);
        metal_deltas.push_back((warm.metal_ms - cold.metal_ms) / cold.metal_ms);
        metal_total_deltas.push_back(
            (warm.metal_total_ms - cold.metal_total_ms) / cold.metal_total_ms);
        const double execution_control = (cold.metal_ms + warm.metal_ms) / 2.0;
        const double controlled_cold_total = cold.plan_ms + execution_control;
        const double controlled_warm_total = warm.plan_ms + execution_control;
        controlled_metal_total_deltas.push_back(
            (controlled_warm_total - controlled_cold_total) / controlled_cold_total);
        cold_samples.push_back(std::move(cold));
        warm_samples.push_back(std::move(warm));
    }

    return {
        summarize_samples(cold_samples, cold_warmup.within_tolerance),
        summarize_samples(warm_samples, warm_warmup.within_tolerance),
        std::move(cold_first),
        getnative::benchmark::summarize(std::move(prewarm_samples)),
        warmup_retained.cache.limits(),
        warmup_retained.setup_physical_build_count,
        warmup_retained.setup_published_plan_count,
        retained_plan_bytes,
        getnative::benchmark::summarize(std::move(plan_deltas)),
        getnative::benchmark::summarize(std::move(plan_speedups)),
        getnative::benchmark::summarize(std::move(cpu_deltas)),
        getnative::benchmark::summarize(std::move(cpu_total_deltas)),
        getnative::benchmark::summarize(std::move(controlled_cpu_total_deltas)),
        getnative::benchmark::summarize(std::move(metal_deltas)),
        getnative::benchmark::summarize(std::move(metal_total_deltas)),
        getnative::benchmark::summarize(std::move(controlled_metal_total_deltas)),
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
        output, "getnative_metal_benchmark", "serial",
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
           << ",\"cache_size\":" << measurements.unique_key_count
           << ",\"physical_build_count\":" << measurements.physical_build_count
           << ",\"ready_hit_count\":" << measurements.ready_hit_count
           << ",\"peak_active_builds\":" << measurements.peak_active_builds
           << ",\"effective_worker_count\":" << measurements.effective_worker_count
           << ",\"metal_peak_workspace_bytes\":" << workspace_bytes
           << ",\"metal_peak_working_set_bytes\":" << working_set_bytes
           << ",\"assertions\":" << (assertions_pass ? "true" : "false")
           << "}}\n";
    return output.str();
}

void append_measurements(std::ostream &output, const Measurements &measurements) {
    output << "{\"metrics\":{\"plan_ms\":";
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
    output << "},\"correctness\":{\"maximum_metric_error\":";
    getnative::benchmark::append_summary(output, measurements.maximum_error);
    output << ",\"valley_step_distance\":";
    getnative::benchmark::append_summary(output, measurements.valley_distance);
    output << ",\"strict_tolerance\":"
           << (measurements.within_tolerance ? "true" : "false")
           << "},\"planner\":{\"unique_key_count\":"
           << measurements.unique_key_count
           << ",\"physical_build_count\":" << measurements.physical_build_count
           << ",\"ready_hit_count\":" << measurements.ready_hit_count
           << ",\"published_plan_count\":" << measurements.published_plan_count
           << ",\"resident_entry_count\":" << measurements.resident_entry_count
           << ",\"resident_bytes\":" << measurements.resident_bytes
           << ",\"peak_active_builds\":" << measurements.peak_active_builds
           << ",\"effective_worker_count\":" << measurements.effective_worker_count
           << "}}";
}

void append_pair_order(
    std::ostream &output,
    const std::vector<bool> &first_mode,
    std::string_view first_then_second,
    std::string_view second_then_first) {
    output << '[';
    for (std::size_t index = 0; index < first_mode.size(); ++index) {
        if (index != 0U) output << ',';
        output << getnative::benchmark::json_string(
            first_mode[index] ? first_then_second : second_then_first);
    }
    output << ']';
}

[[nodiscard]] std::string stage1_measurement_status(
    const Configuration &config,
    const PairedMeasurements &measurements,
    bool assertions_pass) {
    if (!assertions_pass) return "STAGE1_BLOCKED";
    if (config.samples != 21U) return "TUNING_ONLY";
    if (measurements.plan_delta.mad > 0.025
        || measurements.metal_total_delta.mad > 0.025) {
        return "NO_DECISION_NOISY";
    }
    return "MEASURED";
}

[[nodiscard]] std::string make_compare_json(
    const Configuration &config,
    const PairedMeasurements &measurements,
    const getnative::MetalAnalysisEngine &metal,
    std::size_t workspace_bytes,
    std::size_t working_set_bytes,
    bool assertions_pass,
    int argc,
    char **argv) {
    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_metal_benchmark", "compare",
        "synthetic-lcg-vertical-active-scan-v1", argc, argv);
    output << ",\"sample_count\":" << config.samples
           << ",\"warmup_count_per_mode\":1"
           << ",\"stage1_measurement_status\":"
           << getnative::benchmark::json_string(
                  stage1_measurement_status(config, measurements, assertions_pass))
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
           << ",\"pair_order\":";
    append_pair_order(
        output, measurements.serial_first,
        "serial_then_batch", "batch_then_serial");
    output << ",\"serial\":";
    append_measurements(output, measurements.serial);
    output << ",\"batch\":";
    append_measurements(output, measurements.batch);
    output << ",\"paired\":{\"plan_delta\":";
    getnative::benchmark::append_summary(output, measurements.plan_delta);
    output << ",\"plan_speedup\":";
    getnative::benchmark::append_summary(output, measurements.plan_speedup);
    output << ",\"metal_delta\":";
    getnative::benchmark::append_summary(output, measurements.metal_delta);
    output << ",\"metal_total_delta\":";
    getnative::benchmark::append_summary(output, measurements.metal_total_delta);
    output << "},\"correctness\":{\"maximum_metric_error\":"
           << std::setprecision(17)
           << std::max(measurements.serial.maximum_error.maximum,
                       measurements.batch.maximum_error.maximum)
           << ",\"valley_step_distance\":"
           << std::max(measurements.serial.valley_distance.maximum,
                       measurements.batch.valley_distance.maximum)
           << ",\"strict_tolerance\":"
           << (measurements.serial.within_tolerance
                   && measurements.batch.within_tolerance ? "true" : "false")
           << ",\"metal_peak_workspace_bytes\":" << workspace_bytes
           << ",\"metal_peak_working_set_bytes\":" << working_set_bytes
           << ",\"assertions\":" << (assertions_pass ? "true" : "false")
           << "}}\n";
    return output.str();
}

[[nodiscard]] std::string cache_baseline_status(
    const Configuration &config,
    const CachePairedMeasurements &measurements,
    bool assertions_pass) {
    if (!assertions_pass) return "CACHE_BASELINE_BLOCKED";
    if (config.samples != 21U) return "TUNING_ONLY";
    if (measurements.plan_delta.mad > 0.025
        || measurements.controlled_metal_total_delta.mad > 0.025) {
        return "NO_DECISION_NOISY";
    }
    return "MEASURED";
}

[[nodiscard]] std::string cache_baseline_decision(
    const Configuration &config,
    const CachePairedMeasurements &measurements,
    bool assertions_pass) {
    const std::string status = cache_baseline_status(
        config, measurements, assertions_pass);
    if (status != "MEASURED") return status;
    return measurements.controlled_metal_total_delta.median <= -0.05
            && measurements.metal_delta.median <= 0.05
        ? "PROCEED_SESSION_CACHE_STAGE"
        : "STOP_NO_MEASURED_CACHE_BENEFIT";
}

[[nodiscard]] std::string session_cache_decision(
    const Configuration &config,
    const CachePairedMeasurements &measurements,
    bool assertions_pass) {
    const std::string status = cache_baseline_status(
        config, measurements, assertions_pass);
    if (status != "MEASURED") return status;
    return measurements.controlled_metal_total_delta.median <= -0.05
            && measurements.metal_delta.median <= 0.05
        ? "ADOPT_SESSION_CACHE"
        : "REVERT_SESSION_CACHE";
}

[[nodiscard]] getnative::benchmark::Summary amortized_video_improvement(
    const CachePairedMeasurements &measurements,
    std::size_t frame_count) {
    std::vector<double> improvements;
    improvements.reserve(measurements.cold_batch.metal_total_ms.raw.size());
    for (std::size_t index = 0;
         index < measurements.cold_batch.metal_total_ms.raw.size(); ++index) {
        const double execution_control = (
            measurements.cold_batch.metal_ms.raw[index]
            + measurements.warm_cache.metal_ms.raw[index]) / 2.0;
        const double cold = measurements.cold_batch.plan_ms.raw[index] + execution_control;
        const double warm = measurements.warm_cache.plan_ms.raw[index] + execution_control;
        const double amortized = (
            cold + static_cast<double>(frame_count - 1U) * warm)
            / static_cast<double>(frame_count);
        improvements.push_back((cold - amortized) / cold);
    }
    return getnative::benchmark::summarize(std::move(improvements));
}

void append_amortized_video_results(
    std::ostream &output,
    const CachePairedMeasurements &measurements) {
    constexpr std::array<std::size_t, 5> frame_counts{2U, 5U, 10U, 100U, 1000U};
    output << '[';
    for (std::size_t index = 0; index < frame_counts.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"frame_count\":" << frame_counts[index]
               << ",\"metal_total_improvement\":";
        getnative::benchmark::append_summary(
            output, amortized_video_improvement(measurements, frame_counts[index]));
        output << '}';
    }
    output << ']';
}

[[nodiscard]] std::string make_cache_compare_json(
    const Configuration &config,
    const CachePairedMeasurements &measurements,
    const getnative::MetalAnalysisEngine &metal,
    std::size_t workspace_bytes,
    std::size_t working_set_bytes,
    bool assertions_pass,
    int argc,
    char **argv) {
    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_metal_benchmark", "session_cache_compare",
        "synthetic-lcg-production-session-cache-v2", argc, argv);
    output << ",\"sample_count\":" << config.samples
           << ",\"warmup_count_per_mode\":1"
           << ",\"cache_baseline_status\":"
           << getnative::benchmark::json_string(
                  cache_baseline_status(config, measurements, assertions_pass))
           << ",\"cache_baseline_decision\":"
           << getnative::benchmark::json_string(
                  cache_baseline_decision(config, measurements, assertions_pass))
           << ",\"session_cache_status\":"
           << getnative::benchmark::json_string(
                  cache_baseline_status(config, measurements, assertions_pass))
           << ",\"session_cache_decision\":"
           << getnative::benchmark::json_string(
                  session_cache_decision(config, measurements, assertions_pass))
           << ",\"device\":" << getnative::benchmark::json_string(metal.device_info().name)
           << ",\"fixture\":{\"width\":" << config.width
           << ",\"height\":" << config.height
           << ",\"candidates\":" << config.candidates
           << ",\"native_height\":" << config.native_height
           << ",\"kernel\":" << getnative::benchmark::json_string(config.kernel)
           << ",\"cross_call_key_overlap\":1}"
           << ",\"configuration\":{\"tile_size\":" << config.tile_size
           << ",\"reduction_groups\":" << config.reduction_groups
           << ",\"inverse_threads\":" << config.inverse_threads
           << ",\"profile_split_kernels\":"
           << (config.profile_split_kernels ? "true" : "false") << '}'
           << ",\"retained_cache\":{\"production_api\":"
           << getnative::benchmark::json_string("AxisPlanCache::get_or_build_batch")
           << ",\"maximum_entries\":" << measurements.cache_limits.maximum_entries
           << ",\"maximum_resident_bytes\":"
           << measurements.cache_limits.maximum_resident_bytes
           << ",\"entry_count\":" << measurements.warm_cache.resident_entry_count
           << ",\"logical_plan_bytes\":" << measurements.retained_plan_bytes
           << ",\"ready_hit_count\":" << measurements.warm_cache.ready_hit_count
           << ",\"ready_hit_rate\":" << std::setprecision(17)
           << static_cast<double>(measurements.warm_cache.ready_hit_count)
                  / static_cast<double>(measurements.warm_cache.unique_key_count)
           << ",\"setup_physical_build_count\":"
           << measurements.setup_physical_build_count
           << ",\"setup_published_plan_count\":"
           << measurements.setup_published_plan_count
           << ",\"setup_only_cold_publish_ms\":";
    getnative::benchmark::append_summary(
        output, measurements.setup_only_cold_publish_ms);
    output << '}'
           << ",\"pair_order\":";
    append_pair_order(
        output, measurements.cold_first,
        "cold_session_then_warm_session", "warm_session_then_cold_session");
    output << ",\"cold_batch\":";
    append_measurements(output, measurements.cold_batch);
    output << ",\"warm_cache\":";
    append_measurements(output, measurements.warm_cache);
    output << ",\"paired\":{\"plan_delta\":";
    getnative::benchmark::append_summary(output, measurements.plan_delta);
    output << ",\"plan_speedup\":";
    getnative::benchmark::append_summary(output, measurements.plan_speedup);
    output << ",\"cpu_delta\":";
    getnative::benchmark::append_summary(output, measurements.cpu_delta);
    output << ",\"cpu_total_delta\":";
    getnative::benchmark::append_summary(output, measurements.cpu_total_delta);
    output << ",\"controlled_cpu_total_delta\":";
    getnative::benchmark::append_summary(
        output, measurements.controlled_cpu_total_delta);
    output << ",\"metal_delta\":";
    getnative::benchmark::append_summary(output, measurements.metal_delta);
    output << ",\"metal_total_delta\":";
    getnative::benchmark::append_summary(output, measurements.metal_total_delta);
    output << ",\"controlled_metal_total_delta\":";
    getnative::benchmark::append_summary(
        output, measurements.controlled_metal_total_delta);
    output << "},\"amortized_video\":{\"model\":"
           << getnative::benchmark::json_string(
                  "one measured production cold miss/build/publish call followed by "
                  "measured production bounded-cache calls; resident plans are ready hits "
                  "and non-resident plans are rebuilt; "
                  "each pair shares the mean of its two measured Metal executions and "
                  "fixture setup is excluded")
           << ",\"frames\":";
    append_amortized_video_results(output, measurements);
    output << "},\"correctness\":{\"maximum_metric_error\":"
           << std::setprecision(17)
           << std::max(measurements.cold_batch.maximum_error.maximum,
                       measurements.warm_cache.maximum_error.maximum)
           << ",\"valley_step_distance\":"
           << std::max(measurements.cold_batch.valley_distance.maximum,
                       measurements.warm_cache.valley_distance.maximum)
           << ",\"strict_tolerance\":"
           << (measurements.cold_batch.within_tolerance
                   && measurements.warm_cache.within_tolerance ? "true" : "false")
           << ",\"warm_ready_hits_match_resident_entries\":"
           << (measurements.warm_cache.ready_hit_count
                       == measurements.warm_cache.resident_entry_count
                   ? "true" : "false")
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

        if (config.compare_cross_call_cache) {
            const CachePairedMeasurements paired = measure_cache_pairs(
                config, fixture, metal, metric);
            const std::size_t workspace_bytes = metal.peak_workspace_elements() * sizeof(float);
            const std::size_t working_set_bytes = metal.peak_working_set_bytes();
            const std::size_t unique_key_count = paired.cold_batch.unique_key_count;
            const std::size_t resident_entry_count = paired.setup_published_plan_count;
            const bool assertions_pass = paired.cold_batch.within_tolerance
                && paired.warm_cache.within_tolerance
                && paired.cold_batch.valley_distance.maximum <= 1.0
                && paired.warm_cache.valley_distance.maximum <= 1.0
                && unique_key_count == config.candidates
                && paired.warm_cache.unique_key_count == unique_key_count
                && paired.cold_batch.physical_build_count == unique_key_count
                && paired.cold_batch.ready_hit_count == 0U
                && paired.cold_batch.published_plan_count == resident_entry_count
                && paired.cold_batch.resident_entry_count == resident_entry_count
                && paired.warm_cache.physical_build_count
                       == unique_key_count - resident_entry_count
                && paired.warm_cache.ready_hit_count == resident_entry_count
                && paired.warm_cache.published_plan_count == 0U
                && paired.warm_cache.resident_entry_count == resident_entry_count
                && paired.setup_physical_build_count == unique_key_count
                && paired.cold_batch.resident_bytes == paired.retained_plan_bytes
                && paired.warm_cache.resident_bytes == paired.retained_plan_bytes
                && paired.retained_plan_bytes
                       <= paired.cache_limits.maximum_resident_bytes
                && resident_entry_count <= paired.cache_limits.maximum_entries
                && workspace_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL
                && working_set_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL;
            const std::string status = cache_baseline_status(
                config, paired, assertions_pass);
            const bool measurement_valid = config.samples != 21U || status == "MEASURED";
            const auto video_1000 = amortized_video_improvement(paired, 1000U);
            std::cout << std::fixed << std::setprecision(3)
                      << "device=" << metal.device_info().name << '\n'
                      << "planner_mode=session_cache_compare\n"
                      << "pairs=" << config.samples << '\n'
                      << "cold_session_plan_ms=" << paired.cold_batch.plan_ms.median << '\n'
                      << "warm_session_plan_ms=" << paired.warm_cache.plan_ms.median << '\n'
                      << "plan_speedup=" << paired.plan_speedup.median << "x\n"
                      << "plan_delta_mad=" << paired.plan_delta.mad << '\n'
                      << "cold_session_cpu_ms=" << paired.cold_batch.cpu_ms.median << '\n'
                      << "warm_session_cpu_ms=" << paired.warm_cache.cpu_ms.median << '\n'
                      << "cpu_execution_delta=" << paired.cpu_delta.median << '\n'
                      << "observed_steady_state_cpu_total_improvement="
                      << -paired.cpu_total_delta.median << '\n'
                      << "controlled_steady_state_cpu_total_improvement="
                      << -paired.controlled_cpu_total_delta.median << '\n'
                      << "cold_session_metal_ms=" << paired.cold_batch.metal_ms.median << '\n'
                      << "warm_session_metal_ms=" << paired.warm_cache.metal_ms.median << '\n'
                      << "metal_execution_delta=" << paired.metal_delta.median << '\n'
                      << "cold_session_metal_total_ms="
                      << paired.cold_batch.metal_total_ms.median << '\n'
                      << "warm_session_metal_total_ms="
                      << paired.warm_cache.metal_total_ms.median << '\n'
                      << "observed_steady_state_metal_total_improvement="
                      << -paired.metal_total_delta.median << '\n'
                      << "controlled_steady_state_metal_total_improvement="
                      << -paired.controlled_metal_total_delta.median << '\n'
                      << "video_1000_frame_total_improvement=" << video_1000.median << '\n'
                      << "observed_metal_total_delta_mad="
                      << paired.metal_total_delta.mad << '\n'
                      << "controlled_metal_total_delta_mad="
                      << paired.controlled_metal_total_delta.mad << '\n'
                      << "retained_cache_entries=" << resident_entry_count << '\n'
                      << "warm_ready_hit_count=" << paired.warm_cache.ready_hit_count << '\n'
                      << "warm_rebuilt_miss_count="
                      << paired.warm_cache.physical_build_count << '\n'
                      << "warm_ready_hit_rate=" << std::setprecision(6)
                      << static_cast<double>(paired.warm_cache.ready_hit_count)
                             / static_cast<double>(unique_key_count) << '\n'
                      << "retained_plan_mib="
                      << static_cast<double>(paired.retained_plan_bytes)
                              / (1024.0 * 1024.0) << '\n'
                      << "setup_only_cold_publish_ms="
                      << paired.setup_only_cold_publish_ms.median << '\n'
                      << "cache_baseline_status=" << status << '\n'
                      << "cache_baseline_decision="
                      << cache_baseline_decision(config, paired, assertions_pass) << '\n'
                      << "session_cache_decision="
                      << session_cache_decision(config, paired, assertions_pass) << '\n'
                      << "maximum_metric_error=" << std::setprecision(10)
                      << std::max(paired.cold_batch.maximum_error.maximum,
                                  paired.warm_cache.maximum_error.maximum) << '\n'
                      << "valley_step_distance=" << std::setprecision(0)
                      << std::max(paired.cold_batch.valley_distance.maximum,
                                  paired.warm_cache.valley_distance.maximum) << '\n'
                      << "metal_peak_workspace_mib=" << std::setprecision(3)
                      << static_cast<double>(workspace_bytes) / (1024.0 * 1024.0) << '\n'
                      << "metal_peak_working_set_mib="
                      << static_cast<double>(working_set_bytes) / (1024.0 * 1024.0) << '\n';
            if (config.json_output) {
                getnative::benchmark::atomic_write_json(
                    *config.json_output,
                    make_cache_compare_json(
                        config, paired, metal, workspace_bytes,
                        working_set_bytes, assertions_pass, argc, argv));
            }
            if (config.assert_correctness && (!assertions_pass || !measurement_valid)) {
                std::cerr << "Metal session-cache assertion or noise gate failed\n";
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        if (config.compare_planner_modes) {
            const PairedMeasurements paired = measure_pairs(config, fixture, metal, metric);
            const std::size_t workspace_bytes = metal.peak_workspace_elements() * sizeof(float);
            const std::size_t working_set_bytes = metal.peak_working_set_bytes();
            const bool assertions_pass = paired.serial.within_tolerance
                && paired.batch.within_tolerance
                && paired.serial.valley_distance.maximum <= 1.0
                && paired.batch.valley_distance.maximum <= 1.0
                && workspace_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL
                && working_set_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL;
            std::cout << std::fixed << std::setprecision(3)
                      << "device=" << metal.device_info().name << '\n'
                      << "planner_mode=compare\n"
                      << "pairs=" << config.samples << '\n'
                      << "serial_plan_ms=" << paired.serial.plan_ms.median << '\n'
                      << "batch_plan_ms=" << paired.batch.plan_ms.median << '\n'
                      << "plan_speedup=" << paired.plan_speedup.median << "x\n"
                      << "plan_delta_mad=" << paired.plan_delta.mad << '\n'
                      << "serial_metal_ms=" << paired.serial.metal_ms.median << '\n'
                      << "batch_metal_ms=" << paired.batch.metal_ms.median << '\n'
                      << "metal_execution_delta=" << paired.metal_delta.median << '\n'
                      << "serial_metal_total_ms=" << paired.serial.metal_total_ms.median << '\n'
                      << "batch_metal_total_ms=" << paired.batch.metal_total_ms.median << '\n'
                      << "metal_total_improvement=" << -paired.metal_total_delta.median << '\n'
                      << "metal_total_delta_mad=" << paired.metal_total_delta.mad << '\n'
                      << "stage1_measurement_status="
                      << stage1_measurement_status(config, paired, assertions_pass) << '\n'
                      << "maximum_metric_error=" << std::setprecision(10)
                      << std::max(paired.serial.maximum_error.maximum,
                                  paired.batch.maximum_error.maximum) << '\n'
                      << "valley_step_distance=" << std::setprecision(0)
                      << std::max(paired.serial.valley_distance.maximum,
                                  paired.batch.valley_distance.maximum) << '\n'
                      << "metal_peak_workspace_mib=" << std::setprecision(3)
                      << static_cast<double>(workspace_bytes) / (1024.0 * 1024.0) << '\n'
                      << "metal_peak_working_set_mib="
                      << static_cast<double>(working_set_bytes) / (1024.0 * 1024.0) << '\n';
            if (config.json_output) {
                getnative::benchmark::atomic_write_json(
                    *config.json_output,
                    make_compare_json(config, paired, metal, workspace_bytes,
                                      working_set_bytes, assertions_pass, argc, argv));
            }
            if (config.assert_correctness && !assertions_pass) {
                std::cerr << "Metal paired benchmark correctness or memory assertion failed\n";
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        const Measurements measurements = measure(config, fixture, metal, metric, false);
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

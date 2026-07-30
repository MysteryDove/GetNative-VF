#include "benchmark_support.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"

#include "axis_planner.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Configuration {
    std::size_t samples = 1;
    bool assert_speedup = false;
    bool compare_planner_modes = false;
    std::optional<std::filesystem::path> json_output;
};

struct DenseReference {
    std::vector<double> factor;
    double checksum = 0.0;
};

struct RuntimeFixture {
    static constexpr std::int32_t width = 640;
    static constexpr std::int32_t height = 360;
    static constexpr std::int32_t native_width = 426;
    static constexpr std::int32_t native_height = 240;
    static constexpr std::size_t candidate_count = 32;

    std::vector<float> source;
    std::vector<std::string> candidate_ids;
    getnative::AxisPlanRequest horizontal_request{
        width, native_width, static_cast<double>(native_width), 0.0,
        getnative::Filter::bicubic(), getnative::BorderMode::mirror,
    };
    getnative::AxisPlanRequest vertical_request{
        height, native_height, static_cast<double>(native_height), 0.0,
        getnative::Filter::bicubic(), getnative::BorderMode::mirror,
    };
    getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};

    RuntimeFixture()
        : source(static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<float>(
                (index * 131U + (index >> 3U) * 17U) % 1024U) / 1023.0F;
        }
        candidate_ids.reserve(candidate_count);
        for (std::size_t index = 0; index < candidate_count; ++index) {
            candidate_ids.push_back(std::to_string(index));
        }
    }

    [[nodiscard]] getnative::ConstImageView view() const noexcept {
        return {source.data(), width, height, width};
    }
};

struct CandidateReference {
    double candidate_ns = 0.0;
    std::size_t workspace_elements = 0;
    double checksum = 0.0;
};

struct RuntimeSample {
    double plan_ms = 0.0;
    double cpu_ms = 0.0;
    double cpu_total_ms = 0.0;
    double checksum = 0.0;
    std::size_t cache_size = 0;
};

struct RuntimeMeasurements {
    getnative::benchmark::Summary plan_ms;
    getnative::benchmark::Summary cpu_ms;
    getnative::benchmark::Summary cpu_total_ms;
    double checksum = 0.0;
    std::size_t cache_size = 0;
};

struct PlanTiming {
    double plan_ms = 0.0;
    std::uint64_t checksum = 0U;
    std::size_t unique_key_count = 0U;
    std::size_t physical_build_count = 0U;
    std::size_t peak_active_builds = 0U;
    std::size_t effective_worker_count = 0U;
};

struct PairedPlanMeasurements {
    std::string case_name;
    std::size_t request_count = 0U;
    std::size_t unique_key_count = 0U;
    std::size_t requested_worker_count = 0U;
    std::size_t effective_worker_count = 0U;
    std::size_t peak_active_builds = 0U;
    std::vector<bool> serial_first;
    getnative::benchmark::Summary serial_plan_ms;
    getnative::benchmark::Summary batch_plan_ms;
    getnative::benchmark::Summary delta;
    getnative::benchmark::Summary speedup;
    getnative::benchmark::Summary overhead_us;
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
            result.assert_speedup = true;
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
        } else if (argument == "--json-out" && index + 1 < argc) {
            result.json_output = std::filesystem::path{argv[++index]};
        } else {
            throw std::invalid_argument(
                "usage: getnative_core_benchmark [--assert] [--samples N] "
                "[--planner-mode serial | --compare-planner-modes] [--json-out PATH]");
        }
    }
    if (planner_mode_explicit && result.compare_planner_modes) {
        throw std::invalid_argument(
            "--planner-mode and --compare-planner-modes are mutually exclusive");
    }
    return result;
}

[[nodiscard]] DenseReference build_dense_reference(
    const getnative::AxisPlanRequest &request) {
    const auto plan = getnative::build_axis_plan(request);
    const std::size_t n = static_cast<std::size_t>(request.destination_size);
    std::vector<double> normal(n * n);
    for (std::int32_t row = 0; row < request.source_size; ++row) {
        const auto begin = plan.forward_offsets[static_cast<std::size_t>(row)];
        const auto end = plan.forward_offsets[static_cast<std::size_t>(row) + 1U];
        for (std::uint32_t p = begin; p < end; ++p) {
            const std::size_t i = static_cast<std::size_t>(plan.forward_indices[p]);
            for (std::uint32_t q = begin; q < end; ++q) {
                const std::size_t j = static_cast<std::size_t>(plan.forward_indices[q]);
                normal[i * n + j] += static_cast<double>(plan.forward_weights[p])
                    * static_cast<double>(plan.forward_weights[q]);
            }
        }
    }
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1U; j < n; ++j) {
            double value = normal[j * n + i];
            for (std::size_t k = 0; k < i; ++k) {
                value -= normal[j * n + k] * normal[i * n + k] * normal[k * n + k];
            }
            normal[j * n + i] = value / (normal[i * n + i] + epsilon);
        }
        double diagonal = normal[i * n + i];
        for (std::size_t k = 0; k < i; ++k) {
            diagonal -= normal[i * n + k] * normal[i * n + k] * normal[k * n + k];
        }
        normal[i * n + i] = diagonal;
    }
    double checksum = 0.0;
    for (std::size_t i = 0; i < n; ++i) checksum += normal[i * n + i];
    return {std::move(normal), checksum};
}

template <class Function>
[[nodiscard]] double calibrated_nanoseconds(Function &&function) {
    std::size_t repetitions = 1;
    constexpr auto target = std::chrono::milliseconds(120);
    for (;;) {
        const auto start = Clock::now();
        double checksum = 0.0;
        for (std::size_t index = 0; index < repetitions; ++index) checksum += function();
        const auto elapsed = Clock::now() - start;
        if (!std::isfinite(checksum)) throw std::runtime_error("benchmark checksum is nonfinite");
        if (elapsed >= target || repetitions >= 4096U) {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())
                / static_cast<double>(repetitions);
        }
        repetitions *= 2U;
    }
}

[[nodiscard]] CandidateReference benchmark_candidate(const RuntimeFixture &fixture) {
    const auto horizontal = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan(fixture.horizontal_request));
    const auto vertical = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan(fixture.vertical_request));
    getnative::CpuWorkspace workspace;
    const double warmup = getnative::analyze_candidate_f32(
        fixture.view(), *horizontal, *vertical, fixture.metric, workspace);

    std::vector<double> samples;
    for (int sample = 0; sample < 7; ++sample) {
        const auto start = Clock::now();
        const double value = getnative::analyze_candidate_f32(
            fixture.view(), *horizontal, *vertical, fixture.metric, workspace);
        const auto elapsed = Clock::now() - start;
        if (value != warmup) throw std::runtime_error("candidate benchmark is not deterministic");
        samples.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
    }
    return {
        getnative::benchmark::median(std::move(samples)),
        workspace.peak_elements(),
        warmup,
    };
}

[[nodiscard]] RuntimeSample run_runtime_sample(const RuntimeFixture &fixture) {
    getnative::AxisPlanCache cache;
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(fixture.candidate_ids.size());

    const auto plan_start = Clock::now();
    for (const auto &id : fixture.candidate_ids) {
        candidates.push_back({
            id,
            cache.get_or_build(fixture.horizontal_request),
            cache.get_or_build(fixture.vertical_request),
            getnative::AnalysisAxes::both,
        });
    }
    const auto plan_elapsed = Clock::now() - plan_start;

    const auto cpu_start = Clock::now();
    const auto results = getnative::analyze_batch_f32(
        fixture.view(), candidates, fixture.metric);
    const auto cpu_elapsed = Clock::now() - cpu_start;

    double checksum = 0.0;
    for (const auto &result : results) checksum += result.error;
    const double plan_ms = std::chrono::duration<double, std::milli>(plan_elapsed).count();
    const double cpu_ms = std::chrono::duration<double, std::milli>(cpu_elapsed).count();
    return {plan_ms, cpu_ms, plan_ms + cpu_ms, checksum, cache.size()};
}

[[nodiscard]] RuntimeMeasurements benchmark_runtime(
    const RuntimeFixture &fixture,
    std::size_t sample_count) {
    const RuntimeSample warmup = run_runtime_sample(fixture);
    std::vector<double> plan_samples;
    std::vector<double> cpu_samples;
    std::vector<double> total_samples;
    plan_samples.reserve(sample_count);
    cpu_samples.reserve(sample_count);
    total_samples.reserve(sample_count);

    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        const RuntimeSample current = run_runtime_sample(fixture);
        if (current.checksum != warmup.checksum || current.cache_size != 2U) {
            throw std::runtime_error("core staged benchmark changed result or cache cardinality");
        }
        plan_samples.push_back(current.plan_ms);
        cpu_samples.push_back(current.cpu_ms);
        total_samples.push_back(current.cpu_total_ms);
    }
    return {
        getnative::benchmark::summarize(std::move(plan_samples)),
        getnative::benchmark::summarize(std::move(cpu_samples)),
        getnative::benchmark::summarize(std::move(total_samples)),
        warmup.checksum,
        warmup.cache_size,
    };
}

[[nodiscard]] std::vector<getnative::AxisPlanRequest> make_plan_requests(
    std::size_t request_count,
    std::size_t unique_count) {
    std::vector<getnative::AxisPlanRequest> unique_requests;
    unique_requests.reserve(unique_count);
    for (std::size_t index = 0; index < unique_count; ++index) {
        const double active = 720.0
            + static_cast<double>(index) / static_cast<double>(unique_count + 1U);
        unique_requests.push_back({
            1080, 720, active, 0.0, getnative::Filter::bicubic(),
            getnative::BorderMode::mirror,
        });
    }
    std::vector<getnative::AxisPlanRequest> requests;
    requests.reserve(request_count);
    for (std::size_t index = 0; index < request_count; ++index) {
        requests.push_back(unique_requests[index % unique_count]);
    }
    return requests;
}

[[nodiscard]] PlanTiming run_plan_sample(
    const std::vector<getnative::AxisPlanRequest> &requests,
    bool batch,
    std::size_t worker_count) {
    std::vector<std::shared_ptr<const getnative::AxisPlan>> plans;
    std::size_t unique_key_count = 0U;
    std::size_t physical_build_count = 0U;
    std::size_t peak_active_builds = 0U;
    std::size_t effective_worker_count = 1U;
    double plan_ms = 0.0;

    if (batch) {
        const auto start = Clock::now();
        auto result = getnative::detail::build_axis_plans(
            requests, {worker_count, {}, {}});
        plans = std::move(result.plans);
        plan_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
        unique_key_count = result.unique_key_count;
        physical_build_count = result.physical_build_count;
        peak_active_builds = result.peak_active_builds;
        effective_worker_count = result.effective_worker_count;
    } else {
        plans.reserve(requests.size());
        getnative::AxisPlanCache cache;
        const auto start = Clock::now();
        for (const auto &request : requests) plans.push_back(cache.get_or_build(request));
        plan_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
        unique_key_count = cache.size();
        physical_build_count = cache.size();
        peak_active_builds = requests.empty() ? 0U : 1U;
        effective_worker_count = requests.empty() ? 0U : 1U;
    }

    std::unordered_set<const getnative::AxisPlan *> distinct_plans;
    std::uint64_t checksum = 0U;
    for (const auto &plan : plans) {
        if (!plan || !plan->valid()) {
            throw std::runtime_error("paired planner produced an invalid plan");
        }
        distinct_plans.insert(plan.get());
        checksum += static_cast<std::uint64_t>(plan->forward_weights.size());
        checksum += static_cast<std::uint64_t>(plan->packed_factor_elements());
        checksum ^= std::bit_cast<std::uint64_t>(plan->active_length);
    }
    if (plans.size() != requests.size() || distinct_plans.size() != unique_key_count
        || physical_build_count != unique_key_count
        || peak_active_builds > effective_worker_count) {
        throw std::runtime_error("paired planner count or worker invariant changed");
    }
    return {
        plan_ms, checksum, unique_key_count, physical_build_count,
        peak_active_builds, effective_worker_count,
    };
}

[[nodiscard]] PairedPlanMeasurements measure_plan_pairs(
    std::string case_name,
    const std::vector<getnative::AxisPlanRequest> &requests,
    std::size_t expected_unique_count,
    std::size_t worker_count,
    std::size_t sample_count) {
    const PlanTiming serial_warmup = run_plan_sample(requests, false, 1U);
    const PlanTiming batch_warmup = run_plan_sample(requests, true, worker_count);
    if (serial_warmup.checksum != batch_warmup.checksum
        || serial_warmup.unique_key_count != expected_unique_count
        || batch_warmup.unique_key_count != expected_unique_count) {
        throw std::runtime_error("paired planner warmup changed plan identity or key count");
    }

    std::vector<bool> serial_first;
    std::vector<double> serial_samples;
    std::vector<double> batch_samples;
    std::vector<double> deltas;
    std::vector<double> speedups;
    std::vector<double> overheads;
    serial_first.reserve(sample_count);
    serial_samples.reserve(sample_count);
    batch_samples.reserve(sample_count);
    deltas.reserve(sample_count);
    speedups.reserve(sample_count);
    overheads.reserve(sample_count);
    std::size_t peak_active_builds = 0U;
    const std::size_t effective_worker_count = batch_warmup.effective_worker_count;

    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        const bool run_serial_first = (sample & 1U) == 0U;
        PlanTiming serial;
        PlanTiming batch;
        if (run_serial_first) {
            serial = run_plan_sample(requests, false, 1U);
            batch = run_plan_sample(requests, true, worker_count);
        } else {
            batch = run_plan_sample(requests, true, worker_count);
            serial = run_plan_sample(requests, false, 1U);
        }
        if (serial.checksum != batch.checksum
            || serial.unique_key_count != expected_unique_count
            || batch.unique_key_count != expected_unique_count
            || batch.physical_build_count != expected_unique_count
            || batch.effective_worker_count != effective_worker_count) {
            throw std::runtime_error("paired planner sample changed identity or counts");
        }
        serial_first.push_back(run_serial_first);
        serial_samples.push_back(serial.plan_ms);
        batch_samples.push_back(batch.plan_ms);
        deltas.push_back((batch.plan_ms - serial.plan_ms) / serial.plan_ms);
        speedups.push_back(serial.plan_ms / batch.plan_ms);
        overheads.push_back((batch.plan_ms - serial.plan_ms) * 1000.0);
        peak_active_builds = std::max(peak_active_builds, batch.peak_active_builds);
    }

    return {
        std::move(case_name), requests.size(), expected_unique_count, worker_count,
        effective_worker_count, peak_active_builds, std::move(serial_first),
        getnative::benchmark::summarize(std::move(serial_samples)),
        getnative::benchmark::summarize(std::move(batch_samples)),
        getnative::benchmark::summarize(std::move(deltas)),
        getnative::benchmark::summarize(std::move(speedups)),
        getnative::benchmark::summarize(std::move(overheads)),
    };
}

[[nodiscard]] std::vector<PairedPlanMeasurements> benchmark_plan_pairs(
    std::size_t sample_count) {
    struct Case {
        std::string_view name;
        std::size_t request_count;
        std::size_t unique_count;
    };
    constexpr std::array cases{
        Case{"one-unique", 1U, 1U},
        Case{"two-unique", 2U, 2U},
        Case{"thirty-two-unique", 32U, 32U},
        Case{"thousand-unique", 1000U, 1000U},
        Case{"thousand-identical", 1000U, 1U},
    };
    constexpr std::array<std::size_t, 5> worker_counts{1U, 2U, 4U, 8U, 0U};
    std::vector<PairedPlanMeasurements> results;
    results.reserve(cases.size() * worker_counts.size());
    for (const Case &benchmark_case : cases) {
        const auto requests = make_plan_requests(
            benchmark_case.request_count, benchmark_case.unique_count);
        for (const std::size_t worker_count : worker_counts) {
            results.push_back(measure_plan_pairs(
                std::string{benchmark_case.name}, requests, benchmark_case.unique_count,
                worker_count, sample_count));
        }
    }
    return results;
}

void append_pair_order(std::ostream &output, const std::vector<bool> &serial_first) {
    output << '[';
    for (std::size_t index = 0; index < serial_first.size(); ++index) {
        if (index != 0U) output << ',';
        output << (serial_first[index]
            ? "\"serial_then_batch\"" : "\"batch_then_serial\"");
    }
    output << ']';
}

[[nodiscard]] std::string core_measurement_status(
    const Configuration &config,
    const std::vector<PairedPlanMeasurements> &measurements) {
    if (config.samples != 21U) return "TUNING_ONLY";
    const auto decision_case = std::find_if(
        measurements.begin(), measurements.end(), [](const auto &current) {
            return current.case_name == "thousand-unique"
                && current.requested_worker_count == 0U;
        });
    if (decision_case == measurements.end()) return "STAGE1_BLOCKED";
    return decision_case->delta.mad > 0.025 ? "NO_DECISION_NOISY" : "MEASURED";
}

[[nodiscard]] std::string make_compare_json(
    const Configuration &config,
    const std::vector<PairedPlanMeasurements> &measurements,
    int argc,
    char **argv) {
    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_core_benchmark", "compare",
        "synthetic-plan-paired-cases-v1", argc, argv);
    output << ",\"sample_count\":" << config.samples
           << ",\"warmup_count_per_mode\":1"
           << ",\"stage1_measurement_status\":"
           << getnative::benchmark::json_string(
                  core_measurement_status(config, measurements))
           << ",\"cases\":[";
    for (std::size_t index = 0; index < measurements.size(); ++index) {
        if (index != 0U) output << ',';
        const PairedPlanMeasurements &current = measurements[index];
        output << "{\"name\":" << getnative::benchmark::json_string(current.case_name)
               << ",\"request_count\":" << current.request_count
               << ",\"unique_key_count\":" << current.unique_key_count
               << ",\"requested_worker_count\":" << current.requested_worker_count
               << ",\"worker_mode\":"
               << getnative::benchmark::json_string(
                      current.requested_worker_count == 0U ? "auto" : "explicit")
               << ",\"effective_worker_count\":" << current.effective_worker_count
               << ",\"peak_active_builds\":" << current.peak_active_builds
               << ",\"pair_order\":";
        append_pair_order(output, current.serial_first);
        output << ",\"serial_plan_ms\":";
        getnative::benchmark::append_summary(output, current.serial_plan_ms);
        output << ",\"batch_plan_ms\":";
        getnative::benchmark::append_summary(output, current.batch_plan_ms);
        output << ",\"delta\":";
        getnative::benchmark::append_summary(output, current.delta);
        output << ",\"speedup\":";
        getnative::benchmark::append_summary(output, current.speedup);
        output << ",\"overhead_us\":";
        getnative::benchmark::append_summary(output, current.overhead_us);
        output << '}';
    }
    output << "],\"correctness\":{\"assertions\":true}}\n";
    return output.str();
}

[[nodiscard]] std::string make_json(
    const Configuration &config,
    int argc,
    char **argv,
    double banded_ns,
    double dense_ns,
    double planner_speedup,
    const CandidateReference &candidate,
    const RuntimeMeasurements &runtime,
    bool assertions_pass) {
    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_core_benchmark", "serial",
        "synthetic-core-640x360-bicubic-batch32-v1", argc, argv);
    output << ",\"sample_count\":" << config.samples
           << ",\"warmup_count\":1"
           << ",\"stage0_outcome\":\"NOT_APPLICABLE\""
           << ",\"anchors\":{\"banded_us\":" << std::setprecision(17)
           << banded_ns / 1000.0
           << ",\"dense_us\":" << dense_ns / 1000.0
           << ",\"planner_speedup\":" << planner_speedup
           << ",\"candidate_640x360_us\":" << candidate.candidate_ns / 1000.0
           << '}'
           << ",\"metrics\":{\"plan_ms\":";
    getnative::benchmark::append_summary(output, runtime.plan_ms);
    output << ",\"cpu_ms\":";
    getnative::benchmark::append_summary(output, runtime.cpu_ms);
    output << ",\"cpu_total_ms\":";
    getnative::benchmark::append_summary(output, runtime.cpu_total_ms);
    output << "}"
           << ",\"correctness\":{\"cache_size\":" << runtime.cache_size
           << ",\"workspace_elements\":" << candidate.workspace_elements
           << ",\"metric_checksum\":" << std::setprecision(17) << candidate.checksum
           << ",\"batch_checksum\":" << runtime.checksum
           << ",\"assertions\":" << (assertions_pass ? "true" : "false") << "}"
           << "}\n";
    return output.str();
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Configuration config = parse_arguments(argc, argv);
        if (config.json_output) {
            getnative::benchmark::validate_json_output_path(*config.json_output);
        }

        if (config.compare_planner_modes) {
            const auto measurements = benchmark_plan_pairs(config.samples);
            for (const PairedPlanMeasurements &current : measurements) {
                std::cout << "case=" << current.case_name
                          << " workers="
                          << (current.requested_worker_count == 0U
                                  ? std::string{"auto"}
                                  : std::to_string(current.requested_worker_count))
                          << " effective=" << current.effective_worker_count
                          << " serial_ms=" << current.serial_plan_ms.median
                          << " batch_ms=" << current.batch_plan_ms.median
                          << " speedup=" << current.speedup.median
                          << " delta_mad=" << current.delta.mad << '\n';
            }
            if (config.json_output) {
                getnative::benchmark::atomic_write_json(
                    *config.json_output,
                    make_compare_json(config, measurements, argc, argv));
            }
            return EXIT_SUCCESS;
        }

        const getnative::AxisPlanRequest request{
            854, 600, 600.0, 0.0, getnative::Filter::bicubic(),
            getnative::BorderMode::mirror,
        };
        (void)getnative::build_axis_plan(request);
        (void)build_dense_reference(request);
        std::vector<double> banded_samples;
        std::vector<double> dense_samples;
        for (int sample = 0; sample < 5; ++sample) {
            banded_samples.push_back(calibrated_nanoseconds([&] {
                const auto plan = getnative::build_axis_plan(request);
                return static_cast<double>(plan.packed_factor_elements())
                    + static_cast<double>(plan.forward_weights.size());
            }));
            dense_samples.push_back(calibrated_nanoseconds([&] {
                return build_dense_reference(request).checksum;
            }));
        }
        const double banded_ns = getnative::benchmark::median(std::move(banded_samples));
        const double dense_ns = getnative::benchmark::median(std::move(dense_samples));
        const double speedup = dense_ns / banded_ns;

        const RuntimeFixture fixture;
        const CandidateReference candidate = benchmark_candidate(fixture);
        const RuntimeMeasurements runtime = benchmark_runtime(fixture, config.samples);
        constexpr std::size_t expected_workspace = 640U * 240U + 426U * 240U + 640U;
        const bool assertions_pass = speedup >= 2.0 && std::isfinite(speedup)
            && candidate.workspace_elements == expected_workspace;

        std::cout << std::fixed << std::setprecision(3)
                  << "case=bicubic source=854 destination=600 active=600 shift=0 border=mirror\n"
                  << "banded_us=" << banded_ns / 1000.0 << '\n'
                  << "dense_us=" << dense_ns / 1000.0 << '\n'
                  << "planner_speedup=" << speedup << "x\n"
                  << "candidate_640x360_us=" << candidate.candidate_ns / 1000.0 << '\n'
                  << "batch32_ms=" << runtime.cpu_ms.median << '\n'
                  << "batch_candidates_per_s="
                  << static_cast<double>(RuntimeFixture::candidate_count) * 1000.0
                      / runtime.cpu_ms.median
                  << '\n'
                  << "workspace_elements=" << candidate.workspace_elements << '\n'
                  << "metric_checksum=" << candidate.checksum << '\n'
                  << "planner_mode=serial\n"
                  << "samples=" << config.samples << '\n'
                  << "plan_ms=" << runtime.plan_ms.median << '\n'
                  << "plan_mad_ms=" << runtime.plan_ms.mad << '\n'
                  << "cpu_total_ms=" << runtime.cpu_total_ms.median << '\n'
                  << "cpu_total_mad_ms=" << runtime.cpu_total_ms.mad << '\n';

        if (config.json_output) {
            getnative::benchmark::atomic_write_json(
                *config.json_output,
                make_json(config, argc, argv, banded_ns, dense_ns, speedup,
                          candidate, runtime, assertions_pass));
        }
        if (config.assert_speedup && !assertions_pass) {
            std::cerr << "benchmark assertion failed: planner speedup or workspace invariant\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

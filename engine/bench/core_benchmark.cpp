#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct DenseReference {
    std::vector<double> factor;
    double checksum = 0.0;
};

struct RuntimeReference {
    double candidate_ns = 0.0;
    double batch_ns = 0.0;
    std::size_t workspace_elements = 0;
    double checksum = 0.0;
};

[[nodiscard]] DenseReference build_dense_reference(const getnative::AxisPlanRequest &request) {
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
        for (std::size_t i = 0; i < repetitions; ++i) checksum += function();
        const auto elapsed = Clock::now() - start;
        if (!std::isfinite(checksum)) throw std::runtime_error("benchmark checksum is nonfinite");
        if (elapsed >= target || repetitions >= 4096U) {
            return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())
                / static_cast<double>(repetitions);
        }
        repetitions *= 2U;
    }
}

[[nodiscard]] double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

[[nodiscard]] RuntimeReference benchmark_runtime() {
    constexpr std::int32_t width = 640;
    constexpr std::int32_t height = 360;
    constexpr std::int32_t native_width = 426;
    constexpr std::int32_t native_height = 240;
    std::vector<float> source(static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<float>((i * 131U + (i >> 3U) * 17U) % 1024U) / 1023.0F;
    }
    const auto horizontal = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({width, native_width, static_cast<double>(native_width),
                                    0.0, getnative::Filter::bicubic(),
                                    getnative::BorderMode::mirror}));
    const auto vertical = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({height, native_height, static_cast<double>(native_height),
                                    0.0, getnative::Filter::bicubic(),
                                    getnative::BorderMode::mirror}));
    const getnative::ConstImageView view{source.data(), width, height, width};
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::CpuWorkspace workspace;
    const double warmup = getnative::analyze_candidate_f32(
        view, *horizontal, *vertical, metric, workspace);

    std::vector<double> candidate_samples;
    for (int sample = 0; sample < 7; ++sample) {
        const auto start = Clock::now();
        const double value = getnative::analyze_candidate_f32(
            view, *horizontal, *vertical, metric, workspace);
        const auto elapsed = Clock::now() - start;
        if (value != warmup) throw std::runtime_error("candidate benchmark is not deterministic");
        candidate_samples.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
    }

    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(32U);
    for (std::size_t i = 0; i < 32U; ++i) {
        candidates.push_back({std::to_string(i), horizontal, vertical,
                              getnative::AnalysisAxes::both});
    }
    const auto batch_start = Clock::now();
    const auto results = getnative::analyze_batch_f32(view, candidates, metric);
    const auto batch_elapsed = Clock::now() - batch_start;
    for (const auto &result : results) {
        if (result.error != warmup) throw std::runtime_error("batch benchmark changed the metric");
    }
    return {
        median(std::move(candidate_samples)),
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            batch_elapsed).count()),
        workspace.peak_elements(),
        warmup,
    };
}

} // namespace

int main(int argc, char **argv) {
    bool assert_speedup = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--assert") {
            assert_speedup = true;
        } else {
            std::cerr << "usage: getnative_core_benchmark [--assert]\n";
            return EXIT_FAILURE;
        }
    }

    // Representative 480p-width bicubic axis. Dense storage is 2.8 MiB; the
    // banded planner stores seven factor bands and performs bounded-band work.
    const getnative::AxisPlanRequest request{854, 600, 600.0, 0.0,
                                             getnative::Filter::bicubic(),
                                             getnative::BorderMode::mirror};
    try {
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
        const double banded_ns = median(std::move(banded_samples));
        const double dense_ns = median(std::move(dense_samples));
        const double speedup = dense_ns / banded_ns;
        const RuntimeReference runtime = benchmark_runtime();
        constexpr std::size_t expected_workspace = 640U * 240U + 426U * 240U + 640U;
        std::cout << std::fixed << std::setprecision(3)
                  << "case=bicubic source=854 destination=600 active=600 shift=0 border=mirror\n"
                  << "banded_us=" << banded_ns / 1000.0 << '\n'
                  << "dense_us=" << dense_ns / 1000.0 << '\n'
                  << "planner_speedup=" << speedup << "x\n"
                  << "candidate_640x360_us=" << runtime.candidate_ns / 1000.0 << '\n'
                  << "batch32_ms=" << runtime.batch_ns / 1'000'000.0 << '\n'
                  << "batch_candidates_per_s=" << 32'000'000'000.0 / runtime.batch_ns << '\n'
                  << "workspace_elements=" << runtime.workspace_elements << '\n'
                  << "metric_checksum=" << runtime.checksum << '\n';
        if (assert_speedup && (!(speedup >= 2.0) || !std::isfinite(speedup))) {
            std::cerr << "benchmark assertion failed: planner speedup must be at least 2.0x\n";
            return EXIT_FAILURE;
        }
        if (assert_speedup && runtime.workspace_elements != expected_workspace) {
            std::cerr << "benchmark assertion failed: workspace must reuse one intermediate buffer\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

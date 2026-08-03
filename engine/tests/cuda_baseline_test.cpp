#include "getnative/cuda_analysis.hpp"

#include "cuda_memory_policy.hpp"
#include "getnative/axis_plan.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <class Exception, class Function>
void expect_throws(Function &&function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

void test_memory_budget_policy() {
    constexpr std::size_t mib = 1024ULL * 1024ULL;
    const auto high_concurrency = getnative::cuda_detail::make_cuda_memory_budget(
        16ULL * 1024ULL * mib, 16U, 0U);
    expect(high_concurrency.reserve_bytes == 2ULL * 1024ULL * mib,
           "CUDA memory policy reserves one eighth of a large device");
    expect(high_concurrency.workspace_limit_bytes
               == getnative::cuda_detail::cuda_default_workspace_bytes,
           "CUDA memory policy preserves the 640 MiB fast path on 16 GiB");
    expect(!high_concurrency.workspace_limit_clamped,
           "CUDA memory policy does not report an unnecessary clamp");

    const auto constrained = getnative::cuda_detail::make_cuda_memory_budget(
        4ULL * 1024ULL * mib, 16U, 0U);
    expect(constrained.reserve_bytes == 512ULL * mib,
           "CUDA memory policy preserves the low-memory reserve floor");
    expect(constrained.per_slot_budget_bytes == 224ULL * mib
               && constrained.workspace_limit_bytes
                    == (constrained.per_slot_budget_bytes
                        - constrained.per_slot_budget_bytes / 5U)
                        / sizeof(float) * sizeof(float),
           "CUDA memory policy divides constrained memory across slots");
    expect(constrained.workspace_limit_clamped,
           "CUDA memory policy reports an automatic low-memory clamp");

    const auto explicit_limit = getnative::cuda_detail::make_cuda_memory_budget(
        4ULL * 1024ULL * mib, 2U, 128ULL * mib / sizeof(float));
    expect(explicit_limit.workspace_limit_bytes == 128ULL * mib
               && !explicit_limit.workspace_limit_clamped,
           "an explicit lower workspace limit remains authoritative");

    const auto raised_limit = getnative::cuda_detail::make_cuda_memory_budget(
        16ULL * 1024ULL * mib, 1U, 1024ULL * mib / sizeof(float));
    expect(raised_limit.workspace_limit_bytes
               == getnative::cuda_detail::cuda_default_workspace_bytes
               && raised_limit.workspace_limit_clamped,
           "an explicit limit cannot raise the 640 MiB workspace ceiling");

    const auto low_free_memory = getnative::cuda_detail::make_cuda_memory_budget(
        768ULL * mib, 2U, 0U);
    expect(low_free_memory.reserve_bytes == 384ULL * mib
               && low_free_memory.engine_budget_bytes == 384ULL * mib,
           "the reserve is capped at half of free memory without underflow");

    const auto oversized_limit = getnative::cuda_detail::make_cuda_memory_budget(
        16ULL * 1024ULL * mib, 1U,
        std::numeric_limits<std::size_t>::max());
    expect(oversized_limit.workspace_limit_clamped,
           "the explicit 2 GiB hard limit is reported as a clamp");
    expect(getnative::cuda_minimum_compute_capability() == 75
               && getnative::cuda_compiled_artifact_target().find("sm_75")
                    != std::string_view::npos
               && getnative::cuda_compiled_artifact_target().find("compute_75")
                    != std::string_view::npos,
           "compiled CUDA provenance exposes the SM75 compatibility floor");
}

void test_default_launch_policy_is_frozen() {
    constexpr getnative::CudaLaunchPolicy expected{
        64U, 256U, 128U, true,
    };
    constexpr getnative::CudaLaunchPolicy policy =
        getnative::cuda_default_launch_policy;
    expect(policy.inverse_threads == expected.inverse_threads
               && policy.pixel_threads == expected.pixel_threads
               && policy.maximum_metric_blocks == expected.maximum_metric_blocks
               && policy.paired_vertical == expected.paired_vertical,
           "CUDA production launch policy remains i64-p256-m128-vpair");

    const getnative::CudaAnalysisOptions options;
    expect(options.launch_policy.inverse_threads == 64U
               && options.launch_policy.pixel_threads == 256U
               && options.launch_policy.maximum_metric_blocks == 128U
               && options.launch_policy.paired_vertical,
           "default CUDA analysis options use the frozen launch policy");
}

[[nodiscard]] const getnative::CudaDeviceInfo &compatible_device(
    const getnative::CudaRuntimeProbe &probe) {
    const auto selected = std::find_if(
        probe.devices.begin(), probe.devices.end(),
        [](const getnative::CudaDeviceInfo &device) {
            return device.backend_compatible;
        });
    if (selected == probe.devices.end()) {
        throw std::runtime_error("CUDA probe exposed no compatible device");
    }
    return *selected;
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

struct SourceFixture {
    std::vector<float> storage;
    getnative::ConstImageView view;
};

[[nodiscard]] SourceFixture make_source(
    std::int32_t width, std::int32_t height, std::ptrdiff_t stride) {
    SourceFixture result;
    result.storage.assign(
        static_cast<std::size_t>(height) * static_cast<std::size_t>(stride), -17.0F);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            result.storage[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)
                           + static_cast<std::size_t>(x)] = static_cast<float>(
                0.41 + 0.21 * std::sin(0.19 * static_cast<double>(x))
                + 0.17 * std::cos(0.23 * static_cast<double>(y))
                + 0.09 * std::sin(0.07 * static_cast<double>(x + 3 * y)));
        }
    }
    result.view = {result.storage.data(), width, height, stride};
    return result;
}

[[nodiscard]] double cpu_metric(
    getnative::ConstImageView source,
    const getnative::CandidateAnalysis &candidate,
    const getnative::MetricSpec &metric) {
    getnative::CpuWorkspace workspace;
    if (candidate.axes == getnative::AnalysisAxes::both) {
        return getnative::analyze_candidate_f32(
            source, *candidate.horizontal, *candidate.vertical, metric, workspace);
    }
    const auto &plan = candidate.axes == getnative::AnalysisAxes::horizontal
        ? candidate.horizontal : candidate.vertical;
    return getnative::analyze_axis_candidate_f32(
        source, *plan, candidate.axes, metric, workspace);
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> make_candidates(
    std::int32_t width, std::int32_t height) {
    const auto horizontal_only = make_plan(
        width, 25, 25.25, -0.125, getnative::Filter::lanczos(3));
    const auto vertical_only = make_plan(
        height, 19, 19.125, 0.125, getnative::Filter::spline16());

    const auto horizontal_first_x = make_plan(
        width, 35, 35.0, 0.0, getnative::Filter::bicubic());
    const auto horizontal_first_y = make_plan(
        height, 13, 13.0, 0.0, getnative::Filter::bicubic());
    expect(getnative::select_forward_order(*horizontal_first_x, *horizontal_first_y)
               == getnative::ForwardOrder::horizontal_first,
           "fixture must exercise horizontal-first reconstruction");

    const auto vertical_first_x = make_plan(
        width, 17, 17.0, 0.0, getnative::Filter::bicubic());
    const auto vertical_first_y = make_plan(
        height, 27, 27.0, 0.0, getnative::Filter::bicubic());
    expect(getnative::select_forward_order(*vertical_first_x, *vertical_first_y)
               == getnative::ForwardOrder::vertical_first,
           "fixture must exercise vertical-first reconstruction");

    return {
        {"horizontal", horizontal_only, nullptr, getnative::AnalysisAxes::horizontal},
        {"vertical", nullptr, vertical_only, getnative::AnalysisAxes::vertical},
        {"both-horizontal-first", horizontal_first_x, horizontal_first_y,
         getnative::AnalysisAxes::both},
        {"both-vertical-first", vertical_first_x, vertical_first_y,
         getnative::AnalysisAxes::both},
    };
}

void test_generic_baseline(const getnative::CudaRuntimeProbe &probe) {
    constexpr std::int32_t width = 37;
    constexpr std::int32_t height = 29;
    const SourceFixture source = make_source(width, height, width + 5);
    const auto candidates = make_candidates(width, height);
    const getnative::MetricSpec metric{2, 3, 1, 2, 0.0F, 1U};

    std::vector<double> expected;
    expected.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        expected.push_back(cpu_metric(source.view, candidate, metric));
    }

    getnative::CudaAnalysisOptions options;
    options.device_ordinal = compatible_device(probe).ordinal;
    options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
    getnative::CudaAnalysisEngine engine(options);
    const auto actual = engine.analyze_axis_batch_f32(source.view, candidates, metric);
    expect(actual.size() == candidates.size(), "CUDA returns one result per candidate");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expect(actual[index].id == candidates[index].id,
               "CUDA preserves candidate order and ids");
        expect(std::isfinite(actual[index].error), "CUDA result must be finite");
        const double tolerance = std::max(2e-7, 5e-4 * std::abs(expected[index]));
        if (std::abs(actual[index].error - expected[index]) > tolerance) {
            throw std::runtime_error(
                "CUDA cpp-generic result exceeds the CPU tolerance for "
                + candidates[index].id + ": cuda=" + std::to_string(actual[index].error)
                + ", cpu=" + std::to_string(expected[index])
                + ", tolerance=" + std::to_string(tolerance));
        }
    }

    const auto telemetry = engine.runtime_telemetry();
    expect(telemetry.kernel_variant == "cpp-generic", "baseline variant provenance");
    expect(telemetry.artifact_stage == "staged-cpp", "staged stage provenance");
    expect(telemetry.artifact_target.find("native=[") == 0U
               && telemetry.artifact_target.find("sm_75") != std::string::npos
               && telemetry.artifact_target.find("ptx=[") != std::string::npos
               && telemetry.artifact_target.find("compute_75") != std::string::npos,
           "multi-architecture target provenance");
    expect(telemetry.artifact_name == "getnative_cuda_staged.fatbin",
           "staged artifact provenance");
    expect(telemetry.artifact_hash_fnv1a64.size() == 16U,
           "staged artifact hash provenance");
    expect(telemetry.kernel_resources.size() == 9U,
           "all staged kernels expose runtime resource metadata");
    for (const auto &resource : telemetry.kernel_resources) {
        expect(!resource.name.empty() && resource.register_count > 0
                   && resource.local_bytes == 0
                   && resource.binary_version >= 75,
               "runtime kernel resources remain spill-free and identify codegen");
    }
    expect(telemetry.kernel_launch_count >= 1U
               && telemetry.analyzed_candidate_count == candidates.size(),
           "staged launch telemetry");
    expect(telemetry.source_upload_bytes
               == static_cast<std::size_t>(width * height) * sizeof(float),
           "padded host rows upload as a contiguous source image");
    expect(telemetry.plan_upload_bytes > 0U && telemetry.result_readback_bytes > 0U,
           "baseline transfer telemetry");
    expect(engine.peak_workspace_elements() > 0U
               && engine.peak_working_set_bytes() > 0U,
           "baseline workspace telemetry");
    expect(telemetry.initial_device_free_bytes > 0U
               && telemetry.device_memory_reserve_bytes > 0U
               && telemetry.device_memory_budget_bytes
                    + telemetry.device_memory_reserve_bytes
                    == telemetry.initial_device_free_bytes
               && telemetry.per_slot_memory_budget_bytes > 0U
               && telemetry.effective_workspace_limit_bytes > 0U
               && telemetry.effective_workspace_limit_bytes
                    <= telemetry.per_slot_memory_budget_bytes,
           "adaptive CUDA memory budget provenance");

    engine.reset_analysis_telemetry();
    const auto reset = engine.runtime_telemetry();
    expect(reset.kernel_launch_count == 0U && reset.analyzed_candidate_count == 0U,
           "telemetry reset clears counters");
    expect(reset.artifact_name == telemetry.artifact_name
               && reset.artifact_target == telemetry.artifact_target
               && reset.artifact_hash_fnv1a64 == telemetry.artifact_hash_fnv1a64,
           "telemetry reset preserves artifact provenance");
    expect(reset.initial_device_free_bytes == telemetry.initial_device_free_bytes
               && reset.device_memory_budget_bytes
                    == telemetry.device_memory_budget_bytes
               && reset.effective_workspace_limit_bytes
                    == telemetry.effective_workspace_limit_bytes,
           "telemetry reset preserves memory-budget provenance");

    const auto cached = engine.analyze_axis_batch_f32(
        source.view, candidates, metric);
    expect(cached.size() == actual.size(), "cached CUDA result count");
    for (std::size_t index = 0; index < cached.size(); ++index) {
        expect(cached[index].id == actual[index].id
                   && cached[index].error == actual[index].error,
               "cached CUDA batch remains bitwise stable");
    }
    const auto cache_telemetry = engine.runtime_telemetry();
    expect(cache_telemetry.plan_cache_hits == 1U
               && cache_telemetry.plan_cache_misses == 0U
               && cache_telemetry.plan_upload_bytes == 0U,
           "warm CUDA batch reuses resident plans");
    expect(cache_telemetry.buffer_allocation_count == 0U,
           "warm CUDA batch reuses persistent buffers");

    engine.reset_analysis_telemetry();
    auto concurrent_a = std::async(std::launch::async, [&] {
        return engine.analyze_axis_batch_f32(source.view, candidates, metric);
    });
    auto concurrent_b = std::async(std::launch::async, [&] {
        return engine.analyze_axis_batch_f32(source.view, candidates, metric);
    });
    const auto result_a = concurrent_a.get();
    const auto result_b = concurrent_b.get();
    expect(result_a.size() == actual.size() && result_b.size() == actual.size(),
           "concurrent CUDA calls return complete batches");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expect(result_a[index].error == actual[index].error
                   && result_b[index].error == actual[index].error,
               "concurrent CUDA calls remain bitwise stable");
    }

    std::stop_source cancelled;
    cancelled.request_stop();
    expect_throws<std::runtime_error>(
        [&] {
            (void)engine.analyze_axis_batch_f32(
                source.view, candidates, metric, cancelled.get_token());
        },
        "a pre-cancelled baseline call must not launch");

    getnative::MetricSpec unsupported_metric = metric;
    unsupported_metric.norm = 2U;
    expect_throws<std::invalid_argument>(
        [&] {
            (void)engine.analyze_axis_batch_f32(
                source.view, candidates, unsupported_metric);
        },
        "baseline must reject unsupported p-norms");

    getnative::CudaAnalysisOptions limited_options = options;
    limited_options.workspace_limit_elements = 1U;
    getnative::CudaAnalysisEngine limited(limited_options);
    expect_throws<std::length_error>(
        [&] {
            (void)limited.analyze_axis_batch_f32(source.view, candidates, metric);
        },
        "workspace limit must fail before kernel launch");

    getnative::CudaAnalysisOptions tiled_options = options;
    tiled_options.execution_slots = 1U;
    tiled_options.workspace_limit_elements = 1500U;
    getnative::CudaAnalysisEngine tiled(tiled_options);
    const auto tiled_results = tiled.analyze_axis_batch_f32(
        source.view, candidates, metric);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expect(tiled_results[index].error == actual[index].error,
               "candidate tiling preserves deterministic CUDA output");
    }
    const auto tiled_telemetry = tiled.runtime_telemetry();
    expect(tiled_telemetry.tile_count > 1U
               && tiled.peak_workspace_elements() <= 1500U,
           "candidate tiling honors the peak workspace limit");

    getnative::CudaAnalysisOptions alternate_options = options;
    alternate_options.launch_policy.inverse_threads = 128U;
    alternate_options.launch_policy.pixel_threads = 128U;
    alternate_options.launch_policy.maximum_metric_blocks = 64U;
    alternate_options.launch_policy.paired_vertical = false;
    getnative::CudaAnalysisEngine alternate(alternate_options);
    const auto alternate_results = alternate.analyze_axis_batch_f32(
        source.view, candidates, metric);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const double tolerance = std::max(
            2e-7, 5e-4 * std::abs(expected[index]));
        expect(std::abs(alternate_results[index].error - expected[index])
                   <= tolerance,
               "alternate launch policy remains within the CPU tolerance");
    }
}

void test_vertical_pair_tail(const getnative::CudaRuntimeProbe &probe) {
    constexpr std::int32_t width = 131;
    constexpr std::int32_t height = 47;
    const SourceFixture source = make_source(width, height, width + 7);
    const std::vector<getnative::CandidateAnalysis> candidates{
        {
            "vertical-pair-f6", nullptr,
            make_plan(height, 31, 31.25, -0.125, getnative::Filter::lanczos(3)),
            getnative::AnalysisAxes::vertical,
        },
        {
            "vertical-pair-f4", nullptr,
            make_plan(height, 29, 29.0, 0.125, getnative::Filter::bicubic()),
            getnative::AnalysisAxes::vertical,
        },
    };
    const getnative::MetricSpec metric{3, 2, 2, 3, 0.0F, 1U};

    std::vector<double> expected;
    expected.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        expected.push_back(cpu_metric(source.view, candidate, metric));
    }

    getnative::CudaAnalysisOptions options;
    options.device_ordinal = compatible_device(probe).ordinal;
    getnative::CudaAnalysisEngine engine(options);
    const auto actual = engine.analyze_axis_batch_f32(
        source.view, candidates, metric);
    expect(actual.size() == candidates.size(),
           "vertical pair returns one result per candidate");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const double tolerance = std::max(
            2e-7, 5e-4 * std::abs(expected[index]));
        expect(std::abs(actual[index].error - expected[index]) <= tolerance,
               "vertical pair tail remains within the CPU tolerance");
    }

    const auto cached = engine.analyze_axis_batch_f32(
        source.view, candidates, metric);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expect(cached[index].error == actual[index].error,
               "vertical pair warm run remains bitwise stable");
    }
}

void test_unapproved_variants_are_closed() {
    for (const auto variant : {
             getnative::CudaKernelVariant::cpp_specialized,
             getnative::CudaKernelVariant::architecture_specific,
             getnative::CudaKernelVariant::inline_ptx,
         }) {
        getnative::CudaAnalysisOptions options;
        options.kernel_variant = variant;
        expect_throws<std::runtime_error>(
            [&] { getnative::CudaAnalysisEngine rejected(options); },
            "an unapproved CUDA stage must remain unavailable");
    }
}

void test_invalid_launch_policies_are_closed() {
    {
        getnative::CudaAnalysisOptions options;
        options.launch_policy.inverse_threads = 48U;
        expect_throws<std::invalid_argument>(
            [&] { getnative::CudaAnalysisEngine rejected(options); },
            "non-power-of-two inverse threads must be rejected");
    }
    {
        getnative::CudaAnalysisOptions options;
        options.launch_policy.pixel_threads = 512U;
        expect_throws<std::invalid_argument>(
            [&] { getnative::CudaAnalysisEngine rejected(options); },
            "oversized pixel blocks must be rejected");
    }
    {
        getnative::CudaAnalysisOptions options;
        options.launch_policy.maximum_metric_blocks = 0U;
        expect_throws<std::invalid_argument>(
            [&] { getnative::CudaAnalysisEngine rejected(options); },
            "an empty metric grid must be rejected");
    }
}

} // namespace

int main() {
    try {
        test_default_launch_policy_is_frozen();
        test_memory_budget_policy();
        test_unapproved_variants_are_closed();
        test_invalid_launch_policies_are_closed();
        const getnative::CudaRuntimeProbe probe = getnative::cuda_runtime_probe();
        if (!probe.device_available) {
            std::cout << "SKIP: "
                      << (probe.reason.empty()
                              ? "no CUDA device is available" : probe.reason)
                      << '\n';
            return 0;
        }
        test_generic_baseline(probe);
        test_vertical_pair_tail(probe);
        std::cout << "CUDA cpp-generic baseline passed on "
                  << compatible_device(probe).name << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CUDA baseline test failed: " << error.what() << '\n';
        return 1;
    }
}

#include "getnative/cuda_analysis.hpp"

#include "cuda_driver.hpp"
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

[[nodiscard]] std::shared_ptr<const getnative::AxisPlan> make_zero_plan(
    std::int32_t size) {
    getnative::AxisPlan plan;
    plan.source_size = size;
    plan.destination_size = size;
    plan.support = 1;
    plan.half_bandwidth = 0;
    plan.forward_width = 1;
    plan.active_length = static_cast<double>(size);
    plan.forward_offsets.reserve(static_cast<std::size_t>(size) + 1U);
    plan.transpose_offsets.assign(static_cast<std::size_t>(size) + 1U, 0U);
    plan.forward_indices.reserve(static_cast<std::size_t>(size));
    plan.forward_weights.assign(static_cast<std::size_t>(size), 0.0F);
    plan.inverse_diagonal.assign(static_cast<std::size_t>(size), 1.0F);
    for (std::int32_t index = 0; index < size; ++index) {
        plan.forward_offsets.push_back(static_cast<std::uint32_t>(index));
        plan.forward_indices.push_back(index);
    }
    plan.forward_offsets.push_back(static_cast<std::uint32_t>(size));
    expect(plan.valid(), "zero reconstruction test plan is valid");
    return std::make_shared<const getnative::AxisPlan>(std::move(plan));
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

void expect_cuda_matches_cpu(
    getnative::CudaAnalysisEngine &engine, getnative::ConstImageView source,
    const std::vector<getnative::CandidateAnalysis> &candidates,
    const getnative::MetricSpec &metric, std::string_view context) {
    std::vector<double> expected;
    expected.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        expected.push_back(cpu_metric(source, candidate, metric));
    }
    const auto actual = engine.analyze_axis_batch_f32(source, candidates, metric);
    expect(actual.size() == candidates.size(), "CUDA norm result count");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const double tolerance = std::max(
            1e-7, 5e-4 * std::abs(expected[index]));
        if (actual[index].id != candidates[index].id
            || !std::isfinite(actual[index].error)
            || std::abs(actual[index].error - expected[index]) > tolerance) {
            throw std::runtime_error(
                std::string{context} + " mismatch for " + candidates[index].id
                + ": cuda=" + std::to_string(actual[index].error)
                + ", cpu=" + std::to_string(expected[index])
                + ", tolerance=" + std::to_string(tolerance));
        }
    }
}

template <class Sample>
void expect_device_luma_matches_cpu(
    getnative::CudaAnalysisEngine &engine,
    const std::vector<Sample> &encoded,
    const std::vector<float> &normalized,
    std::int32_t width, std::int32_t height,
    getnative::CudaLumaFormat format, std::int32_t bit_depth,
    getnative::CudaColorRange range, std::string_view context) {
    const auto api = getnative::cuda_detail::load_cuda_driver();
    getnative::cuda_detail::cuda_check(*api, api->init(0U), "cuInit(test)");
    CUcontext previous = nullptr;
    getnative::cuda_detail::cuda_check(
        *api, api->ctx_get_current(&previous), "cuCtxGetCurrent(test)");
    const auto native_context = reinterpret_cast<CUcontext>(engine.native_context());
    const auto producer_stream = reinterpret_cast<CUstream>(
        engine.native_decode_stream());
    getnative::cuda_detail::cuda_check(
        *api, api->ctx_set_current(native_context), "cuCtxSetCurrent(test)");

    CUdeviceptr device = 0U;
    try {
        const std::size_t encoded_bytes = encoded.size() * sizeof(Sample);
        getnative::cuda_detail::cuda_check(
            *api, api->mem_alloc(&device, encoded_bytes), "cuMemAlloc(test luma)");
        getnative::cuda_detail::cuda_check(
            *api,
            api->memcpy_htod_async(
                device, encoded.data(), encoded_bytes, producer_stream),
            "cuMemcpyHtoDAsync(test luma)");

        const auto candidates = make_candidates(width, height);
        const getnative::MetricSpec metric{1, 2, 2, 1, 0.0F, 1U};
        const getnative::ConstImageView cpu_source{
            normalized.data(), width, height, width,
        };
        std::vector<double> expected;
        expected.reserve(candidates.size());
        for (const auto &candidate : candidates) {
            expected.push_back(cpu_metric(cpu_source, candidate, metric));
        }

        const getnative::CudaLumaFrameView source{
            static_cast<std::uintptr_t>(device),
            static_cast<std::size_t>(width) * sizeof(Sample),
            width,
            height,
            format,
            bit_depth,
            range,
            engine.native_context(),
            engine.native_decode_stream(),
        };
        engine.reset_analysis_telemetry();
        const auto actual = engine.analyze_axis_batch_cuda_luma(
            source, candidates, metric);
        expect(actual.size() == expected.size(),
               "CUDA device luma result count");
        for (std::size_t index = 0U; index < actual.size(); ++index) {
            const double tolerance = std::max(
                2e-7, 5e-4 * std::abs(expected[index]));
            if (std::abs(actual[index].error - expected[index]) > tolerance) {
                throw std::runtime_error(
                    std::string{context} + " mismatch for "
                    + candidates[index].id + ": cuda="
                    + std::to_string(actual[index].error) + ", cpu="
                    + std::to_string(expected[index]));
            }
        }
        const auto telemetry = engine.runtime_telemetry();
        const std::size_t f32_bytes = normalized.size() * sizeof(float);
        expect(telemetry.source_upload_bytes == 0U
                   && telemetry.source_upload_count == 0U,
               "CUDA device luma performs no frame-sized host upload");
        expect(telemetry.source_conversion_bytes == f32_bytes
                   && telemetry.source_conversion_count == 1U
                   && telemetry.source_conversion_ms >= 0.0,
               "CUDA device luma records device-local conversion telemetry");

        getnative::cuda_detail::cuda_check(
            *api, api->mem_free(device), "cuMemFree(test luma)");
        device = 0U;
        getnative::cuda_detail::cuda_check(
            *api, api->ctx_set_current(previous), "cuCtxSetCurrent(restore test)");
    } catch (...) {
        if (device != 0U) (void)api->mem_free(device);
        (void)api->ctx_set_current(previous);
        throw;
    }
}

void test_device_luma_conversion(const getnative::CudaRuntimeProbe &probe) {
    constexpr std::int32_t width = 67;
    constexpr std::int32_t height = 43;
    const std::size_t pixels = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    getnative::CudaAnalysisOptions options;
    options.device_ordinal = compatible_device(probe).ordinal;
    options.execution_slots = 1U;
    getnative::CudaAnalysisEngine engine(options);

    std::vector<std::uint8_t> nv12(pixels);
    std::vector<float> nv12_f32(pixels);
    for (std::size_t index = 0U; index < pixels; ++index) {
        const auto sample = static_cast<std::uint8_t>((index * 37U + 11U) % 256U);
        nv12[index] = sample;
        nv12_f32[index] = static_cast<float>(sample) * 256.0F / 65535.0F;
    }
    expect_device_luma_matches_cpu(
        engine, nv12, nv12_f32, width, height,
        getnative::CudaLumaFormat::nv12, 8,
        getnative::CudaColorRange::full, "NV12 full-range conversion");

    std::vector<std::uint16_t> p010(pixels);
    std::vector<float> p010_f32(pixels);
    for (std::size_t index = 0U; index < pixels; ++index) {
        const auto sample = static_cast<std::uint16_t>(64U + (index * 53U) % 877U);
        p010[index] = static_cast<std::uint16_t>(sample << 6U);
        p010_f32[index] = static_cast<float>(sample) * 64.0F / 65535.0F;
    }
    expect_device_luma_matches_cpu(
        engine, p010, p010_f32, width, height,
        getnative::CudaLumaFormat::p010, 10,
        getnative::CudaColorRange::limited, "P010 limited-range conversion");
}

void test_supported_norms(const getnative::CudaRuntimeProbe &probe) {
    constexpr std::int32_t width = 37;
    constexpr std::int32_t height = 29;
    const SourceFixture source = make_source(width, height, width + 5);
    const auto candidates = make_candidates(width, height);

    getnative::CudaAnalysisOptions options;
    options.device_ordinal = compatible_device(probe).ordinal;
    options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
    options.execution_slots = 1U;
    getnative::CudaAnalysisEngine engine(options);
    for (std::uint32_t norm = getnative::cuda_minimum_p_norm;
         norm <= getnative::cuda_maximum_p_norm; ++norm) {
        const getnative::MetricSpec metric{2, 3, 1, 2, 0.015F, norm};
        expect_cuda_matches_cpu(
            engine, source.view, candidates, metric,
            "CUDA p=" + std::to_string(norm));
    }

    getnative::CudaAnalysisOptions tiled_options = options;
    tiled_options.workspace_limit_elements = 1500U;
    getnative::CudaAnalysisEngine tiled(tiled_options);
    const getnative::MetricSpec p4_metric{2, 3, 1, 2, 0.015F, 4U};
    expect_cuda_matches_cpu(
        tiled, source.view, candidates, p4_metric, "CUDA tiled p=4");
    expect(tiled.runtime_telemetry().tile_count > 1U,
           "CUDA p=4 exercises multiple candidate tiles");

    const std::span<const getnative::CandidateAnalysis> single{
        candidates.data() + 1U, 1U};
    const auto single_result = engine.analyze_axis_batch_f32(
        source.view, single, p4_metric);
    expect(single_result.size() == 1U && single_result[0].id == "vertical",
           "CUDA p=4 supports a single tail-shaped candidate");
}

void test_strict_threshold_boundary(const getnative::CudaRuntimeProbe &probe) {
    constexpr std::int32_t width = 19;
    constexpr std::int32_t height = 13;
    SourceFixture source;
    source.storage.assign(static_cast<std::size_t>(width * height), 0.25F);
    source.view = {source.storage.data(), width, height, width};
    const std::vector<getnative::CandidateAnalysis> candidates{{
        "strict-threshold", make_zero_plan(width), nullptr,
        getnative::AnalysisAxes::horizontal,
    }};

    getnative::CudaAnalysisOptions options;
    options.device_ordinal = compatible_device(probe).ordinal;
    options.execution_slots = 1U;
    getnative::CudaAnalysisEngine engine(options);
    for (std::uint32_t norm = getnative::cuda_minimum_p_norm;
         norm <= getnative::cuda_maximum_p_norm; ++norm) {
        const getnative::MetricSpec equal{1, 2, 1, 2, 0.25F, norm};
        const auto excluded = engine.analyze_axis_batch_f32(
            source.view, candidates, equal);
        expect(excluded.size() == 1U && excluded[0].error == 0.0,
               "CUDA threshold equality is excluded");

        getnative::MetricSpec below = equal;
        below.threshold = std::nextafter(0.25F, 0.0F);
        const auto included = engine.analyze_axis_batch_f32(
            source.view, candidates, below);
        expect(included.size() == 1U && included[0].error == 0.25,
               "CUDA threshold uses strict greater-than for p=1..4");
    }
}

[[nodiscard]] std::vector<std::size_t> top_local_valleys(
    const std::vector<double> &errors, std::size_t maximum_count) {
    std::vector<std::size_t> valleys;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        const bool lower_than_left = index == 0U || errors[index] <= errors[index - 1U];
        const bool lower_than_right = index + 1U == errors.size()
            || errors[index] <= errors[index + 1U];
        if (lower_than_left && lower_than_right) valleys.push_back(index);
    }
    std::stable_sort(valleys.begin(), valleys.end(), [&](std::size_t left, std::size_t right) {
        return errors[left] < errors[right];
    });
    if (valleys.size() > maximum_count) valleys.resize(maximum_count);
    std::sort(valleys.begin(), valleys.end());
    return valleys;
}

void test_search_ranking_stability(const getnative::CudaRuntimeProbe &probe) {
    constexpr std::int32_t width = 131;
    constexpr std::int32_t height = 47;
    const SourceFixture source = make_source(width, height, width + 3);
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(17U);
    for (std::size_t index = 0; index < 17U; ++index) {
        const double active_length = 78.0 + static_cast<double>(index) * 0.25;
        candidates.push_back({
            std::to_string(index),
            make_plan(width, 82, active_length, 0.0, getnative::Filter::bicubic()),
            nullptr,
            getnative::AnalysisAxes::horizontal,
        });
    }

    getnative::CudaAnalysisOptions options;
    options.device_ordinal = compatible_device(probe).ordinal;
    options.execution_slots = 1U;
    getnative::CudaAnalysisEngine engine(options);
    for (std::uint32_t norm = getnative::cuda_minimum_p_norm;
         norm <= getnative::cuda_maximum_p_norm; ++norm) {
        const getnative::MetricSpec metric{3, 2, 2, 3, 0.015F, norm};
        std::vector<double> cpu_errors;
        cpu_errors.reserve(candidates.size());
        for (const auto &candidate : candidates) {
            cpu_errors.push_back(cpu_metric(source.view, candidate, metric));
        }
        const auto cuda_results = engine.analyze_axis_batch_f32(
            source.view, candidates, metric);
        std::vector<double> cuda_errors;
        cuda_errors.reserve(cuda_results.size());
        for (std::size_t index = 0; index < cuda_results.size(); ++index) {
            const double tolerance = std::max(
                1e-7, 5e-4 * std::abs(cpu_errors[index]));
            expect(std::abs(cuda_results[index].error - cpu_errors[index]) <= tolerance,
                   "CUDA search candidate remains within CPU tolerance");
            cuda_errors.push_back(cuda_results[index].error);
        }
        const std::size_t cpu_best = static_cast<std::size_t>(std::distance(
            cpu_errors.begin(), std::min_element(cpu_errors.begin(), cpu_errors.end())));
        const std::size_t cuda_best = static_cast<std::size_t>(std::distance(
            cuda_errors.begin(), std::min_element(cuda_errors.begin(), cuda_errors.end())));
        expect(cpu_best <= cuda_best + 1U && cuda_best <= cpu_best + 1U,
               "CUDA search minimum stays within one candidate step");
        expect(top_local_valleys(cpu_errors, 3U) == top_local_valleys(cuda_errors, 3U),
               "CUDA top local-valley set matches CPU");
    }
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

    // A vertical-only call caches the source without needing its transpose.
    // The following both-axes call must still build the transpose instead of
    // treating source residency as proof that both device representations exist.
    {
        getnative::CudaAnalysisOptions sequence_options;
        sequence_options.device_ordinal = compatible_device(probe).ordinal;
        sequence_options.kernel_variant = getnative::CudaKernelVariant::cpp_generic;
        sequence_options.execution_slots = 1U;
        getnative::CudaAnalysisEngine sequence_engine(sequence_options);
        const std::span<const getnative::CandidateAnalysis> vertical_only{
            candidates.data() + 1U, 1U};
        const std::span<const getnative::CandidateAnalysis> both_after_vertical{
            candidates.data() + 2U, 1U};
        (void)sequence_engine.analyze_axis_batch_f32(
            source.view, vertical_only, metric);
        const auto sequence_result = sequence_engine.analyze_axis_batch_f32(
            source.view, both_after_vertical, metric);
        const double tolerance = std::max(2e-7, 5e-4 * std::abs(expected[2]));
        expect(sequence_result.size() == 1U
                   && std::abs(sequence_result[0].error - expected[2]) <= tolerance,
               "CUDA source cache keeps transpose validity separate");
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
    expect(telemetry.kernel_resources.size() == 10U,
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
    expect(telemetry.source_upload_count == 1U
               && telemetry.source_transpose_count == 1U,
           "one immutable source upload and transpose serve the batch");
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

    for (const std::uint32_t unsupported_norm : {
             0U, getnative::cuda_maximum_p_norm + 1U,
             std::numeric_limits<std::uint32_t>::max(),
         }) {
        getnative::MetricSpec unsupported_metric = metric;
        unsupported_metric.norm = unsupported_norm;
        expect_throws<std::invalid_argument>(
            [&] {
                (void)engine.analyze_axis_batch_f32(
                    source.view, candidates, unsupported_metric);
            },
            "baseline must reject unsupported p-norms");
    }

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
        test_supported_norms(probe);
        test_strict_threshold_boundary(probe);
        test_search_ranking_stability(probe);
        test_vertical_pair_tail(probe);
        test_device_luma_conversion(probe);
        std::cout << "CUDA cpp-generic baseline passed on "
                  << compatible_device(probe).name << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CUDA baseline test failed: " << error.what() << '\n';
        return 1;
    }
}

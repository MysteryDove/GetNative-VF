#include "getnative/cuda_analysis.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct TestConfig {
    getnative::CudaKernelVariant variant = getnative::CudaKernelVariant::automatic;
    std::int32_t device_ordinal = 0;
    std::string device_uuid;
};

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <class Exception = std::exception, class Function>
void expect_throws(Function &&function, std::string_view message) {
    try {
        function();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

[[nodiscard]] getnative::CudaKernelVariant parse_variant(std::string_view value) {
    if (value == "automatic") return getnative::CudaKernelVariant::automatic;
    if (value == "cpp-generic") return getnative::CudaKernelVariant::cpp_generic;
    if (value == "cpp-specialized") return getnative::CudaKernelVariant::cpp_specialized;
    if (value == "architecture-specific") {
        return getnative::CudaKernelVariant::architecture_specific;
    }
    if (value == "inline-ptx") return getnative::CudaKernelVariant::inline_ptx;
    throw std::invalid_argument("unknown --cuda-variant value");
}

[[nodiscard]] TestConfig parse_arguments(int argc, char **argv) {
    TestConfig result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next = [&](std::string_view name) -> std::string_view {
            if (++index >= argc) throw std::invalid_argument(std::string{name} + " needs a value");
            return argv[index];
        };
        if (argument == "--cuda-variant") {
            result.variant = parse_variant(next(argument));
        } else if (argument == "--device-ordinal") {
            result.device_ordinal = std::stoi(std::string{next(argument)});
        } else if (argument == "--device-uuid") {
            result.device_uuid = next(argument);
        } else {
            throw std::invalid_argument("unknown CUDA conformance argument: "
                                        + std::string{argument});
        }
    }
    return result;
}

[[nodiscard]] getnative::CudaAnalysisOptions make_options(
    const TestConfig &config, getnative::CudaKernelVariant variant,
    std::size_t tile_size = 4U, std::size_t groups = 8U,
    std::size_t workspace_limit = 0U) {
    getnative::CudaAnalysisOptions options;
    options.device_uuid = config.device_uuid;
    options.device_ordinal = config.device_ordinal;
    options.tile_size = tile_size;
    options.reduction_groups_per_candidate = groups;
    options.workspace_limit_elements = workspace_limit;
    options.kernel_variant = variant;
    return options;
}

[[nodiscard]] std::vector<float> make_source(std::int32_t width,
                                              std::int32_t height) {
    std::vector<float> source(static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            source[static_cast<std::size_t>(y * width + x)] = static_cast<float>(
                0.42 + 0.23 * std::sin(0.073 * static_cast<double>(x))
                + 0.19 * std::cos(0.091 * static_cast<double>(y))
                + 0.07 * std::sin(0.037 * static_cast<double>(x + y)));
        }
    }
    return source;
}

[[nodiscard]] getnative::ConstImageView view_of(const std::vector<float> &pixels,
                                                 std::int32_t width,
                                                 std::int32_t height,
                                                 std::int32_t stride = 0) {
    return {pixels.data(), width, height, stride == 0 ? width : stride};
}

[[nodiscard]] std::vector<double> cpu_metrics(
    getnative::ConstImageView source,
    std::span<const getnative::CandidateAnalysis> candidates,
    const getnative::MetricSpec &metric) {
    std::vector<double> result;
    result.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        getnative::CpuWorkspace workspace;
        if (candidate.axes == getnative::AnalysisAxes::both) {
            result.push_back(getnative::analyze_candidate_f32(
                source, *candidate.horizontal, *candidate.vertical, metric, workspace));
        } else {
            const auto &plan = candidate.axes == getnative::AnalysisAxes::horizontal
                ? candidate.horizontal : candidate.vertical;
            result.push_back(getnative::analyze_axis_candidate_f32(
                source, *plan, candidate.axes, metric, workspace));
        }
    }
    return result;
}

[[nodiscard]] std::vector<getnative::CandidateResult> compare_cpu_cuda(
    getnative::ConstImageView source,
    const std::vector<getnative::CandidateAnalysis> &candidates,
    const getnative::MetricSpec &metric,
    getnative::CudaAnalysisEngine &cuda,
    std::string_view label) {
    const std::vector<double> cpu = cpu_metrics(source, candidates, metric);
    const auto gpu = cuda.analyze_axis_batch_f32(source, candidates, metric);
    expect(gpu.size() == candidates.size(), "CUDA returns one result per candidate");
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        expect(gpu[index].id == candidates[index].id,
               "CUDA retains candidate order and ids");
        expect(std::isfinite(gpu[index].error), "CUDA metric is finite");
        const double tolerance = std::max(1e-7, 5e-4 * std::abs(cpu[index]));
        if (std::abs(gpu[index].error - cpu[index]) > tolerance) {
            throw std::runtime_error(std::string{label}
                                     + " CUDA metric exceeds strict CPU tolerance");
        }
    }
    if (!gpu.empty()) {
        const std::size_t cpu_minimum = static_cast<std::size_t>(
            std::min_element(cpu.begin(), cpu.end()) - cpu.begin());
        const std::size_t gpu_minimum = static_cast<std::size_t>(
            std::min_element(gpu.begin(), gpu.end(), [](const auto &left, const auto &right) {
                return left.error < right.error;
            }) - gpu.begin());
        const std::size_t distance = cpu_minimum > gpu_minimum
            ? cpu_minimum - gpu_minimum : gpu_minimum - cpu_minimum;
        if (distance > 1U) {
            throw std::runtime_error(std::string{label}
                                     + " CUDA minimum differs by more than one step");
        }
    }
    return gpu;
}

[[nodiscard]] getnative::CandidateAnalysis make_axis_candidate(
    getnative::AxisPlanCache &cache, std::string id,
    getnative::AnalysisAxes axes, std::int32_t source_size,
    std::int32_t native_size, double active_size, double shift,
    const getnative::Filter &filter) {
    auto plan = cache.get_or_build({source_size, native_size, active_size, shift,
                                    filter, getnative::BorderMode::mirror});
    if (axes == getnative::AnalysisAxes::horizontal) {
        return {std::move(id), std::move(plan), nullptr, axes};
    }
    return {std::move(id), nullptr, std::move(plan), axes};
}

[[nodiscard]] getnative::CandidateAnalysis make_dual_candidate(
    getnative::AxisPlanCache &cache, std::string id,
    std::int32_t source_width, std::int32_t source_height,
    std::int32_t native_width, std::int32_t native_height,
    double active_width, double active_height,
    const getnative::Filter &filter) {
    return {
        std::move(id),
        cache.get_or_build({source_width, native_width, active_width, 0.125,
                            filter, getnative::BorderMode::mirror}),
        cache.get_or_build({source_height, native_height, active_height, -0.0625,
                            filter, getnative::BorderMode::mirror}),
        getnative::AnalysisAxes::both,
    };
}

[[nodiscard]] const std::vector<std::pair<std::string_view, getnative::Filter>> &
all_filters() {
    static const std::vector<std::pair<std::string_view, getnative::Filter>> filters{
        {"bilinear", getnative::Filter::bilinear()},
        {"bicubic-arbitrary", getnative::Filter::bicubic(0.25, 0.4)},
        {"spline16", getnative::Filter::spline16()},
        {"spline36", getnative::Filter::spline36()},
        {"spline64", getnative::Filter::spline64()},
        {"lanczos1", getnative::Filter::lanczos(1)},
        {"lanczos2", getnative::Filter::lanczos(2)},
        {"lanczos3", getnative::Filter::lanczos(3)},
        {"lanczos4", getnative::Filter::lanczos(4)},
        {"lanczos5", getnative::Filter::lanczos(5)},
        {"lanczos6", getnative::Filter::lanczos(6)},
        {"lanczos7", getnative::Filter::lanczos(7)},
        {"lanczos8", getnative::Filter::lanczos(8)},
    };
    return filters;
}

[[nodiscard]] const std::vector<std::pair<std::string_view, getnative::Filter>> &
fixed_filters() {
    static const std::vector<std::pair<std::string_view, getnative::Filter>> filters{
        {"b3", getnative::Filter::bilinear()},
        {"b7", getnative::Filter::bicubic(0.25, 0.4)},
        {"b11", getnative::Filter::spline36()},
        {"b15", getnative::Filter::spline64()},
    };
    return filters;
}

void test_probe_and_selectors(const TestConfig &config) {
    const auto probe = getnative::cuda_runtime_probe();
    expect(probe.driver_loaded && probe.initialized && probe.device_available,
           "available CUDA probe reports driver, initialization, and device");
    const auto devices = getnative::enumerate_cuda_devices();
    expect(!devices.empty() && devices.size() == probe.devices.size(),
           "CUDA enumeration agrees with the runtime probe");
    for (std::size_t index = 0; index < devices.size(); ++index) {
        expect(devices[index].ordinal >= 0 && !devices[index].name.empty()
                   && devices[index].uuid.starts_with("GPU-")
                   && devices[index].compute_capability_major > 0
                   && devices[index].driver_version > 0
                   && devices[index].total_memory_bytes > 0U,
               "CUDA device exposes stable identity and limits");
        if (index != 0U) {
            expect(devices[index - 1U].ordinal < devices[index].ordinal
                       && devices[index - 1U].uuid != devices[index].uuid,
                   "CUDA devices have stable increasing ordinals and unique UUIDs");
        }
    }

    const auto selected = std::find_if(devices.begin(), devices.end(), [&](const auto &item) {
        return config.device_uuid.empty() ? item.ordinal == config.device_ordinal
                                          : item.uuid == config.device_uuid;
    });
    expect(selected != devices.end(), "requested CUDA conformance device exists");
    getnative::CudaAnalysisOptions by_uuid = make_options(config, config.variant);
    by_uuid.device_uuid = selected->uuid;
    by_uuid.device_ordinal = std::numeric_limits<std::int32_t>::max();
    getnative::CudaAnalysisEngine uuid_engine(by_uuid);
    expect(uuid_engine.device_info().uuid == selected->uuid,
           "exact UUID selection is authoritative over ordinal");

    getnative::CudaAnalysisOptions invalid = make_options(config, config.variant);
    invalid.device_uuid = "GPU-00000000-0000-0000-0000-000000000000";
    expect_throws<std::runtime_error>(
        [&] { getnative::CudaAnalysisEngine rejected(invalid); },
        "unknown CUDA UUID is rejected before execution");
}

void test_filter_axis_and_order_matrix(const TestConfig &config) {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto source = make_source(width, height);
    const auto view = view_of(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    getnative::CudaAnalysisEngine cuda(make_options(config, config.variant));
    const auto &filters = config.variant == getnative::CudaKernelVariant::cpp_specialized
        ? fixed_filters() : all_filters();

    std::vector<getnative::CandidateAnalysis> horizontal;
    std::vector<getnative::CandidateAnalysis> vertical;
    std::vector<getnative::CandidateAnalysis> horizontal_first;
    std::vector<getnative::CandidateAnalysis> vertical_first;
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const auto &[name, filter] = filters[index];
        horizontal.push_back(make_axis_candidate(
            cache, std::string{name} + "-h", getnative::AnalysisAxes::horizontal,
            width, 58 + static_cast<std::int32_t>(index % 7U),
            58.3125 + static_cast<double>(index % 7U), 0.125, filter));
        vertical.push_back(make_axis_candidate(
            cache, std::string{name} + "-v", getnative::AnalysisAxes::vertical,
            height, 48 + static_cast<std::int32_t>(index % 7U),
            48.1875 + static_cast<double>(index % 7U), -0.0625, filter));
        horizontal_first.push_back(make_dual_candidate(
            cache, std::string{name} + "-hf", width, height, 90, 40,
            90.25, 40.125, filter));
        vertical_first.push_back(make_dual_candidate(
            cache, std::string{name} + "-vf", width, height, 46, 74,
            46.25, 74.125, filter));
        expect(getnative::select_forward_order(*horizontal_first.back().horizontal,
                                               *horizontal_first.back().vertical)
                   == getnative::ForwardOrder::horizontal_first,
               "dual-axis matrix selects horizontal-first order");
        expect(getnative::select_forward_order(*vertical_first.back().horizontal,
                                               *vertical_first.back().vertical)
                   == getnative::ForwardOrder::vertical_first,
               "dual-axis matrix selects vertical-first order");
    }
    (void)compare_cpu_cuda(view, horizontal, metric, cuda, "horizontal filter matrix");
    (void)compare_cpu_cuda(view, vertical, metric, cuda, "vertical filter matrix");
    (void)compare_cpu_cuda(view, horizontal_first, metric, cuda,
                           "horizontal-first filter matrix");
    (void)compare_cpu_cuda(view, vertical_first, metric, cuda,
                           "vertical-first filter matrix");
    const auto telemetry = cuda.runtime_telemetry();
    expect(telemetry.module_artifact_name == "getnative_cuda.fatbin",
           "CUDA telemetry identifies the single production artifact");
    expect(telemetry.created_kernel_names.size() == 25U,
           "CUDA module resolves all 25 production kernel entry points");
    expect(telemetry.stream_submission_count == telemetry.stream_completion_count
               && telemetry.stream_submission_count > 0U,
           "successful CUDA matrix completes every submitted stream sequence");
    expect(telemetry.total_peak_explicit_bytes < 2ULL * 1024ULL * 1024ULL * 1024ULL,
           "CUDA matrix remains below the two GiB explicit-memory gate");
    if (config.variant == getnative::CudaKernelVariant::cpp_generic) {
        expect(telemetry.generic_tile_count == telemetry.analyzed_tile_count
                   && telemetry.specialized_tile_count == 0U,
               "forced generic matrix dispatches only generic tiles");
    } else if (config.variant == getnative::CudaKernelVariant::cpp_specialized) {
        expect(telemetry.specialized_tile_count == telemetry.analyzed_tile_count
                   && telemetry.generic_tile_count == 0U,
               "forced specialized matrix dispatches only fixed-shape tiles");
    }
}

void test_generic_specialized_identity(const TestConfig &config) {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto source = make_source(width, height);
    const auto view = view_of(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    std::vector<getnative::CandidateAnalysis> candidates;
    for (std::size_t index = 0; index < fixed_filters().size(); ++index) {
        const auto &[name, filter] = fixed_filters()[index];
        candidates.push_back(make_axis_candidate(
            cache, std::string{name} + "-v", getnative::AnalysisAxes::vertical,
            height, 48 + static_cast<std::int32_t>(index),
            48.125 + static_cast<double>(index), -0.0625, filter));
        candidates.push_back(make_axis_candidate(
            cache, std::string{name} + "-h", getnative::AnalysisAxes::horizontal,
            width, 60 + static_cast<std::int32_t>(index),
            60.25 + static_cast<double>(index), 0.125, filter));
        const bool horizontal_first = (index & 1U) == 0U;
        candidates.push_back(make_dual_candidate(
            cache, std::string{name} + (horizontal_first ? "-hf" : "-vf"),
            width, height, horizontal_first ? 90 : 46,
            horizontal_first ? 40 : 74,
            horizontal_first ? 90.25 : 46.25,
            horizontal_first ? 40.125 : 74.125, filter));
    }
    getnative::CudaAnalysisEngine generic(make_options(
        config, getnative::CudaKernelVariant::cpp_generic));
    getnative::CudaAnalysisEngine specialized(make_options(
        config, getnative::CudaKernelVariant::cpp_specialized));
    const auto generic_results = compare_cpu_cuda(
        view, candidates, metric, generic, "forced generic fixed-shape matrix");
    const auto specialized_results = compare_cpu_cuda(
        view, candidates, metric, specialized, "forced specialized fixed-shape matrix");
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (std::bit_cast<std::uint64_t>(generic_results[index].error)
            != std::bit_cast<std::uint64_t>(specialized_results[index].error)) {
            throw std::runtime_error("CUDA specialized and generic metrics differ by bits");
        }
    }
    expect(generic.runtime_telemetry().generic_tile_count > 0U
               && generic.runtime_telemetry().specialized_tile_count == 0U,
           "generic identity oracle reports generic dispatch");
    expect(specialized.runtime_telemetry().specialized_tile_count > 0U
               && specialized.runtime_telemetry().generic_tile_count == 0U,
           "specialized identity path reports fixed-shape dispatch");
}

void test_padded_stride_and_mixed_order(const TestConfig &config) {
    constexpr std::int32_t width = 64;
    constexpr std::int32_t height = 48;
    constexpr std::int32_t stride = 71;
    const auto contiguous = make_source(width, height);
    constexpr float canary = -12345.5F;
    std::vector<float> padded(static_cast<std::size_t>(stride * height), canary);
    for (std::int32_t y = 0; y < height; ++y) {
        std::copy_n(contiguous.data() + static_cast<std::ptrdiff_t>(y * width), width,
                    padded.data() + static_cast<std::ptrdiff_t>(y * stride));
    }
    getnative::AxisPlanCache cache;
    std::vector<getnative::CandidateAnalysis> candidates;
    const auto &filters = config.variant == getnative::CudaKernelVariant::cpp_specialized
        ? fixed_filters() : all_filters();
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const bool horizontal_first = (index & 1U) == 0U;
        candidates.push_back(make_dual_candidate(
            cache, std::string{filters[index].first} + "-mixed", width, height,
            horizontal_first ? 58 : 34, horizontal_first ? 28 : 44,
            horizontal_first ? 58.25 : 34.25,
            horizontal_first ? 28.125 : 44.125, filters[index].second));
    }
    const getnative::MetricSpec metric{3, 3, 3, 3, 0.015F, 1U};
    getnative::CudaAnalysisEngine cuda(make_options(config, config.variant, 2U, 4U));
    const auto contiguous_results = cuda.analyze_axis_batch_f32(
        view_of(contiguous, width, height), candidates, metric);
    const auto padded_results = compare_cpu_cuda(
        view_of(padded, width, height, stride), candidates, metric, cuda,
        "padded mixed-order matrix");
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        expect(std::bit_cast<std::uint64_t>(contiguous_results[index].error)
                   == std::bit_cast<std::uint64_t>(padded_results[index].error),
               "contiguous and padded CUDA source metrics are bit-identical");
    }
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = width; x < stride; ++x) {
            expect(padded[static_cast<std::size_t>(y * stride + x)] == canary,
                   "CUDA source staging leaves row padding unchanged");
        }
    }
}

void test_validation_workspace_and_variants(const TestConfig &config) {
    constexpr std::int32_t width = 32;
    constexpr std::int32_t height = 24;
    const auto source = make_source(width, height);
    const auto view = view_of(source, width, height);
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    auto plan = cache.get_or_build({height, 16, 16.0, 0.0,
                                    getnative::Filter::bicubic(),
                                    getnative::BorderMode::mirror});
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"first", nullptr, plan, getnative::AnalysisAxes::vertical},
        {"second", nullptr, plan, getnative::AnalysisAxes::vertical},
    };
    getnative::CudaAnalysisEngine cuda(make_options(config, config.variant));
    expect(cuda.analyze_axis_batch_f32(view, {}, metric).empty(),
           "CUDA accepts an empty candidate span without submission");
    expect_throws<std::invalid_argument>(
        [&] { (void)cuda.analyze_axis_batch_f32({nullptr, width, height, width}, candidates, metric); },
        "CUDA rejects a null source");
    expect_throws<std::invalid_argument>(
        [&] { (void)cuda.analyze_axis_batch_f32({source.data(), width, height, width - 1}, candidates, metric); },
        "CUDA rejects a source stride smaller than width");
    expect_throws(
        [&] { (void)cuda.analyze_axis_batch_f32(view, candidates, {2, 2, 2, 2, 0.015F, 2U}); },
        "CUDA rejects non-p1 metrics");
    for (float threshold : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
        expect_throws([&] {
            (void)cuda.analyze_axis_batch_f32(view, candidates, {2, 2, 2, 2, threshold, 1U});
        }, "CUDA rejects nonfinite thresholds");
    }
    const std::vector<getnative::CandidateAnalysis> null_plan{
        {"null", nullptr, nullptr, getnative::AnalysisAxes::vertical},
    };
    expect_throws<std::invalid_argument>(
        [&] { (void)cuda.analyze_axis_batch_f32(view, null_plan, metric); },
        "CUDA rejects a null candidate plan");
    auto oversized_plan = cache.get_or_build({height, 20, 20.0, 0.0,
                                              getnative::Filter::lanczos(9),
                                              getnative::BorderMode::mirror});
    const std::vector<getnative::CandidateAnalysis> oversized{
        {"l9", nullptr, oversized_plan, getnative::AnalysisAxes::vertical},
    };
    expect_throws<std::invalid_argument>(
        [&] { (void)cuda.analyze_axis_batch_f32(view, oversized, metric); },
        "CUDA rejects a plan beyond B15/F16");

    getnative::CudaAnalysisEngine constrained(make_options(
        config, config.variant, 2U, 2U, 600U));
    expect(constrained.analyze_axis_batch_f32(view, candidates, metric).size()
               == candidates.size(),
           "CUDA shrinks a tile to satisfy the workspace limit");
    expect(constrained.peak_workspace_elements() == 512U,
           "CUDA adaptive tile retains one candidate workspace");
    getnative::CudaAnalysisEngine too_small(make_options(
        config, config.variant, 2U, 2U, 511U));
    expect_throws<std::length_error>(
        [&] { (void)too_small.analyze_axis_batch_f32(view, candidates, metric); },
        "CUDA rejects one candidate larger than the workspace limit");

    auto unavailable = make_options(config, getnative::CudaKernelVariant::architecture_specific);
    expect_throws<std::runtime_error>(
        [&] { getnative::CudaAnalysisEngine rejected(unavailable); },
        "unapproved architecture-specific CUDA variant is rejected before launch");
    unavailable.kernel_variant = getnative::CudaKernelVariant::inline_ptx;
    expect_throws<std::runtime_error>(
        [&] { getnative::CudaAnalysisEngine rejected(unavailable); },
        "NO_PTX_CANDIDATE variant is rejected before launch");

    auto generic_only = make_options(config, getnative::CudaKernelVariant::cpp_specialized);
    getnative::CudaAnalysisEngine required(generic_only);
    auto lanczos5 = cache.get_or_build({height, 20, 20.0, 0.0,
                                       getnative::Filter::lanczos(5),
                                       getnative::BorderMode::mirror});
    const std::vector<getnative::CandidateAnalysis> unsupported_specialization{
        {"b19", nullptr, lanczos5, getnative::AnalysisAxes::vertical},
    };
    expect_throws<std::runtime_error>(
        [&] { (void)required.analyze_axis_batch_f32(view, unsupported_specialization, metric); },
        "required CUDA specialization rejects a generic-only legal shape");
}

void test_buffer_reuse_trim_and_ceiling(const TestConfig &config) {
    constexpr std::int32_t width = 32;
    constexpr std::int32_t height = 24;
    const auto source = make_source(width, height);
    const auto view = view_of(source, width, height);
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    auto plan = cache.get_or_build({height, 16, 16.0, 0.0,
                                    getnative::Filter::bicubic(),
                                    getnative::BorderMode::mirror});
    std::vector<getnative::CandidateAnalysis> small;
    std::vector<getnative::CandidateAnalysis> large;
    for (std::size_t index = 0; index < 4U; ++index) {
        getnative::CandidateAnalysis candidate{
            std::to_string(index), nullptr, plan, getnative::AnalysisAxes::vertical,
        };
        large.push_back(candidate);
        if (index < 2U) small.push_back(std::move(candidate));
    }

    auto persistent_options = make_options(config, config.variant, 8U, 2U);
    persistent_options.reuse_working_buffers = true;
    persistent_options.retained_working_buffer_limit_bytes = 1024U * 1024U;
    getnative::CudaAnalysisEngine persistent(persistent_options);
    const auto first = persistent.analyze_axis_batch_f32(view, small, metric);
    const auto first_telemetry = persistent.runtime_telemetry();
    expect(first_telemetry.working_buffer_allocation_count == 3U
               && first_telemetry.working_buffer_reuse_count == 0U,
           "CUDA first persistent call allocates three working buffers");

    persistent.reset_analysis_telemetry();
    const auto grown = persistent.analyze_axis_batch_f32(view, large, metric);
    const auto grown_telemetry = persistent.runtime_telemetry();
    expect(grown_telemetry.working_buffer_allocation_count == 2U
               && grown_telemetry.working_buffer_reuse_count == 1U,
           "CUDA grow-to-fit replaces workspace and partial buffers only");

    persistent.reset_analysis_telemetry();
    const auto reused = persistent.analyze_axis_batch_f32(view, large, metric);
    const auto reused_telemetry = persistent.runtime_telemetry();
    expect(reused_telemetry.working_buffer_allocation_count == 0U
               && reused_telemetry.working_buffer_reuse_count == 3U,
           "CUDA stable call reuses all three retained working buffers");
    expect(reused_telemetry.stream_submission_count
               == reused_telemetry.stream_completion_count,
           "CUDA reuse call drains every submitted tile");

    persistent.trim_working_buffers();
    expect(persistent.runtime_telemetry().working_buffer_retained_bytes == 0U,
           "CUDA trim releases every retained working buffer");
    persistent.reset_analysis_telemetry();
    const auto after_trim = persistent.analyze_axis_batch_f32(view, large, metric);
    expect(persistent.runtime_telemetry().working_buffer_allocation_count == 3U,
           "CUDA first call after trim reallocates all working buffers");
    persistent.reset_analysis_telemetry();
    const auto after_trim_reused = persistent.analyze_axis_batch_f32(view, large, metric);

    auto transient_options = persistent_options;
    transient_options.reuse_working_buffers = false;
    getnative::CudaAnalysisEngine transient(transient_options);
    const auto transient_results = transient.analyze_axis_batch_f32(view, large, metric);
    const auto transient_results_second = transient.analyze_axis_batch_f32(
        view, large, metric);
    expect(transient.runtime_telemetry().working_buffer_retained_bytes == 0U,
           "CUDA transient path retains no working buffers");

    auto ceiling_options = persistent_options;
    ceiling_options.retained_working_buffer_limit_bytes = 1U;
    getnative::CudaAnalysisEngine ceiling(ceiling_options);
    const auto ceiling_results = ceiling.analyze_axis_batch_f32(view, large, metric);
    expect(ceiling.runtime_telemetry().working_buffer_retained_bytes == 0U,
           "CUDA retained ceiling falls back to transient buffers");
    expect(first.size() == small.size() && grown.size() == large.size()
               && reused.size() == large.size() && after_trim.size() == large.size()
               && transient_results.size() == large.size()
               && ceiling_results.size() == large.size(),
           "all CUDA working-buffer paths return every candidate");
    for (std::size_t index = 0; index < large.size(); ++index) {
        const std::uint64_t expected = std::bit_cast<std::uint64_t>(grown[index].error);
        const std::uint64_t first_bits = std::bit_cast<std::uint64_t>(
            first[index % first.size()].error);
        const std::uint64_t reused_bits = std::bit_cast<std::uint64_t>(
            reused[index].error);
        const std::uint64_t after_trim_bits = std::bit_cast<std::uint64_t>(
            after_trim[index].error);
        const std::uint64_t after_trim_reused_bits = std::bit_cast<std::uint64_t>(
            after_trim_reused[index].error);
        const std::uint64_t transient_bits = std::bit_cast<std::uint64_t>(
            transient_results[index].error);
        const std::uint64_t transient_second_bits = std::bit_cast<std::uint64_t>(
            transient_results_second[index].error);
        const std::uint64_t ceiling_bits = std::bit_cast<std::uint64_t>(
            ceiling_results[index].error);
        if (reused_bits != expected || after_trim_bits != expected
            || transient_bits != expected || ceiling_bits != expected) {
            throw std::runtime_error(
                "CUDA working-buffer result mismatch at candidate "
                + std::to_string(index) + ": grown=" + std::to_string(expected)
                + ", first=" + std::to_string(first_bits)
                + ", reused=" + std::to_string(reused_bits)
                + ", after_trim=" + std::to_string(after_trim_bits)
                + ", after_trim_reused=" + std::to_string(after_trim_reused_bits)
                + ", transient=" + std::to_string(transient_bits)
                + ", transient_second=" + std::to_string(transient_second_bits)
                + ", ceiling=" + std::to_string(ceiling_bits));
        }
    }
}

void test_cancellation_drain_and_reuse(const TestConfig &config) {
    constexpr std::int32_t width = 320;
    constexpr std::int32_t height = 240;
    const auto source = make_source(width, height);
    const auto view = view_of(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    auto plan = cache.get_or_build({height, 160, 160.0, 0.0,
                                    getnative::Filter::spline64(),
                                    getnative::BorderMode::mirror});
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(1000U);
    for (std::size_t index = 0; index < 1000U; ++index) {
        candidates.push_back({std::to_string(index), nullptr, plan,
                              getnative::AnalysisAxes::vertical});
    }
    getnative::CudaAnalysisEngine cuda(make_options(config, config.variant, 1U, 2U));
    std::stop_source pre_stopped;
    pre_stopped.request_stop();
    expect_throws<std::runtime_error>([&] {
        (void)cuda.analyze_axis_batch_f32(view, candidates, metric, pre_stopped.get_token());
    }, "CUDA observes cancellation before tile planning or submission");
    expect(cuda.runtime_telemetry().stream_submission_count == 0U,
           "pre-cancelled CUDA call submits no kernel work");

    bool observed_submitted_cancellation = false;
    for (const auto delay : {
             std::chrono::milliseconds(10), std::chrono::milliseconds(25),
             std::chrono::milliseconds(50), std::chrono::milliseconds(75),
             std::chrono::milliseconds(100), std::chrono::milliseconds(150),
             std::chrono::milliseconds(250), std::chrono::milliseconds(400),
             std::chrono::milliseconds(650), std::chrono::milliseconds(1000),
         }) {
        cuda.reset_analysis_telemetry();
        std::stop_source stop;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(delay);
            stop.request_stop();
        });
        bool cancelled = false;
        try {
            (void)cuda.analyze_axis_batch_f32(
                view, candidates, metric, stop.get_token());
        } catch (const std::runtime_error &) {
            cancelled = true;
        }
        canceller.join();
        const auto telemetry = cuda.runtime_telemetry();
        if (telemetry.stream_submission_count == 0U) {
            expect(cancelled, "pre-submission CUDA cancellation is observed");
            continue;
        }
        expect(cancelled, "CUDA observes cancellation after stream submission");
        expect(telemetry.stream_completion_count == telemetry.stream_submission_count,
               "CUDA cancellation drains every submitted stream sequence");
        observed_submitted_cancellation = true;
        break;
    }
    expect(observed_submitted_cancellation,
           "submitted-cancellation case reaches a CUDA kernel launch");

    cuda.reset_analysis_telemetry();
    const auto reused = cuda.analyze_axis_batch_f32(
        view, std::span<const getnative::CandidateAnalysis>{candidates.data(), 1U}, metric);
    const auto reuse_telemetry = cuda.runtime_telemetry();
    expect(reused.size() == 1U && std::isfinite(reused.front().error),
           "CUDA engine is reusable immediately after submitted cancellation");
    expect(reuse_telemetry.working_buffer_reuse_count == 3U
               && reuse_telemetry.stream_submission_count
                   == reuse_telemetry.stream_completion_count,
           "post-cancellation reuse waits for drain before reusing buffers");
}

void run_unavailable_variant_rejection(const TestConfig &config) {
    auto options = make_options(config, config.variant);
    expect_throws<std::runtime_error>(
        [&] { getnative::CudaAnalysisEngine rejected(options); },
        "uncompiled CUDA variant must reject before launch");
}

} // namespace

int main(int argc, char **argv) {
    try {
        const TestConfig config = parse_arguments(argc, argv);
        if (config.variant == getnative::CudaKernelVariant::architecture_specific
            || config.variant == getnative::CudaKernelVariant::inline_ptx) {
            run_unavailable_variant_rejection(config);
            std::cout << "CUDA unavailable-variant rejection passed: "
                      << getnative::cuda_kernel_variant_name(config.variant) << '\n';
            return 0;
        }
        const auto probe = getnative::cuda_runtime_probe();
        if (!probe.device_available) {
            std::cout << "CUDA conformance tests skipped: " << probe.reason << '\n';
            return 0;
        }
        test_probe_and_selectors(config);
        test_filter_axis_and_order_matrix(config);
        test_generic_specialized_identity(config);
        test_padded_stride_and_mixed_order(config);
        test_validation_workspace_and_variants(config);
        test_buffer_reuse_trim_and_ceiling(config);
        test_cancellation_drain_and_reuse(config);
        std::cout << "CUDA conformance tests passed: variant="
                  << getnative::cuda_kernel_variant_name(config.variant)
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CUDA conformance failure: " << error.what() << '\n';
        return 1;
    }
}

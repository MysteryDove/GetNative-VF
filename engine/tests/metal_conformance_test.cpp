#include "getnative/metal_analysis.hpp"
#include "getnative/joining_thread.hpp"

#include "getnative/filter.hpp"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
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

[[nodiscard]] getnative::ConstImageView const_view(const std::vector<float> &pixels,
                                                   std::int32_t width,
                                                   std::int32_t height) {
    return {pixels.data(), width, height, width};
}

[[nodiscard]] std::vector<float> make_source(std::int32_t width, std::int32_t height) {
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

void compare_cpu_metal(getnative::ConstImageView source,
                       const std::vector<getnative::CandidateAnalysis> &candidates,
                       const getnative::MetricSpec &metric,
                       getnative::MetalAnalysisEngine &metal,
                       std::string_view label) {
    std::vector<double> cpu;
    cpu.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        getnative::CpuWorkspace workspace;
        if (candidate.axes == getnative::AnalysisAxes::both) {
            cpu.push_back(getnative::analyze_candidate_f32(
                source, *candidate.horizontal, *candidate.vertical, metric, workspace));
        } else {
            const auto &plan = candidate.axes == getnative::AnalysisAxes::horizontal
                ? candidate.horizontal : candidate.vertical;
            cpu.push_back(getnative::analyze_axis_candidate_f32(
                source, *plan, candidate.axes, metric, workspace));
        }
    }
    const auto gpu = metal.analyze_axis_batch_f32(source, candidates, metric);
    expect(gpu.size() == candidates.size(), "Metal returns one result per candidate");
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        expect(gpu[index].id == candidates[index].id, "Metal retains candidate order and ids");
        expect(std::isfinite(gpu[index].error), "Metal metric is finite");
        const double tolerance = std::max(1e-7, 5e-4 * std::abs(cpu[index]));
        if (std::abs(gpu[index].error - cpu[index]) > tolerance) {
            throw std::runtime_error(std::string{label}
                                     + " Metal metric exceeds the strict CPU tolerance");
        }
    }
    const auto cpu_minimum = static_cast<std::size_t>(
        std::min_element(cpu.begin(), cpu.end()) - cpu.begin());
    const auto gpu_minimum = static_cast<std::size_t>(
        std::min_element(gpu.begin(), gpu.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.error < rhs.error;
        }) - gpu.begin());
    const std::size_t distance = cpu_minimum > gpu_minimum
        ? cpu_minimum - gpu_minimum : gpu_minimum - cpu_minimum;
    if (distance > 1) {
        throw std::runtime_error(std::string{label}
                                 + " Metal minimum differs by more than one candidate step");
    }
}

void test_iosurface_zero_copy_luma_format(
    OSType pixel_format, std::int32_t bit_depth,
    std::string_view surface_format, std::string_view range) {
    constexpr std::int32_t width = 64;
    constexpr std::int32_t height = 48;
    const bool ten_bit = bit_depth > 8;
    const bool full_range = range == "full";
    const std::string label = std::string{"IOSurface "} + std::string{surface_format};
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef io_surface_properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    expect(attributes != nullptr && io_surface_properties != nullptr,
           label + " dictionaries allocate");
    CFDictionarySetValue(
        attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface_properties);
    CVPixelBufferRef pixel_buffer = nullptr;
    const CVReturn created = CVPixelBufferCreate(
        kCFAllocatorDefault, static_cast<std::size_t>(width),
        static_cast<std::size_t>(height), pixel_format, attributes, &pixel_buffer);
    CFRelease(io_surface_properties);
    CFRelease(attributes);
    expect(created == kCVReturnSuccess && pixel_buffer != nullptr
               && CVPixelBufferGetIOSurface(pixel_buffer) != nullptr,
           label + " fixture has IOSurface backing");
    struct PixelBufferRelease {
        CVPixelBufferRef value;
        ~PixelBufferRelease() { if (value != nullptr) CVPixelBufferRelease(value); }
    } release{pixel_buffer};

    expect(CVPixelBufferLockBaseAddress(pixel_buffer, 0) == kCVReturnSuccess,
           label + " locks for initialization");
    std::vector<float> source(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    void *luma_base = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0);
    const std::size_t luma_stride_bytes =
        CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
    expect(luma_base != nullptr && luma_stride_bytes > 0,
           label + " luma plane is addressable");
    if (ten_bit) {
        expect(luma_stride_bytes % sizeof(std::uint16_t) == 0,
               label + " luma stride is 16-bit aligned");
        auto *luma = static_cast<std::uint16_t *>(luma_base);
        const std::size_t luma_stride = luma_stride_bytes / sizeof(std::uint16_t);
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                const std::uint16_t code10 = static_cast<std::uint16_t>(
                    96 + ((x * 7 + y * 11 + (x * y) % 31) % 720));
                luma[static_cast<std::size_t>(y) * luma_stride
                     + static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>(code10 << 6);
                const float value = full_range
                    ? static_cast<float>(code10) / 1023.0F
                    : std::max(0.0F, (static_cast<float>(code10) - 64.0F) / 876.0F);
                source[static_cast<std::size_t>(y * width + x)] = value;
            }
        }
    } else {
        auto *luma = static_cast<std::uint8_t *>(luma_base);
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                const std::uint8_t code = static_cast<std::uint8_t>(
                    24 + ((x * 7 + y * 11 + (x * y) % 31) % 208));
                luma[static_cast<std::size_t>(y) * luma_stride_bytes
                     + static_cast<std::size_t>(x)] = code;
                const float value = full_range
                    ? static_cast<float>(code) / 255.0F
                    : std::max(0.0F, (static_cast<float>(code) - 16.0F) / 219.0F);
                source[static_cast<std::size_t>(y * width + x)] = value;
            }
        }
    }
    void *chroma_base = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1);
    const std::size_t chroma_stride_bytes =
        CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
    const std::size_t chroma_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 1);
    expect(chroma_base != nullptr && chroma_stride_bytes > 0,
           label + " chroma plane is addressable");
    if (ten_bit) {
        expect(chroma_stride_bytes % sizeof(std::uint16_t) == 0,
               label + " chroma stride is 16-bit aligned");
        auto *chroma = static_cast<std::uint16_t *>(chroma_base);
        const std::size_t chroma_stride = chroma_stride_bytes / sizeof(std::uint16_t);
        for (std::size_t y = 0; y < chroma_height; ++y) {
            std::fill_n(chroma + y * chroma_stride, chroma_stride,
                        static_cast<std::uint16_t>(512U << 6));
        }
    } else {
        auto *chroma = static_cast<std::uint8_t *>(chroma_base);
        for (std::size_t y = 0; y < chroma_height; ++y) {
            std::fill_n(chroma + y * chroma_stride_bytes, chroma_stride_bytes,
                        std::uint8_t{128});
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

    auto plan = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        height, 32, 32.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"iosurface", nullptr, plan, getnative::AnalysisAxes::vertical},
    };
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    getnative::CpuWorkspace workspace;
    const double cpu = getnative::analyze_axis_candidate_f32(
        const_view(source, width, height), *plan, getnative::AnalysisAxes::vertical,
        metric, workspace);

    getnative::MetalAnalysisEngine metal;
    metal.reset_analysis_telemetry();
    const getnative::MetalLumaFrameView frame{
        reinterpret_cast<std::uintptr_t>(pixel_buffer), width, height, bit_depth,
        std::string{surface_format}, std::string{range},
    };
    const auto gpu = metal.analyze_axis_batch_metal_luma(frame, candidates, metric);
    expect(gpu.size() == 1U && std::isfinite(gpu.front().error),
           label + " analysis returns a finite metric");
    expect(std::abs(gpu.front().error - cpu) <= std::max(1e-7, 5e-4 * std::abs(cpu)),
           label + " metric matches the CPU oracle");
    const auto telemetry = metal.runtime_telemetry();
    expect(telemetry.external_source_zero_copy
               && telemetry.source_direct_write_bytes == 0U
               && telemetry.source_legacy_copy_bytes == 0U
               && telemetry.plan_direct_write_bytes > 0U,
           label + " performs no host source write or legacy copy");
}

void test_iosurface_zero_copy_luma() {
    test_iosurface_zero_copy_luma_format(
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 8, "420f", "full");
    test_iosurface_zero_copy_luma_format(
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 8, "420v", "limited");
    test_iosurface_zero_copy_luma_format(
        kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, 10, "xf20", "full");
    test_iosurface_zero_copy_luma_format(
        kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, 10, "x420", "limited");
}

[[nodiscard]] getnative::CandidateAnalysis make_dual_candidate(
    getnative::AxisPlanCache &cache, std::string id, std::int32_t source_width,
    std::int32_t source_height, std::int32_t native_width, std::int32_t native_height,
    double active_width, double active_height, const getnative::Filter &filter) {
    return {
        std::move(id),
        cache.get_or_build({source_width, native_width, active_width, 0.125,
                            filter, getnative::BorderMode::mirror}),
        cache.get_or_build({source_height, native_height, active_height, -0.0625,
                            filter, getnative::BorderMode::mirror}),
        getnative::AnalysisAxes::both,
    };
}

template <class Engine>
[[nodiscard]] std::optional<std::size_t> reported_working_set_bytes(const Engine &engine) {
    if constexpr (requires { engine.peak_working_set_bytes(); }) {
        return engine.peak_working_set_bytes();
    } else {
        return std::nullopt;
    }
}

[[nodiscard]] std::size_t plan_storage_bytes(const getnative::AxisPlan &plan) {
    return sizeof(plan)
        + plan.forward_offsets.size() * sizeof(std::uint32_t)
        + plan.forward_indices.size() * sizeof(std::int32_t)
        + plan.forward_weights.size() * sizeof(float)
        + plan.transpose_offsets.size() * sizeof(std::uint32_t)
        + plan.transpose_indices.size() * sizeof(std::int32_t)
        + plan.transpose_weights.size() * sizeof(float)
        + plan.lower_ld.size() * sizeof(float)
        + plan.upper_l.size() * sizeof(float)
        + plan.inverse_diagonal.size() * sizeof(float);
}

void test_dual_axis_filter_families_and_forward_orders() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    const std::vector<std::pair<std::string_view, getnative::Filter>> filters{
        {"bilinear", getnative::Filter::bilinear()},
        {"bicubic", getnative::Filter::bicubic(0.25, 0.4)},
        {"lanczos", getnative::Filter::lanczos(3)},
        {"spline16", getnative::Filter::spline16()},
        {"spline36", getnative::Filter::spline36()},
        {"spline64", getnative::Filter::spline64()},
        {"spline64-blur125", getnative::Filter::spline64(1.25)},
        {"spline64-blur150", getnative::Filter::spline64(1.5)},
    };
    getnative::AxisPlanCache cache;
    getnative::MetalAnalysisEngine metal({4, 8, 0});
    for (const auto &[name, filter] : filters) {
        std::vector<getnative::CandidateAnalysis> horizontal_first;
        std::vector<getnative::CandidateAnalysis> vertical_first;
        for (std::int32_t index = 0; index < 7; ++index) {
            horizontal_first.push_back(make_dual_candidate(
                cache, std::string{name} + "-hf-" + std::to_string(index), width, height,
                88 + index, 38 + index, 88.25 + index, 38.125 + index, filter));
            expect(getnative::select_forward_order(*horizontal_first.back().horizontal,
                                                   *horizontal_first.back().vertical)
                       == getnative::ForwardOrder::horizontal_first,
                   "dual-axis case selects horizontal-first reconstruction");

            vertical_first.push_back(make_dual_candidate(
                cache, std::string{name} + "-vf-" + std::to_string(index), width, height,
                44 + index, 72 + index, 44.25 + index, 72.125 + index, filter));
            expect(getnative::select_forward_order(*vertical_first.back().horizontal,
                                                   *vertical_first.back().vertical)
                       == getnative::ForwardOrder::vertical_first,
                   "dual-axis case selects vertical-first reconstruction");
        }
        compare_cpu_metal(view, horizontal_first, metric, metal,
                          std::string{name} + " dual horizontal-first");
        compare_cpu_metal(view, vertical_first, metric, metal,
                          std::string{name} + " dual vertical-first");
    }
}

void test_dual_axis_mixed_shapes_retain_order() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    getnative::AxisPlanCache cache;
    const std::vector<std::pair<std::string_view, getnative::Filter>> filters{
        {"b", getnative::Filter::bilinear()},
        {"l8", getnative::Filter::lanczos(8)},
        {"bc", getnative::Filter::bicubic(0.25, 0.4)},
        {"s36", getnative::Filter::spline36()},
        {"s16", getnative::Filter::spline16()},
        {"s64", getnative::Filter::spline64()},
    };
    std::vector<getnative::CandidateAnalysis> candidates;
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const auto &[name, filter] = filters[index];
        const bool horizontal_first = index % 2U == 0U;
        candidates.push_back(make_dual_candidate(
            cache, std::string{name} + "-mixed-" + std::to_string(index), width, height,
            horizontal_first ? 90 : 46, horizontal_first ? 40 : 74,
            horizontal_first ? 90.25 : 46.25, horizontal_first ? 40.125 : 74.125,
            filter));
    }
    getnative::MetalAnalysisEngine metal({2, 4, 0});
    compare_cpu_metal(view, candidates, getnative::MetricSpec{5, 5, 5, 5, 0.015F, 1U},
                      metal, "interleaved dual-axis mixed shapes");
}

void test_dual_axis_cancellation_and_complete_working_set() {
    constexpr std::int32_t width = 160;
    constexpr std::int32_t height = 120;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    getnative::AxisPlanCache cache;
    std::vector<getnative::CandidateAnalysis> candidates;
    const std::vector<getnative::Filter> filters{
        getnative::Filter::bilinear(), getnative::Filter::bicubic(),
        getnative::Filter::spline36(), getnative::Filter::lanczos(8),
    };
    for (std::size_t index = 0; index < filters.size(); ++index) {
        candidates.push_back(make_dual_candidate(
            cache, "working-set-" + std::to_string(index), width, height,
            96 + static_cast<std::int32_t>(index), 72 + static_cast<std::int32_t>(index),
            96.25 + static_cast<double>(index), 72.125 + static_cast<double>(index),
            filters[index]));
    }
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::MetalAnalysisEngine metal({4, 8, 0});

    std::stop_source stopped;
    stopped.request_stop();
    expect_throws([&] {
        (void)metal.analyze_axis_batch_f32(view, candidates, metric, stopped.get_token());
    }, "Metal observes cancellation before dual-axis submission");

    compare_cpu_metal(view, candidates, metric, metal, "dual-axis working-set batch");
    const auto working_set = reported_working_set_bytes(metal);
    expect(working_set.has_value(),
           "Metal public API reports complete peak working-set bytes");
    std::size_t accounted_minimum = source.size() * sizeof(float)
        + candidates.size() * 8U * sizeof(float)
        + candidates.size() * sizeof(getnative::CandidateResult);
    for (const auto &candidate : candidates) {
        accounted_minimum += plan_storage_bytes(*candidate.horizontal);
        accounted_minimum += plan_storage_bytes(*candidate.vertical);
    }
    expect(*working_set >= accounted_minimum,
           "working-set accounting includes source, plans, partials, and result staging");
    expect(*working_set < 2ULL * 1024ULL * 1024ULL * 1024ULL,
           "complete Metal working set remains below two GiB");
}

void test_vertical_and_horizontal_batches() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 72;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    getnative::MetalAnalysisEngine metal({4, 8, 0});

    std::vector<getnative::CandidateAnalysis> vertical;
    for (std::int32_t index = 0; index < 9; ++index) {
        const std::int32_t native = 42 + index;
        vertical.push_back({
            "v" + std::to_string(index), nullptr,
            cache.get_or_build({height, native, static_cast<double>(native) + 0.125,
                                -0.0625, getnative::Filter::bicubic(),
                                getnative::BorderMode::mirror}),
            getnative::AnalysisAxes::vertical,
        });
    }
    compare_cpu_metal(view, vertical, metric, metal, "bicubic vertical");

    std::vector<getnative::CandidateAnalysis> horizontal;
    for (std::int32_t index = 0; index < 7; ++index) {
        const std::int32_t native = 58 + index;
        horizontal.push_back({
            "h" + std::to_string(index),
            cache.get_or_build({width, native, static_cast<double>(native) + 0.25,
                                0.125, getnative::Filter::bicubic(),
                                getnative::BorderMode::mirror}),
            nullptr, getnative::AnalysisAxes::horizontal,
        });
    }
    compare_cpu_metal(view, horizontal, metric, metal, "bicubic horizontal");
    expect(metal.peak_workspace_elements() > 0, "Metal reports bounded tile workspace");
}

void test_kernel_dispatch_policy_and_telemetry() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    const auto bilinear = cache.get_or_build({
        height, 52, 52.125, -0.0625, getnative::Filter::bilinear(),
        getnative::BorderMode::mirror,
    });
    const auto bicubic = cache.get_or_build({
        height, 53, 53.125, -0.0625, getnative::Filter::bicubic(0.25, 0.4),
        getnative::BorderMode::mirror,
    });
    const auto spline36 = cache.get_or_build({
        height, 54, 54.125, -0.0625, getnative::Filter::spline36(),
        getnative::BorderMode::mirror,
    });
    const auto spline64 = cache.get_or_build({
        height, 55, 55.125, -0.0625, getnative::Filter::spline64(),
        getnative::BorderMode::mirror,
    });
    const auto lanczos6 = cache.get_or_build({
        height, 56, 56.125, -0.0625, getnative::Filter::lanczos(6),
        getnative::BorderMode::mirror,
    });
    const std::vector<getnative::CandidateAnalysis> fixed_candidates{
        {"b3", nullptr, bilinear, getnative::AnalysisAxes::vertical},
        {"b7", nullptr, bicubic, getnative::AnalysisAxes::vertical},
    };

    getnative::MetalAnalysisEngine generic({
        4, 8, 0, false, 32, getnative::MetalKernelDispatchPolicy::generic_only,
    });
    compare_cpu_metal(view, fixed_candidates, metric, generic, "forced generic controls");
    const auto generic_telemetry = generic.runtime_telemetry();
    expect(generic_telemetry.created_pipeline_names.size() == 5,
           "generic-only engine creates only the five generic stages");
    expect(generic_telemetry.generic_tile_count
                   == generic_telemetry.analyzed_tile_count
               && generic_telemetry.generic_tile_count > 0
               && generic_telemetry.specialized_tile_count == 0,
           "generic-only engine reports generic tile dispatch");
    expect(generic_telemetry.buffer_allocation_count > 0
               && generic_telemetry.buffer_allocation_bytes > 0,
           "Metal runtime reports explicit buffer allocations");
    generic.reset_analysis_telemetry();
    const auto reset_telemetry = generic.runtime_telemetry();
    expect(reset_telemetry.buffer_allocation_count == 0
               && reset_telemetry.analyzed_tile_count == 0,
           "analysis telemetry reset clears per-run counters");
    expect(reset_telemetry.created_pipeline_names.size() == 5
               && reset_telemetry.pipeline_creation_ms > 0.0,
           "analysis telemetry reset preserves pipeline creation evidence");

    getnative::MetalAnalysisEngine required({
        4, 8, 0, false, 32,
        getnative::MetalKernelDispatchPolicy::required_specialized,
    });
    compare_cpu_metal(view, fixed_candidates, metric, required,
                      "required specialized controls");
    const auto required_telemetry = required.runtime_telemetry();
    expect(required_telemetry.created_pipeline_names.size() == 15,
           "specialized engine reports every eager control pipeline");
    expect(required_telemetry.specialized_tile_count == 2
               && required_telemetry.generic_tile_count == 0,
           "required-specialized engine reports specialized tile dispatch");
    const std::vector<getnative::CandidateAnalysis> wider_fixed_candidates{
        {"b11", nullptr, spline36, getnative::AnalysisAxes::vertical},
        {"b15", nullptr, spline64, getnative::AnalysisAxes::vertical},
    };
    compare_cpu_metal(view, wider_fixed_candidates, metric, required,
                      "required specialized wider fixed shapes");
    const auto wider_telemetry = required.runtime_telemetry();
    expect(wider_telemetry.created_pipeline_names.size() == 19,
           "single-axis B11/B15 use creates only inverse and metric pipelines");
    for (const std::string_view name : {
             "inverse_axis_b11", "metric_axis_b11",
             "inverse_axis_b15", "metric_axis_b15"}) {
        expect(std::find(wider_telemetry.created_pipeline_names.begin(),
                         wider_telemetry.created_pipeline_names.end(), name)
                   != wider_telemetry.created_pipeline_names.end(),
               "wider fixed single-axis stage is created lazily");
    }
    for (const std::string_view name : {
             "inverse_axis_matrix_b11", "forward_axis_matrix_b11",
             "metric_axis_horizontal_first_b11", "inverse_axis_matrix_b15",
             "forward_axis_matrix_b15", "metric_axis_horizontal_first_b15"}) {
        expect(std::find(wider_telemetry.created_pipeline_names.begin(),
                         wider_telemetry.created_pipeline_names.end(), name)
                   == wider_telemetry.created_pipeline_names.end(),
               "unused wider fixed two-axis stage remains uncreated");
    }
    const std::vector<getnative::CandidateAnalysis> removed_function_constant_shape{
        {"b23", nullptr, lanczos6, getnative::AnalysisAxes::vertical},
    };
    expect_throws<std::runtime_error>(
        [&] {
            (void)required.analyze_axis_batch_f32(
                view, removed_function_constant_shape, metric);
        },
        "required-specialized policy rejects removed function-constant shapes");
    getnative::MetalAnalysisEngine automatic;
    compare_cpu_metal(view, removed_function_constant_shape, metric, automatic,
                      "automatic generic fallback for removed function constants");
    const auto automatic_telemetry = automatic.runtime_telemetry();
    expect(automatic_telemetry.generic_tile_count == 1
               && automatic_telemetry.specialized_tile_count == 0,
           "automatic policy reports generic fallback for a removed shape");
    auto unsupported_shape = std::make_shared<getnative::AxisPlan>(*lanczos6);
    unsupported_shape->half_bandwidth = 9;
    const std::size_t unsupported_factor_count = 9U
        * static_cast<std::size_t>(unsupported_shape->destination_size);
    unsupported_shape->lower_ld.resize(unsupported_factor_count);
    unsupported_shape->upper_l.resize(unsupported_factor_count);
    expect(unsupported_shape->valid(), "test constructs a valid unsupported exact shape");
    const std::vector<getnative::CandidateAnalysis> unsupported{
        {"b19-f12", nullptr, std::move(unsupported_shape),
         getnative::AnalysisAxes::vertical},
    };
    expect_throws<std::runtime_error>(
        [&] { (void)required.analyze_axis_batch_f32(view, unsupported, metric); },
        "required-specialized policy rejects an unavailable exact shape");
}

void test_all_supported_plan_shapes_horizontal_and_vertical() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    const std::vector<std::pair<std::string_view, getnative::Filter>> filters{
        {"bilinear-1-2", getnative::Filter::bilinear()},
        {"bicubic-3-4", getnative::Filter::bicubic(0.25, 0.4)},
        {"spline36-5-6", getnative::Filter::spline36()},
        {"spline64-7-8", getnative::Filter::spline64()},
        {"lanczos5-9-10", getnative::Filter::lanczos(5)},
        {"lanczos6-11-12", getnative::Filter::lanczos(6)},
        {"lanczos7-13-14", getnative::Filter::lanczos(7)},
        {"lanczos8-15-16", getnative::Filter::lanczos(8)},
    };
    getnative::AxisPlanCache cache;
    getnative::MetalAnalysisEngine metal({4, 8, 0});
    for (const auto &[name, filter] : filters) {
        const std::int32_t support = filter.support();
        std::vector<getnative::CandidateAnalysis> vertical;
        std::vector<getnative::CandidateAnalysis> horizontal;
        for (std::int32_t index = 0; index < 7; ++index) {
            const std::int32_t vertical_native = 48 + index;
            auto vertical_plan = cache.get_or_build({
                height, vertical_native, static_cast<double>(vertical_native) + 0.1875,
                -0.0625, filter, getnative::BorderMode::mirror,
            });
            expect(vertical_plan->half_bandwidth == 2 * support - 1
                       && vertical_plan->forward_width == 2 * support,
                   "vertical plan exposes the expected factor and forward widths");
            vertical.push_back({std::string{name} + "-v" + std::to_string(index), nullptr,
                                std::move(vertical_plan), getnative::AnalysisAxes::vertical});

            const std::int32_t horizontal_native = 58 + index;
            auto horizontal_plan = cache.get_or_build({
                width, horizontal_native, static_cast<double>(horizontal_native) + 0.3125,
                0.125, filter, getnative::BorderMode::mirror,
            });
            expect(horizontal_plan->half_bandwidth == 2 * support - 1
                       && horizontal_plan->forward_width == 2 * support,
                   "horizontal plan exposes the expected factor and forward widths");
            horizontal.push_back({std::string{name} + "-h" + std::to_string(index),
                                  std::move(horizontal_plan), nullptr,
                                  getnative::AnalysisAxes::horizontal});
        }
        compare_cpu_metal(view, vertical, metric, metal,
                          std::string{name} + " vertical");
        compare_cpu_metal(view, horizontal, metric, metal,
                          std::string{name} + " horizontal");
    }
}

void test_validation_and_cancellation() {
    constexpr std::int32_t width = 32;
    constexpr std::int32_t height = 24;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    auto plan = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        height, 16, 16.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"first", nullptr, plan, getnative::AnalysisAxes::vertical},
        {"second", nullptr, plan, getnative::AnalysisAxes::vertical},
    };
    auto horizontal_plan = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            width, 20, 20.25, 0.125, getnative::Filter::bicubic(),
            getnative::BorderMode::mirror,
        }));
    const std::vector<getnative::CandidateAnalysis> p_norm_candidates{
        {"horizontal", horizontal_plan, nullptr, getnative::AnalysisAxes::horizontal},
        {"vertical", nullptr, plan, getnative::AnalysisAxes::vertical},
        {"both", horizontal_plan, plan, getnative::AnalysisAxes::both},
    };
    getnative::MetalAnalysisEngine metal;
    float oversized_pixel = 0.0F;
    const getnative::ConstImageView oversized_source{
        &oversized_pixel,
        std::numeric_limits<std::int32_t>::max(),
        3,
        std::numeric_limits<std::int32_t>::max(),
    };
    expect_throws<std::length_error>(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                oversized_source, candidates,
                getnative::MetricSpec{0, 0, 0, 0, 0.015F, 1U});
        },
        "Metal rejects source images that exceed the shader's 32-bit element index range");

    auto unsupported_shape = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            height, 20, 20.0, 0.0, getnative::Filter::lanczos(9),
            getnative::BorderMode::mirror,
        }));
    const std::vector<getnative::CandidateAnalysis> unsupported_candidates{
        {"lanczos9", nullptr, unsupported_shape, getnative::AnalysisAxes::vertical},
    };
    expect_throws<std::invalid_argument>(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                view, unsupported_candidates,
                getnative::MetricSpec{2, 2, 2, 2, 0.015F, 1U});
        },
        "Metal rejects plans beyond half-bandwidth 15 and forward width 16");

    expect_throws(
        [] {
            const auto overflowing_groups =
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) / 256U;
            getnative::MetalAnalysisEngine invalid({1, overflowing_groups, 0});
        },
        "Metal rejects reduction schedules that can overflow shader indices");
    for (const std::uint32_t norm : {1U, 2U, 3U, 4U}) {
        compare_cpu_metal(
            view, p_norm_candidates,
            getnative::MetricSpec{2, 2, 2, 2, 0.015F, norm}, metal,
            std::string{"Metal p-norm "} + std::to_string(norm));
    }
    expect_throws(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                view, candidates, getnative::MetricSpec{2, 2, 2, 2, 0.015F, 5U});
        },
        "Metal rejects p-norms above four");

    for (const float threshold : {std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()}) {
        expect_throws(
            [&] {
                (void)metal.analyze_axis_batch_f32(
                    view, candidates, getnative::MetricSpec{2, 2, 2, 2, threshold, 1U});
            },
            "Metal rejects nonfinite metric thresholds");
    }

    constexpr auto maximum_crop = std::numeric_limits<std::int32_t>::max();
    expect_throws(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                view, candidates,
                getnative::MetricSpec{maximum_crop, maximum_crop, 0, 0, 0.015F, 1U});
        },
        "Metal rejects horizontally overflowing crop sums");
    expect_throws(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                view, candidates,
                getnative::MetricSpec{0, 0, maximum_crop, maximum_crop, 0.015F, 1U});
        },
        "Metal rejects vertically overflowing crop sums");

    std::stop_source stop;
    stop.request_stop();
    expect_throws(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                view, candidates, getnative::MetricSpec{2, 2, 2, 2, 0.015F, 1U},
                stop.get_token());
        },
        "Metal observes cancellation before submitting a tile");

    getnative::MetalAnalysisEngine constrained({2, 2, 600});
    const auto constrained_results = constrained.analyze_axis_batch_f32(
        view, candidates, getnative::MetricSpec{2, 2, 2, 2, 0.015F, 1U});
    expect(constrained_results.size() == candidates.size(),
           "Metal adapts tile size to the workspace limit");
    expect(constrained.peak_workspace_elements() == 512,
           "adaptive tiling retains one candidate workspace at a time");

    getnative::MetalAnalysisEngine too_small({2, 2, 511});
    expect_throws(
        [&] {
            (void)too_small.analyze_axis_batch_f32(
                view, candidates, getnative::MetricSpec{2, 2, 2, 2, 0.015F, 1U});
        },
        "Metal rejects a candidate larger than the workspace limit");
}

void test_queued_tile_window() {
    constexpr std::int32_t width = 16;
    constexpr std::int32_t height = 12;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    auto plan = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        height, 8, 8.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(66);
    for (std::size_t index = 0; index < 66; ++index) {
        candidates.push_back({
            std::to_string(index), nullptr, plan, getnative::AnalysisAxes::vertical,
        });
    }

    getnative::MetalAnalysisEngine metal({2, 2, 0, true, 32});
    compare_cpu_metal(
        view, candidates, getnative::MetricSpec{2, 2, 2, 2, 0.015F, 1U}, metal,
        "queued bicubic vertical");
    const auto telemetry = metal.runtime_telemetry();
    expect(telemetry.plan_direct_write_bytes > 0U
               && telemetry.plan_legacy_copy_bytes == 0U
               && telemetry.source_direct_write_bytes
                   == static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                          * sizeof(float)
               && telemetry.source_legacy_copy_bytes == 0U,
           "default Metal path writes plan and source directly into shared buffers");
    expect(metal.peak_workspace_elements() == 256,
           "queued tiles reuse one bounded workspace across submission windows");
}

void test_persistent_working_buffer_reuse_and_ceiling() {
    constexpr std::int32_t width = 32;
    constexpr std::int32_t height = 24;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    auto plan = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        height, 16, 16.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    std::vector<getnative::CandidateAnalysis> small;
    std::vector<getnative::CandidateAnalysis> large;
    for (std::size_t index = 0; index < 4; ++index) {
        getnative::CandidateAnalysis candidate{
            std::to_string(index), nullptr, plan, getnative::AnalysisAxes::vertical,
        };
        large.push_back(candidate);
        if (index < 2U) small.push_back(std::move(candidate));
    }

    getnative::MetalAnalysisOptions persistent_options{
        8, 2, 0, false, 32, getnative::MetalKernelDispatchPolicy::automatic,
    };
    expect(getnative::MetalAnalysisOptions{}.retained_working_buffer_limit_bytes
               == 2ULL * 1024ULL * 1024ULL * 1024ULL,
           "default retained working-buffer ceiling is exactly two GiB");
    persistent_options.reuse_working_buffers = true;
    persistent_options.retained_working_buffer_limit_bytes = 1024U * 1024U;
    getnative::MetalAnalysisEngine persistent(persistent_options);

    const auto first = persistent.analyze_axis_batch_f32(view, small, metric);
    const auto first_telemetry = persistent.runtime_telemetry();
    expect(first_telemetry.working_buffer_allocation_count == 3
               && first_telemetry.working_buffer_reuse_count == 0,
           "persistent Metal working buffers allocate source, workspace, and partials once");
    expect(first_telemetry.working_buffer_retained_bytes
               == first_telemetry.working_buffer_active_bytes
               && first_telemetry.working_buffer_peak_retained_bytes
                   == first_telemetry.working_buffer_retained_bytes,
           "first persistent call reports exact active and retained working-buffer bytes");

    persistent.reset_analysis_telemetry();
    const auto grown = persistent.analyze_axis_batch_f32(view, large, metric);
    const auto grown_telemetry = persistent.runtime_telemetry();
    expect(grown_telemetry.working_buffer_allocation_count == 2
               && grown_telemetry.working_buffer_reuse_count == 1,
           "grow-to-fit replaces only workspace and partial buffers");
    expect(grown_telemetry.working_buffer_retained_bytes
               > first_telemetry.working_buffer_retained_bytes,
           "grow-to-fit increases retained capacity for the larger batch");

    persistent.reset_analysis_telemetry();
    const auto reused = persistent.analyze_axis_batch_f32(view, large, metric);
    const auto reused_telemetry = persistent.runtime_telemetry();
    expect(reused_telemetry.working_buffer_allocation_count == 0
               && reused_telemetry.working_buffer_allocation_bytes == 0
               && reused_telemetry.working_buffer_reuse_count == 3,
           "stable repeated call reuses all three Metal working buffers");
    expect(reused_telemetry.buffer_allocation_count
               == reused_telemetry.analyzed_tile_count,
           "stable repeated call allocates one packed plan arena per tile");
    expect(std::isfinite(reused_telemetry.buffer_allocation_ms)
               && std::isfinite(reused_telemetry.source_upload_ms)
               && std::isfinite(reused_telemetry.buffer_wiring_ms),
           "Metal runtime reports finite allocation, upload, and wiring timings");

    persistent.trim_working_buffers();
    expect(persistent.runtime_telemetry().working_buffer_retained_bytes == 0,
           "explicit trim releases all retained Metal working buffers");
    persistent.reset_analysis_telemetry();
    const auto after_trim = persistent.analyze_axis_batch_f32(view, large, metric);
    const auto after_trim_telemetry = persistent.runtime_telemetry();
    expect(after_trim_telemetry.working_buffer_allocation_count == 3
               && after_trim_telemetry.working_buffer_reuse_count == 0
               && after_trim_telemetry.working_buffer_retained_bytes > 0,
           "first call after trim reallocates and retains all working buffers");

    getnative::MetalAnalysisOptions transient_options = persistent_options;
    transient_options.reuse_working_buffers = false;
    getnative::MetalAnalysisEngine transient(transient_options);
    const auto transient_results = transient.analyze_axis_batch_f32(view, large, metric);
    const auto transient_telemetry = transient.runtime_telemetry();
    expect(transient_telemetry.working_buffer_allocation_count == 3
               && transient_telemetry.working_buffer_reuse_count == 0
               && transient_telemetry.working_buffer_retained_bytes == 0,
           "diagnostic transient path allocates and retains no working buffers");

    getnative::MetalAnalysisOptions legacy_options = persistent_options;
    legacy_options.direct_plan_pack = false;
    legacy_options.direct_source_write = false;
    getnative::MetalAnalysisEngine legacy(legacy_options);
    const auto legacy_results = legacy.analyze_axis_batch_f32(view, large, metric);
    const auto legacy_telemetry = legacy.runtime_telemetry();
    expect(legacy_telemetry.plan_direct_write_bytes == 0U
               && legacy_telemetry.plan_legacy_copy_bytes > 0U
               && legacy_telemetry.source_direct_write_bytes == 0U
               && legacy_telemetry.source_legacy_copy_bytes
                   == static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                          * sizeof(float),
           "legacy Metal adapter reports its compatibility copies explicitly");

    getnative::MetalAnalysisOptions constrained_options = persistent_options;
    constrained_options.retained_working_buffer_limit_bytes = 1;
    getnative::MetalAnalysisEngine constrained(constrained_options);
    const auto constrained_results = constrained.analyze_axis_batch_f32(
        view, large, metric);
    const auto constrained_telemetry = constrained.runtime_telemetry();
    expect(constrained_telemetry.working_buffer_allocation_count == 3
               && constrained_telemetry.working_buffer_reuse_count == 0
               && constrained_telemetry.working_buffer_retained_bytes == 0,
           "working-buffer request above the retained ceiling falls back to transient buffers");

    expect(first.size() == small.size() && grown.size() == large.size()
               && reused.size() == large.size() && after_trim.size() == large.size()
               && transient_results.size() == large.size()
               && legacy_results.size() == large.size()
               && constrained_results.size() == large.size(),
           "working-buffer paths return every candidate");
    for (std::size_t index = 0; index < large.size(); ++index) {
        const std::uint64_t expected = std::bit_cast<std::uint64_t>(grown[index].error);
        expect(std::bit_cast<std::uint64_t>(reused[index].error) == expected
                   && std::bit_cast<std::uint64_t>(after_trim[index].error) == expected
                   && std::bit_cast<std::uint64_t>(transient_results[index].error) == expected
                   && std::bit_cast<std::uint64_t>(legacy_results[index].error) == expected
                   && std::bit_cast<std::uint64_t>(constrained_results[index].error) == expected,
               "persistent, trimmed, transient, and ceiling-fallback metrics are bit-identical");
    }
}

void test_submitted_cancellation_drains_before_reuse() {
    constexpr std::int32_t width = 320;
    constexpr std::int32_t height = 240;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    auto plan = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        height, 160, 160.0, 0.0, getnative::Filter::lanczos(8),
        getnative::BorderMode::mirror,
    }));
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(1000);
    for (std::size_t index = 0; index < 1000; ++index) {
        candidates.push_back({
            std::to_string(index), nullptr, plan, getnative::AnalysisAxes::vertical,
        });
    }

    getnative::MetalAnalysisEngine metal({1, 2, 0, true, 32});
    std::stop_source stop;
    getnative::JoiningThread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stop.request_stop();
    });
    expect_throws<std::runtime_error>(
        [&] {
            (void)metal.analyze_axis_batch_f32(
                view, candidates, metric, stop.get_token());
        },
        "Metal observes cancellation after command submission");
    canceller.join();

    const auto cancelled = metal.runtime_telemetry();
    expect(cancelled.command_buffer_submission_count > 0,
           "cancellation test submits Metal work before requesting stop");
    expect(cancelled.command_buffer_completion_count
               == cancelled.command_buffer_submission_count,
           "cancellation drains every submitted command before returning");

    metal.reset_analysis_telemetry();
    const std::span<const getnative::CandidateAnalysis> one_candidate{
        candidates.data(), 1U,
    };
    const auto reused = metal.analyze_axis_batch_f32(view, one_candidate, metric);
    const auto reused_telemetry = metal.runtime_telemetry();
    expect(reused.size() == 1U && std::isfinite(reused.front().error),
           "engine remains usable immediately after submitted cancellation");
    expect(reused_telemetry.working_buffer_reuse_count == 3U
               && reused_telemetry.command_buffer_submission_count
                   == reused_telemetry.command_buffer_completion_count,
           "immediate reuse uses retained buffers only after submitted work is complete");
}

void test_horizontal_transpose_layout_matches_cpu() {
    constexpr std::int32_t width = 40;
    constexpr std::int32_t height = 24;
    std::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y * width + x)] =
                static_cast<float>((x + 1) * 17 + (y + 3) * 13);
        }
    }
    std::vector<float> transposed(pixels.size());
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            transposed[static_cast<std::size_t>(x * height + y)] =
                pixels[static_cast<std::size_t>(y * width + x)];
        }
    }
    expect(transposed[static_cast<std::size_t>(1 * height + 0)]
               == pixels[static_cast<std::size_t>(0 * width + 1)],
           "transpose_source layout is dest[x * height + y] = src[y * width + x]");
    auto plan = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        width, 28, 28.25, -0.125, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"h-transpose", plan, nullptr, getnative::AnalysisAxes::horizontal},
    };
    getnative::MetalAnalysisEngine metal;
    compare_cpu_metal(const_view(pixels, width, height), candidates,
                      {2, 2, 2, 2, 0.015F, 1U}, metal,
                      "horizontal transpose layout");
}

void test_concurrent_execution_slots() {
    constexpr std::int32_t width = 48;
    constexpr std::int32_t height = 32;
    const auto source = make_source(width, height);
    const auto view = const_view(source, width, height);
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    auto vertical = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        height, 24, 24.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    auto horizontal = std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        width, 36, 36.0, 0.0, getnative::Filter::bicubic(),
        getnative::BorderMode::mirror,
    }));
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"v-concurrent", nullptr, vertical, getnative::AnalysisAxes::vertical},
        {"h-concurrent", horizontal, nullptr, getnative::AnalysisAxes::horizontal},
    };

    getnative::MetalAnalysisOptions options;
    options.execution_slots = 2;
    getnative::MetalAnalysisEngine metal(options);
    std::stop_source stop;
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        for (int tick = 0; tick < 80 && !finished.load(std::memory_order_relaxed); ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!finished.load(std::memory_order_relaxed)) {
            stop.request_stop();
        }
    });

    std::exception_ptr failure;
    std::mutex failure_mutex;
    const auto run = [&] {
        try {
            const auto results = metal.analyze_axis_batch_f32(
                view, candidates, metric, stop.get_token());
            expect(results.size() == candidates.size(),
                   "concurrent Metal analysis returns one result per candidate");
            for (const auto &result : results) {
                expect(std::isfinite(result.error),
                       "concurrent Metal analysis returns a finite metric");
            }
        } catch (...) {
            const std::scoped_lock lock(failure_mutex);
            if (!failure) failure = std::current_exception();
        }
    };

    std::thread first(run);
    std::thread second(run);
    first.join();
    second.join();
    finished.store(true, std::memory_order_relaxed);
    watchdog.join();
    if (failure) std::rethrow_exception(failure);
    expect(!stop.get_token().stop_requested(),
           "two concurrent Metal slot analyses completed without hanging");
}

} // namespace

int main() {
    try {
        if (!getnative::metal_backend_available()) {
            std::cout << "metal conformance tests skipped: no Metal device\n";
            return 0;
        }
        test_vertical_and_horizontal_batches();
        test_horizontal_transpose_layout_matches_cpu();
        test_iosurface_zero_copy_luma();
        test_kernel_dispatch_policy_and_telemetry();
        test_all_supported_plan_shapes_horizontal_and_vertical();
        test_dual_axis_filter_families_and_forward_orders();
        test_dual_axis_mixed_shapes_retain_order();
        test_dual_axis_cancellation_and_complete_working_set();
        test_validation_and_cancellation();
        test_queued_tile_window();
        test_persistent_working_buffer_reuse_and_ceiling();
        test_submitted_cancellation_drains_before_reuse();
        test_concurrent_execution_slots();
        std::cout << "metal conformance tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "metal conformance failure: " << error.what() << '\n';
        return 1;
    }
}

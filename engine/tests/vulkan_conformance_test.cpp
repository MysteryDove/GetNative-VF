#include "getnative/vulkan_analysis.hpp"

#include "getnative/filter.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

struct SourceImage {
    std::vector<float> pixels;
    getnative::ConstImageView view{};
};

[[nodiscard]] SourceImage make_source(std::int32_t width, std::int32_t height,
                                      std::ptrdiff_t stride = 0) {
    if (stride == 0) stride = width;
    SourceImage result;
    result.pixels.assign(static_cast<std::size_t>(stride)
                             * static_cast<std::size_t>(height),
                         -17.0F);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            result.pixels[static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(y) * stride + x)] =
                static_cast<float>(
                    0.42 + 0.23 * std::sin(0.073 * static_cast<double>(x))
                    + 0.19 * std::cos(0.091 * static_cast<double>(y))
                    + 0.07 * std::sin(0.037 * static_cast<double>(x + y)));
        }
    }
    result.view = {result.pixels.data(), width, height, stride};
    return result;
}

[[nodiscard]] std::vector<double> cpu_metrics(
    getnative::ConstImageView source,
    std::span<const getnative::CandidateAnalysis> candidates,
    const getnative::MetricSpec &metric) {
    std::vector<double> result;
    result.reserve(candidates.size());
    for (const getnative::CandidateAnalysis &candidate : candidates) {
        getnative::CpuWorkspace workspace;
        if (candidate.axes == getnative::AnalysisAxes::both) {
            result.push_back(getnative::analyze_candidate_f32(
                source, *candidate.horizontal, *candidate.vertical,
                metric, workspace));
        } else {
            const auto &plan = candidate.axes == getnative::AnalysisAxes::horizontal
                ? candidate.horizontal : candidate.vertical;
            result.push_back(getnative::analyze_axis_candidate_f32(
                source, *plan, candidate.axes, metric, workspace));
        }
    }
    return result;
}

void compare_cpu_vulkan(
    getnative::ConstImageView source,
    const std::vector<getnative::CandidateAnalysis> &candidates,
    const getnative::MetricSpec &metric,
    getnative::VulkanAnalysisEngine &vulkan,
    std::string_view label) {
    const std::vector<double> cpu = cpu_metrics(source, candidates, metric);
    const auto gpu = vulkan.analyze_axis_batch_f32(source, candidates, metric);
    expect(gpu.size() == candidates.size(),
           "Vulkan returns one result per candidate");
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        expect(gpu[index].id == candidates[index].id,
               "Vulkan retains candidate order and ids");
        expect(std::isfinite(gpu[index].error), "Vulkan metric is finite");
        const double tolerance = std::max(1e-7, 5e-4 * std::abs(cpu[index]));
        if (std::abs(gpu[index].error - cpu[index]) > tolerance) {
            throw std::runtime_error(std::string{label}
                                     + " exceeds the strict CPU tolerance");
        }
    }
    if (!candidates.empty()) {
        const auto cpu_min = static_cast<std::size_t>(
            std::min_element(cpu.begin(), cpu.end()) - cpu.begin());
        const auto gpu_min = static_cast<std::size_t>(
            std::min_element(gpu.begin(), gpu.end(), [](const auto &lhs,
                                                        const auto &rhs) {
                return lhs.error < rhs.error;
            }) - gpu.begin());
        const std::size_t distance = cpu_min > gpu_min
            ? cpu_min - gpu_min : gpu_min - cpu_min;
        if (distance > 1) {
            throw std::runtime_error(std::string{label}
                                     + " minimum differs by more than one step");
        }
    }
}

[[nodiscard]] getnative::CandidateAnalysis make_dual_candidate(
    getnative::AxisPlanCache &cache, std::string id,
    std::int32_t source_width, std::int32_t source_height,
    std::int32_t native_width, std::int32_t native_height,
    const getnative::Filter &filter) {
    return {
        std::move(id),
        cache.get_or_build({source_width, native_width,
                            static_cast<double>(native_width) + 0.25, 0.125,
                            filter, getnative::BorderMode::mirror}),
        cache.get_or_build({source_height, native_height,
                            static_cast<double>(native_height) + 0.125, -0.0625,
                            filter, getnative::BorderMode::mirror}),
        getnative::AnalysisAxes::both,
    };
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> make_all_shape_axes(
    getnative::AxisPlanCache &cache, std::int32_t width, std::int32_t height) {
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
    std::vector<getnative::CandidateAnalysis> candidates;
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const auto &[name, filter] = filters[index];
        const std::int32_t support = filter.support();
        const std::int32_t vertical_native = 44 + static_cast<std::int32_t>(index);
        auto vertical = cache.get_or_build({
            height, vertical_native, static_cast<double>(vertical_native) + 0.1875,
            -0.0625, filter, getnative::BorderMode::mirror});
        expect(vertical->half_bandwidth == 2 * support - 1
                   && vertical->forward_width == 2 * support,
               "vertical plan exposes the requested supported shape");
        candidates.push_back({std::string{name} + "-v", nullptr,
                              std::move(vertical),
                              getnative::AnalysisAxes::vertical});

        const std::int32_t horizontal_native = 54 + static_cast<std::int32_t>(index);
        auto horizontal = cache.get_or_build({
            width, horizontal_native,
            static_cast<double>(horizontal_native) + 0.3125,
            0.125, filter, getnative::BorderMode::mirror});
        expect(horizontal->half_bandwidth == 2 * support - 1
                   && horizontal->forward_width == 2 * support,
               "horizontal plan exposes the requested supported shape");
        candidates.push_back({std::string{name} + "-h", std::move(horizontal),
                              nullptr, getnative::AnalysisAxes::horizontal});
    }
    return candidates;
}

void test_probe_and_selection(const getnative::VulkanBackendProbe &probe) {
    expect(probe.loader_available && probe.instance_available,
           "Vulkan probe reaches instance enumeration");
    std::set<std::string> selectors;
    for (const getnative::VulkanDeviceInfo &device : probe.devices) {
        expect(!device.name.empty() && !device.stable_selector.empty(),
               "Vulkan device has a name and stable selector");
        expect(selectors.insert(device.stable_selector).second,
               "Vulkan stable selectors are unique");
    }

    const auto first = std::find_if(probe.devices.begin(), probe.devices.end(),
        [](const getnative::VulkanDeviceInfo &device) {
            return device.production_compute_supported;
        });
    expect(first != probe.devices.end(),
           "probe exposes a production-capable Vulkan device");
    getnative::VulkanAnalysisOptions options;
    options.device_selector.uuid = first->uuid;
    getnative::VulkanAnalysisEngine selected(options);
    expect(selected.device_info().uuid == first->uuid,
           "UUID selector chooses the requested Vulkan device");
    expect(selected.device_info().compute_queue_family
               != std::numeric_limits<std::uint32_t>::max(),
           "selected Vulkan device has a compute queue");
}

void test_all_shapes_and_dispatch_identity() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const SourceImage source = make_source(width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    const auto candidates = make_all_shape_axes(cache, width, height);

    getnative::VulkanAnalysisEngine automatic;
    compare_cpu_vulkan(source.view, candidates, metric, automatic,
                       "all supported Vulkan shapes");
    expect(automatic.peak_workspace_elements() > 0,
           "Vulkan reports bounded tile workspace");
    const auto automatic_results = automatic.analyze_axis_batch_f32(
        source.view, candidates, metric);

    getnative::VulkanAnalysisOptions generic_options;
    generic_options.kernel_dispatch =
        getnative::VulkanKernelDispatchPolicy::generic_only;
    getnative::VulkanAnalysisEngine generic(generic_options);
    const auto generic_results = generic.analyze_axis_batch_f32(
        source.view, candidates, metric);
    expect(generic_results.size() == automatic_results.size(),
           "generic and automatic Vulkan paths return equal result counts");
    for (std::size_t index = 0; index < generic_results.size(); ++index) {
        expect(std::bit_cast<std::uint64_t>(generic_results[index].error)
                   == std::bit_cast<std::uint64_t>(automatic_results[index].error),
               "specialized and generic Vulkan results are bit-identical");
    }
    const auto generic_telemetry = generic.runtime_telemetry();
    expect(generic_telemetry.created_pipeline_names.size() == 5
               && generic_telemetry.generic_tile_count > 0
               && generic_telemetry.specialized_tile_count == 0,
           "generic-only Vulkan engine reports five generic stages");
    const auto automatic_telemetry = automatic.runtime_telemetry();
    for (const std::string_view name : {
             "inverse_axis_b3", "metric_axis_p1_b3",
             "inverse_axis_b7", "metric_axis_p1_b7",
             "inverse_axis_b11", "metric_axis_p1_b11",
             "inverse_axis_b15", "metric_axis_p1_b15"}) {
        expect(std::find(automatic_telemetry.created_pipeline_names.begin(),
                         automatic_telemetry.created_pipeline_names.end(), name)
                   != automatic_telemetry.created_pipeline_names.end(),
               "Vulkan fixed single-axis pipeline is created and reported");
    }
}

void test_dual_axes_orders_and_mixed_shapes() {
    constexpr std::int32_t width = 96;
    constexpr std::int32_t height = 80;
    const SourceImage source = make_source(width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    getnative::AxisPlanCache cache;
    const std::vector<getnative::Filter> filters{
        getnative::Filter::bilinear(),
        getnative::Filter::bicubic(0.25, 0.4),
        getnative::Filter::spline36(), getnative::Filter::spline64(),
        getnative::Filter::lanczos(8),
    };
    std::vector<getnative::CandidateAnalysis> candidates;
    for (std::size_t index = 0; index < filters.size(); ++index) {
        const bool horizontal_first = index % 2 == 0;
        candidates.push_back(make_dual_candidate(
            cache, "both-" + std::to_string(index), width, height,
            horizontal_first ? 88 : 44,
            horizontal_first ? 38 : 72, filters[index]));
        const getnative::ForwardOrder expected = horizontal_first
            ? getnative::ForwardOrder::horizontal_first
            : getnative::ForwardOrder::vertical_first;
        expect(getnative::select_forward_order(*candidates.back().horizontal,
                                               *candidates.back().vertical)
                   == expected,
               "dual-axis Vulkan case selects the requested forward order");
    }
    getnative::VulkanAnalysisOptions options;
    options.tile_size = 2;
    getnative::VulkanAnalysisEngine vulkan(options);
    compare_cpu_vulkan(source.view, candidates, metric, vulkan,
                       "mixed dual-axis Vulkan batch");
    const auto telemetry = vulkan.runtime_telemetry();
    for (const std::string_view name : {
             "inverse_axis_matrix_b3", "forward_axis_matrix_b3",
             "metric_axis_p1_horizontal_first_b3",
             "inverse_axis_matrix_b11", "forward_axis_matrix_b11",
             "metric_axis_p1_horizontal_first_b11",
             "inverse_axis_matrix_b15", "forward_axis_matrix_b15",
             "metric_axis_p1_b15"}) {
        if (std::find(telemetry.created_pipeline_names.begin(),
                      telemetry.created_pipeline_names.end(), name)
            == telemetry.created_pipeline_names.end()) {
            std::string message = "missing Vulkan two-axis pipeline ";
            message += name;
            message += "; created=";
            for (const std::string &created : telemetry.created_pipeline_names) {
                if (message.back() != '=') message += ',';
                message += created;
            }
            throw std::runtime_error(message);
        }
    }
}

void test_stride_validation_workspace_and_empty() {
    constexpr std::int32_t width = 48;
    constexpr std::int32_t height = 32;
    const SourceImage source = make_source(width, height, width + 7);
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    auto plan = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            height, 20, 20.125, -0.0625, getnative::Filter::bicubic(),
            getnative::BorderMode::mirror}));
    const std::vector<getnative::CandidateAnalysis> candidates{
        {"first", nullptr, plan, getnative::AnalysisAxes::vertical},
        {"second", nullptr, plan, getnative::AnalysisAxes::vertical},
    };
    getnative::VulkanAnalysisEngine vulkan;
    compare_cpu_vulkan(source.view, candidates, metric, vulkan,
                       "padded-stride Vulkan source");
    expect(vulkan.analyze_axis_batch_f32(source.view, {}, metric).empty(),
           "Vulkan accepts an empty candidate batch");
    expect_throws<std::invalid_argument>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                {nullptr, width, height, width}, candidates, metric);
        }, "Vulkan rejects a null source");
    expect_throws<std::invalid_argument>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                source.view, candidates,
                getnative::MetricSpec{2, 2, 2, 2, 0.015F, 2U});
        }, "Vulkan rejects p != 1");
    expect_throws<std::invalid_argument>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                source.view, candidates,
                getnative::MetricSpec{24, 24, 0, 0, 0.015F, 1U});
        }, "Vulkan rejects an empty crop");
    expect_throws<std::invalid_argument>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                source.view,
                std::vector<getnative::CandidateAnalysis>{{
                    "null", nullptr, nullptr,
                    getnative::AnalysisAxes::vertical}}, metric);
        }, "Vulkan rejects a null axis plan");
    const auto unsupported = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            height, 20, 20.0, 0.0, getnative::Filter::lanczos(9),
            getnative::BorderMode::mirror}));
    expect_throws<std::invalid_argument>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                source.view,
                std::vector<getnative::CandidateAnalysis>{{
                    "l9", nullptr, unsupported,
                    getnative::AnalysisAxes::vertical}}, metric);
        }, "Vulkan rejects shapes beyond B31/F16");
    std::stop_source stopped;
    stopped.request_stop();
    expect_throws<std::runtime_error>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                source.view, candidates, metric, stopped.get_token());
        }, "Vulkan observes cancellation before submission");

    const std::size_t candidate_workspace = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(plan->destination_size);
    getnative::VulkanAnalysisOptions constrained_options;
    constrained_options.tile_size = 2;
    constrained_options.workspace_limit_elements = candidate_workspace;
    getnative::VulkanAnalysisEngine constrained(constrained_options);
    const auto constrained_results = constrained.analyze_axis_batch_f32(
        source.view, candidates, metric);
    expect(constrained_results.size() == candidates.size()
               && constrained.peak_workspace_elements() == candidate_workspace,
           "Vulkan shrinks a tile to the workspace limit");
    constrained_options.workspace_limit_elements = candidate_workspace - 1;
    getnative::VulkanAnalysisEngine too_small(constrained_options);
    expect_throws<std::length_error>(
        [&] {
            (void)too_small.analyze_axis_batch_f32(
                source.view, candidates, metric);
        }, "Vulkan rejects one candidate above the workspace limit");
}

void test_buffer_reuse_trim_and_telemetry() {
    constexpr std::int32_t width = 48;
    constexpr std::int32_t height = 32;
    const SourceImage source = make_source(width, height);
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.015F, 1U};
    auto plan = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            height, 20, 20.125, -0.0625, getnative::Filter::bicubic(),
            getnative::BorderMode::mirror}));
    std::vector<getnative::CandidateAnalysis> candidates;
    for (std::size_t index = 0; index < 4; ++index) {
        candidates.push_back({std::to_string(index), nullptr, plan,
                              getnative::AnalysisAxes::vertical});
    }
    getnative::VulkanAnalysisOptions options;
    options.tile_size = 4;
    options.reduction_groups_per_candidate = 2;
    options.retained_working_buffer_limit_bytes = 16U * 1024U * 1024U;
    getnative::VulkanAnalysisEngine persistent(options);
    const auto first = persistent.analyze_axis_batch_f32(
        source.view, candidates, metric);
    const auto first_telemetry = persistent.runtime_telemetry();
    expect(first_telemetry.working_buffer_allocation_count == 7
               && first_telemetry.working_buffer_retained_bytes > 0,
           "first Vulkan call allocates and retains seven working buffers");
    expect(first_telemetry.queue_submission_count
               == first_telemetry.queue_completion_count,
           "Vulkan completes every submitted tile");
    expect(first_telemetry.peak_total_explicit_bytes <
               2ULL * 1024ULL * 1024ULL * 1024ULL,
           "Vulkan explicit working set remains below two GiB");

    persistent.reset_analysis_telemetry();
    const auto second = persistent.analyze_axis_batch_f32(
        source.view, candidates, metric);
    const auto reused = persistent.runtime_telemetry();
    expect(reused.working_buffer_allocation_count == 0
               && reused.working_buffer_reuse_count == 7,
           "stable repeated Vulkan call reuses all seven buffers");
    expect(reused.queue_submission_count == reused.queue_completion_count,
           "reused Vulkan call drains every submission");
    for (std::size_t index = 0; index < first.size(); ++index) {
        expect(std::bit_cast<std::uint64_t>(first[index].error)
                   == std::bit_cast<std::uint64_t>(second[index].error),
               "reused Vulkan metrics are bit-identical");
    }

    persistent.trim_working_buffers();
    expect(persistent.runtime_telemetry().working_buffer_retained_bytes == 0,
           "Vulkan trim releases retained buffers");
    persistent.reset_analysis_telemetry();
    const auto after_trim = persistent.analyze_axis_batch_f32(
        source.view, candidates, metric);
    expect(persistent.runtime_telemetry().working_buffer_allocation_count == 7,
           "Vulkan call after trim reallocates all working buffers");

    options.retained_working_buffer_limit_bytes = 1;
    getnative::VulkanAnalysisEngine transient(options);
    const auto transient_results = transient.analyze_axis_batch_f32(
        source.view, candidates, metric);
    expect(transient.runtime_telemetry().working_buffer_retained_bytes == 0,
           "Vulkan requests over the retained ceiling remain transient");
    for (std::size_t index = 0; index < first.size(); ++index) {
        const std::uint64_t expected = std::bit_cast<std::uint64_t>(first[index].error);
        expect(std::bit_cast<std::uint64_t>(after_trim[index].error) == expected
                   && std::bit_cast<std::uint64_t>(transient_results[index].error)
                       == expected,
               "trimmed and transient Vulkan metrics are bit-identical");
    }
}

void test_submitted_cancel_and_reuse() {
    constexpr std::int32_t width = 320;
    constexpr std::int32_t height = 240;
    const SourceImage source = make_source(width, height);
    const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
    auto plan = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan({
            height, 160, 160.0, 0.0, getnative::Filter::lanczos(8),
            getnative::BorderMode::mirror}));
    std::vector<getnative::CandidateAnalysis> candidates;
    candidates.reserve(1000);
    for (std::size_t index = 0; index < 1000; ++index) {
        candidates.push_back({std::to_string(index), nullptr, plan,
                              getnative::AnalysisAxes::vertical});
    }
    getnative::VulkanAnalysisOptions options;
    options.tile_size = 1;
    options.reduction_groups_per_candidate = 2;
    getnative::VulkanAnalysisEngine vulkan(options);
    std::stop_source pre_stopped;
    pre_stopped.request_stop();
    expect_throws<std::runtime_error>(
        [&] {
            (void)vulkan.analyze_axis_batch_f32(
                source.view, candidates, metric, pre_stopped.get_token());
        }, "Vulkan observes cancellation before tile planning or submission");
    expect(vulkan.runtime_telemetry().queue_submission_count == 0,
           "pre-cancelled Vulkan call submits no queue work");

    bool observed_submitted_cancellation = false;
    for (const auto delay : {
             std::chrono::milliseconds(10), std::chrono::milliseconds(25),
             std::chrono::milliseconds(50), std::chrono::milliseconds(75),
             std::chrono::milliseconds(100), std::chrono::milliseconds(150),
             std::chrono::milliseconds(250), std::chrono::milliseconds(400),
             std::chrono::milliseconds(650), std::chrono::milliseconds(1000),
         }) {
        vulkan.reset_analysis_telemetry();
        std::stop_source stop;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(delay);
            stop.request_stop();
        });
        bool cancelled = false;
        try {
            (void)vulkan.analyze_axis_batch_f32(
                source.view, candidates, metric, stop.get_token());
        } catch (const std::runtime_error &) {
            cancelled = true;
        }
        canceller.join();
        const auto telemetry = vulkan.runtime_telemetry();
        if (telemetry.queue_submission_count == 0) {
            expect(cancelled, "pre-submission Vulkan cancellation is observed");
            continue;
        }
        expect(cancelled, "Vulkan observes cancellation after queue submission");
        expect(telemetry.queue_submission_count == telemetry.queue_completion_count,
               "Vulkan cancellation drains every submitted queue sequence");
        observed_submitted_cancellation = true;
        break;
    }
    expect(observed_submitted_cancellation,
           "submitted-cancellation case reaches a Vulkan queue submission");

    vulkan.reset_analysis_telemetry();
    const auto one = vulkan.analyze_axis_batch_f32(
        source.view, std::span<const getnative::CandidateAnalysis>{
                         candidates.data(), 1}, metric);
    expect(one.size() == 1 && std::isfinite(one.front().error),
           "Vulkan engine is reusable immediately after cancellation");
}

void test_validation_layer_if_available(
    const getnative::VulkanBackendProbe &validation_probe) {
    const auto device = std::find_if(
        validation_probe.devices.begin(), validation_probe.devices.end(),
        [](const getnative::VulkanDeviceInfo &value) {
            return value.production_compute_supported;
        });
    if (!validation_probe.instance_available
        || device == validation_probe.devices.end()) {
        return;
    }
    getnative::VulkanAnalysisOptions options;
    options.enable_validation = true;
    options.device_selector.uuid = device->uuid;
    getnative::VulkanAnalysisEngine vulkan(options);
    const SourceImage source = make_source(48, 32);
    getnative::AxisPlanCache cache;
    const std::vector<getnative::CandidateAnalysis> candidates{{
        "validation", nullptr,
        cache.get_or_build({32, 20, 20.125, -0.0625,
                            getnative::Filter::bicubic(),
                            getnative::BorderMode::mirror}),
        getnative::AnalysisAxes::vertical}};
    (void)vulkan.analyze_axis_batch_f32(
        source.view, candidates,
        getnative::MetricSpec{2, 2, 2, 2, 0.015F, 1U});
    expect(vulkan.runtime_telemetry().validation_error_count == 0,
           "Vulkan validation layer reports no errors");
}

} // namespace

int main() {
    try {
        const auto missing = getnative::probe_vulkan_backend(
            L"getnative-vulkan-loader-that-does-not-exist.dll");
        expect(!missing.loader_available && !missing.unavailable_reason.empty(),
               "missing Vulkan loader has a stable non-throwing probe result");

        const getnative::VulkanBackendProbe probe =
            getnative::probe_vulkan_backend();
        const bool has_device = std::ranges::any_of(
            probe.devices, [](const getnative::VulkanDeviceInfo &device) {
                return device.production_compute_supported;
            });
        if (!has_device) {
            std::cout << "Vulkan conformance skipped: "
                      << probe.unavailable_reason << '\n';
            return 0;
        }

        test_probe_and_selection(probe);
        test_all_shapes_and_dispatch_identity();
        test_dual_axes_orders_and_mixed_shapes();
        test_stride_validation_workspace_and_empty();
        test_buffer_reuse_trim_and_telemetry();
        test_submitted_cancel_and_reuse();
        test_validation_layer_if_available(
            getnative::probe_vulkan_backend(L"vulkan-1.dll", true));
        std::cout << "Vulkan conformance tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Vulkan conformance tests failed: " << error.what() << '\n';
        return 1;
    }
}

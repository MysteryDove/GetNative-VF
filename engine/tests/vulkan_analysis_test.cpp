#include "getnative/vulkan_analysis.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
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

void test_default_device_selection() {
    using getnative::VulkanDeviceInfo;
    using getnative::VulkanDeviceType;
    std::vector<VulkanDeviceInfo> devices(3U);
    devices[0].index = 0;
    devices[0].device_type = VulkanDeviceType::integrated_gpu;
    devices[0].device_local_memory_bytes = 8U;
    devices[0].backend_compatible = true;
    devices[1].index = 1;
    devices[1].device_type = VulkanDeviceType::discrete_gpu;
    devices[1].device_local_memory_bytes = 4U;
    devices[1].backend_compatible = true;
    devices[2].index = 2;
    devices[2].device_type = VulkanDeviceType::discrete_gpu;
    devices[2].device_local_memory_bytes = 16U;
    devices[2].backend_compatible = false;
    expect(getnative::select_default_vulkan_device_index(devices) == 1,
           "a compatible discrete GPU must outrank an earlier integrated GPU");

    devices[1].backend_compatible = false;
    expect(getnative::select_default_vulkan_device_index(devices) == 0,
           "an integrated GPU remains available for explicit/default Vulkan");
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

[[nodiscard]] const getnative::VulkanDeviceInfo &compatible_device(
    const getnative::VulkanRuntimeProbe &probe) {
    const auto selected = std::find_if(
        probe.devices.begin(), probe.devices.end(), [](const auto &device) {
            return device.backend_compatible;
        });
    if (selected == probe.devices.end()) {
        throw std::runtime_error("Vulkan probe exposed no compatible device");
    }
    return *selected;
}

[[nodiscard]] std::shared_ptr<const getnative::AxisPlan> make_plan(
    std::int32_t source_size, std::int32_t destination_size,
    double active_length, double shift, const getnative::Filter &filter) {
    return std::make_shared<const getnative::AxisPlan>(getnative::build_axis_plan({
        source_size, destination_size, active_length, shift, filter,
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
        static_cast<std::size_t>(height) * static_cast<std::size_t>(stride),
        -19.0F);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            result.storage[static_cast<std::size_t>(y)
                               * static_cast<std::size_t>(stride)
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
    const auto horizontal = make_plan(
        width, 25, 25.25, -0.125, getnative::Filter::lanczos(8));
    const auto vertical = make_plan(
        height, 19, 19.125, 0.125, getnative::Filter::spline36());
    const auto horizontal_first_x = make_plan(
        width, 35, 35.0, 0.0, getnative::Filter::bilinear());
    const auto horizontal_first_y = make_plan(
        height, 13, 13.0, 0.0, getnative::Filter::bicubic());
    const auto vertical_first_x = make_plan(
        width, 17, 17.0, 0.0, getnative::Filter::lanczos(3));
    const auto vertical_first_y = make_plan(
        height, 27, 27.0, 0.0, getnative::Filter::spline64());
    expect(getnative::select_forward_order(
               *horizontal_first_x, *horizontal_first_y)
               == getnative::ForwardOrder::horizontal_first,
           "fixture must select horizontal-first reconstruction");
    expect(getnative::select_forward_order(
               *vertical_first_x, *vertical_first_y)
               == getnative::ForwardOrder::vertical_first,
           "fixture must select vertical-first reconstruction");
    return {
        {"horizontal-lanczos8", horizontal, nullptr,
         getnative::AnalysisAxes::horizontal},
        {"vertical-spline36", nullptr, vertical,
         getnative::AnalysisAxes::vertical},
        {"both-horizontal-first", horizontal_first_x, horizontal_first_y,
         getnative::AnalysisAxes::both},
        {"both-vertical-first", vertical_first_x, vertical_first_y,
         getnative::AnalysisAxes::both},
    };
}

[[nodiscard]] std::shared_ptr<const getnative::AxisPlan> make_exact_gain_plan(
    std::int32_t size) {
    getnative::AxisPlan plan;
    plan.source_size = size;
    plan.destination_size = size;
    plan.support = 1;
    plan.half_bandwidth = 0;
    plan.forward_width = 1;
    plan.active_length = static_cast<double>(size);
    plan.forward_offsets.resize(static_cast<std::size_t>(size) + 1U);
    plan.forward_indices.resize(static_cast<std::size_t>(size));
    plan.forward_weights.assign(static_cast<std::size_t>(size), 0.5F);
    plan.transpose_offsets.resize(static_cast<std::size_t>(size) + 1U);
    plan.transpose_indices.resize(static_cast<std::size_t>(size));
    plan.transpose_weights.assign(static_cast<std::size_t>(size), 1.0F);
    plan.inverse_diagonal.assign(static_cast<std::size_t>(size), 1.0F);
    for (std::int32_t index = 0; index < size; ++index) {
        plan.forward_offsets[static_cast<std::size_t>(index)] =
            static_cast<std::uint32_t>(index);
        plan.forward_indices[static_cast<std::size_t>(index)] = index;
        plan.transpose_offsets[static_cast<std::size_t>(index)] =
            static_cast<std::uint32_t>(index);
        plan.transpose_indices[static_cast<std::size_t>(index)] = index;
    }
    plan.forward_offsets.back() = static_cast<std::uint32_t>(size);
    plan.transpose_offsets.back() = static_cast<std::uint32_t>(size);
    expect(plan.valid(), "exact threshold fixture plan is valid");
    return std::make_shared<const getnative::AxisPlan>(std::move(plan));
}

void compare_with_cpu(
    getnative::VulkanAnalysisEngine &engine, getnative::ConstImageView source,
    const std::vector<getnative::CandidateAnalysis> &candidates,
    const getnative::MetricSpec &metric, std::string_view label) {
    std::vector<double> expected;
    expected.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        expected.push_back(cpu_metric(source, candidate, metric));
    }
    const auto actual = engine.analyze_axis_batch_f32(source, candidates, metric);
    expect(actual.size() == candidates.size(), "Vulkan result count");
    for (std::size_t index = 0U; index < actual.size(); ++index) {
        const double tolerance = std::max(
            2e-7, 5e-4 * std::abs(expected[index]));
        if (actual[index].id != candidates[index].id
            || !std::isfinite(actual[index].error)
            || std::abs(actual[index].error - expected[index]) > tolerance) {
            throw std::runtime_error(
                std::string{label} + " mismatch for " + candidates[index].id
                + ": vulkan=" + std::to_string(actual[index].error)
                + ", cpu=" + std::to_string(expected[index])
                + ", tolerance=" + std::to_string(tolerance));
        }
    }
}

void test_conformance(const getnative::VulkanRuntimeProbe &probe) {
    constexpr std::int32_t width = 37;
    constexpr std::int32_t height = 29;
    const SourceFixture source = make_source(width, height, width + 5);
    const auto candidates = make_candidates(width, height);
    const getnative::MetricSpec metric{2, 3, 1, 2, 0.015F, 1U};

    getnative::VulkanAnalysisOptions options;
    options.device_index = compatible_device(probe).index;
    options.execution_slots = 2U;
    getnative::VulkanAnalysisEngine engine(options);
    compare_with_cpu(engine, source.view, candidates, metric, "mixed-axis batch");

    std::vector<float> exact_storage(8U * 6U, 0.5F);
    const getnative::ConstImageView exact_source{
        exact_storage.data(), 8, 6, 8};
    const std::vector<getnative::CandidateAnalysis> exact_candidate{{
        "strict-threshold", make_exact_gain_plan(8), nullptr,
        getnative::AnalysisAxes::horizontal}};
    const auto excluded = engine.analyze_axis_batch_f32(
        exact_source, exact_candidate,
        getnative::MetricSpec{0, 0, 0, 0, 0.25F, 1U});
    const auto included = engine.analyze_axis_batch_f32(
        exact_source, exact_candidate,
        getnative::MetricSpec{
            0, 0, 0, 0, std::nextafter(0.25F, 0.0F), 1U});
    expect(excluded.size() == 1U && excluded[0].error == 0.0
               && included.size() == 1U && included[0].error == 0.25,
           "Vulkan metric uses strict difference > threshold");

    const auto telemetry = engine.runtime_telemetry();
    expect(telemetry.command_buffer_submission_count == 3U
               && telemetry.kernel_dispatch_count >= 4U
               && telemetry.analyzed_candidate_count == candidates.size() + 2U
               && telemetry.plan_upload_bytes > 0U
               && telemetry.source_upload_bytes
                    == (static_cast<std::size_t>(width * height)
                        + 2U * 8U * 6U) * sizeof(float)
               && telemetry.result_readback_bytes > 0U,
           "Vulkan execution telemetry is complete");
    expect(engine.peak_workspace_elements() > 0U
               && engine.peak_working_set_bytes() > 0U,
           "Vulkan working-set telemetry is populated");

    auto first = std::async(std::launch::async, [&] {
        return engine.analyze_axis_batch_f32(source.view, candidates, metric);
    });
    auto second = std::async(std::launch::async, [&] {
        return engine.analyze_axis_batch_f32(source.view, candidates, metric);
    });
    expect(first.get().size() == candidates.size()
               && second.get().size() == candidates.size(),
           "two Vulkan execution slots support concurrent callers");

    std::stop_source cancelled;
    cancelled.request_stop();
    expect_throws<std::runtime_error>(
        [&] {
            (void)engine.analyze_axis_batch_f32(
                source.view, candidates, metric, cancelled.get_token());
        },
        "Vulkan rejects a pre-cancelled call");

    for (const std::uint32_t norm : {1U, 2U, 3U, 4U}) {
        getnative::MetricSpec p_norm_metric = metric;
        p_norm_metric.norm = norm;
        compare_with_cpu(
            engine, source.view, candidates, p_norm_metric,
            std::string{"p-norm "} + std::to_string(norm));
    }

    getnative::MetricSpec unsupported = metric;
    unsupported.norm = 5U;
    expect_throws<std::invalid_argument>(
        [&] {
            (void)engine.analyze_axis_batch_f32(
                source.view, candidates, unsupported);
        },
        "Vulkan rejects p-norms above four");

    getnative::VulkanAnalysisOptions tiled_options = options;
    tiled_options.execution_slots = 1U;
    tiled_options.workspace_limit_elements = 1500U;
    getnative::VulkanAnalysisEngine tiled(tiled_options);
    compare_with_cpu(tiled, source.view, candidates, metric, "tiled batch");
    expect(tiled.runtime_telemetry().tile_count > 1U
               && tiled.peak_workspace_elements() <= 1500U,
           "Vulkan workspace limit forces multiple tiles");

    try {
        getnative::VulkanAnalysisOptions validation_options = options;
        validation_options.execution_slots = 1U;
        validation_options.enable_validation = true;
        validation_options.force_non_coherent = true;
        getnative::VulkanAnalysisEngine validated(validation_options);
        compare_with_cpu(validated, source.view, candidates, metric,
                         "validated noncoherent batch");
        expect(validated.runtime_telemetry().validation_error_count == 0U,
               "Vulkan validation reports no errors");
    } catch (const std::runtime_error &error) {
        if (std::string_view{error.what()}.find(
                "VK_LAYER_KHRONOS_validation is unavailable")
            == std::string_view::npos) {
            throw;
        }
        std::cout << "NOTE: Vulkan validation layer is unavailable; "
                     "validation-only subcase skipped\n";
    }
}

} // namespace

int main() {
    try {
        test_default_device_selection();
        const getnative::VulkanRuntimeProbe probe = getnative::vulkan_runtime_probe();
        if (!probe.device_available) {
            std::cout << "SKIP: "
                      << (probe.reason.empty()
                              ? "no Vulkan compute device is available" : probe.reason)
                      << '\n';
            return 0;
        }
        test_conformance(probe);
        std::cout << "Vulkan analysis conformance passed on "
                  << compatible_device(probe).name << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Vulkan analysis test failed: " << error.what() << '\n';
        return 1;
    }
}

#include "gpu_batch.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/filter.hpp"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using getnative::AnalysisAxes;
using getnative::AxisPlan;
using getnative::AxisPlanRequest;
using getnative::CandidateAnalysis;
using getnative::ConstImageView;
using getnative::Filter;
using getnative::MetricSpec;
namespace gpu = getnative::detail::gpu;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <typename Function>
void require_failure(Function &&function, std::string_view message) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, message);
}

std::shared_ptr<const AxisPlan> make_plan(std::int32_t source,
                                         std::int32_t destination,
                                         Filter filter = Filter::bicubic()) {
    return std::make_shared<const AxisPlan>(getnative::build_axis_plan({
        .source_size = source,
        .destination_size = destination,
        .active_length = static_cast<double>(destination),
        .shift = 0.0,
        .filter = filter,
        .border = getnative::BorderMode::mirror,
    }));
}

void test_validation_and_shapes() {
    std::vector<float> pixels(64U * 48U, 0.25F);
    ConstImageView source{pixels.data(), 64, 48, 64};
    MetricSpec metric{5, 5, 4, 4, 0.015F, 1U};
    const auto crop = gpu::validate_source_and_metric(source, metric);
    require(crop.pixel_count == 54.0 * 40.0, "metric crop pixel count changed");

    CandidateAnalysis vertical{"v", nullptr, make_plan(48, 32), AnalysisAxes::vertical};
    const auto signature = gpu::candidate_signature(
        source, vertical, gpu::KernelDispatchPolicy::automatic);
    require(signature.vertical_inverse_shape == gpu::KernelShape::bandwidth7
                && signature.vertical_forward_shape == gpu::KernelShape::bandwidth7,
            "bicubic B7/F4 shape was not specialized");
    const auto generic = gpu::candidate_signature(
        source, vertical, gpu::KernelDispatchPolicy::generic_only);
    require(!gpu::uses_specialized_pipeline(generic),
            "generic-only policy selected a specialized shape");

    require_failure([&] {
        MetricSpec invalid = metric;
        invalid.norm = 2U;
        (void)gpu::validate_source_and_metric(source, invalid);
    }, "p!=1 metric was accepted");
    require_failure([&] {
        CandidateAnalysis missing{"missing", nullptr, nullptr, AnalysisAxes::vertical};
        (void)gpu::candidate_signature(
            source, missing, gpu::KernelDispatchPolicy::automatic);
    }, "null plan was accepted");
    require_failure([&] {
        auto mutable_plan = std::make_shared<AxisPlan>(*vertical.vertical);
        mutable_plan->transpose_offsets[1] =
            static_cast<std::uint32_t>(mutable_plan->transpose_indices.size() + 1U);
        CandidateAnalysis invalid{"bad-offset", nullptr, mutable_plan, AnalysisAxes::vertical};
        (void)gpu::candidate_signature(
            source, invalid, gpu::KernelDispatchPolicy::automatic);
    }, "invalid transpose offset was accepted");
    require_failure([&] {
        auto mutable_plan = std::make_shared<AxisPlan>(*vertical.vertical);
        mutable_plan->inverse_diagonal.front() =
            std::numeric_limits<float>::quiet_NaN();
        CandidateAnalysis invalid{"bad-finite", nullptr, mutable_plan, AnalysisAxes::vertical};
        (void)gpu::candidate_signature(
            source, invalid, gpu::KernelDispatchPolicy::automatic);
    }, "nonfinite plan value was accepted");
}

void test_tiling_and_packing() {
    std::vector<float> pixels(64U * 48U, 0.5F);
    ConstImageView source{pixels.data(), 64, 48, 67};
    const auto horizontal = make_plan(64, 40);
    const auto vertical = make_plan(48, 32);
    const auto bilinear = make_plan(48, 36, Filter::bilinear());
    std::vector<CandidateAnalysis> candidates{
        {"both-0", horizontal, vertical, AnalysisAxes::both},
        {"both-1", horizontal, vertical, AnalysisAxes::both},
        {"vertical", nullptr, bilinear, AnalysisAxes::vertical},
    };

    const std::size_t both_workspace = gpu::candidate_workspace_elements(
        source, candidates[0], gpu::KernelDispatchPolicy::automatic);
    const auto tiled = gpu::plan_tiles(
        source, candidates, 32U, both_workspace * 2U,
        gpu::KernelDispatchPolicy::automatic);
    require(tiled.tiles.size() == 2U, "adjacent signatures were not tiled deterministically");
    require(tiled.tiles[0].begin == 0U && tiled.tiles[0].end == 2U
                && tiled.tiles[0].workspace_elements == both_workspace * 2U,
            "two-axis tile boundaries changed");
    require(tiled.tiles[1].begin == 2U && tiled.tiles[1].end == 3U,
            "vertical tile boundaries changed");

    const auto packed = gpu::pack_tile(
        source, std::span<const CandidateAnalysis>{candidates}.first(2U),
        gpu::KernelDispatchPolicy::automatic);
    require(packed.descriptors.size() == 4U,
            "two-axis tile did not emit H descriptors followed by V descriptors");
    require(packed.descriptors[0].direction == 0U
                && packed.descriptors[1].direction == 0U
                && packed.descriptors[2].direction == 1U
                && packed.descriptors[3].direction == 1U,
            "two-axis descriptor order changed");
    require(packed.workspace_elements == both_workspace * 2U,
            "packed workspace size differs from preflight");
    require(gpu::packed_plan_bytes(packed) > packed.descriptors.size() * 64U,
            "packed plan byte accounting omitted plan arrays");

    require_failure([&] {
        (void)gpu::pack_tile(source, candidates,
                             gpu::KernelDispatchPolicy::automatic);
    }, "mixed-signature tile was accepted");
    require_failure([&] {
        (void)gpu::plan_tiles(source, candidates, 32U, both_workspace - 1U,
                              gpu::KernelDispatchPolicy::automatic);
    }, "single-candidate workspace overflow was accepted");
}

void test_deterministic_merge() {
    std::vector<CandidateAnalysis> candidates{
        {"a", nullptr, nullptr, AnalysisAxes::vertical},
        {"b", nullptr, nullptr, AnalysisAxes::vertical},
    };
    const std::vector<float> partials{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    const auto results = gpu::merge_metric_partials(candidates, partials, 3U, 10.0);
    require(results.size() == 2U && results[0].id == "a" && results[1].id == "b",
            "partial merge changed candidate order or ids");
    require(std::bit_cast<std::uint64_t>(results[0].error)
                == std::bit_cast<std::uint64_t>(0.6),
            "partial merge changed fixed group order");
    require_failure([&] {
        const std::vector<float> wrong{1.0F};
        (void)gpu::merge_metric_partials(candidates, wrong, 3U, 10.0);
    }, "wrong partial table size was accepted");
}

} // namespace

int main() {
    try {
        test_validation_and_shapes();
        test_tiling_and_packing();
        test_deterministic_merge();
        std::cout << "GPU batch tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "GPU batch test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

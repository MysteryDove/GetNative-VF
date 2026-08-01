#pragma once

#include "getnative/cpu_analysis.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace getnative::detail::gpu {

inline constexpr std::int32_t maximum_half_bandwidth = 15;
inline constexpr std::int32_t maximum_forward_width = 16;

enum class KernelShape : std::uint8_t {
    bandwidth3,
    bandwidth7,
    bandwidth11,
    bandwidth15,
    generic,
};

enum class KernelDispatchPolicy : std::uint8_t {
    automatic,
    generic_only,
    required_specialized,
};

struct alignas(16) AxisPlanDescriptor {
    std::uint32_t source_size;
    std::uint32_t destination_size;
    std::uint32_t half_bandwidth;
    std::uint32_t forward_width;
    std::uint32_t transpose_offsets_base;
    std::uint32_t transpose_entries_base;
    std::uint32_t lower_ld_base;
    std::uint32_t upper_l_base;
    std::uint32_t inverse_diagonal_base;
    std::uint32_t forward_left_base;
    std::uint32_t forward_weights_base;
    std::uint32_t workspace_base;
    std::uint32_t direction;
    std::uint32_t vector_count;
    std::uint32_t reserved_0 = 0;
    std::uint32_t reserved_1 = 0;
};

struct AnalysisJob {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t crop_left;
    std::uint32_t crop_right;
    std::uint32_t crop_top;
    std::uint32_t crop_bottom;
    float threshold;
    std::uint32_t groups_per_candidate;
    std::uint32_t candidate_count;
    std::uint32_t maximum_vector_count;
};

struct MetricCropBounds {
    std::uint32_t left;
    std::uint32_t right;
    std::uint32_t top;
    std::uint32_t bottom;
    double pixel_count;
};

struct AxisKernelShapes {
    KernelShape inverse = KernelShape::generic;
    KernelShape forward = KernelShape::generic;
};

struct TileSignature {
    AnalysisAxes axes = AnalysisAxes::vertical;
    KernelShape horizontal_inverse_shape = KernelShape::generic;
    KernelShape horizontal_forward_shape = KernelShape::generic;
    KernelShape vertical_inverse_shape = KernelShape::generic;
    KernelShape vertical_forward_shape = KernelShape::generic;
    ForwardOrder forward_order = ForwardOrder::vertical_first;

    friend bool operator==(const TileSignature &, const TileSignature &) = default;
};

struct TileRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t workspace_elements = 0;
    TileSignature signature{};
};

struct TiledBatch {
    std::vector<TileRange> tiles;
    std::size_t maximum_workspace_elements = 0;
};

struct PackedTile {
    std::vector<AxisPlanDescriptor> descriptors;
    std::vector<std::uint32_t> transpose_offsets;
    std::vector<std::uint32_t> transpose_indices;
    std::vector<float> transpose_weights;
    std::vector<float> lower_ld;
    std::vector<float> upper_l;
    std::vector<float> inverse_diagonal;
    std::vector<std::int32_t> forward_left;
    std::vector<float> forward_weights;
    std::size_t workspace_elements = 0;
    std::uint32_t maximum_vector_count = 0;
    std::uint32_t maximum_native_width = 0;
    std::uint32_t maximum_native_height = 0;
};

static_assert(sizeof(AxisPlanDescriptor) == 64);
static_assert(alignof(AxisPlanDescriptor) == 16);
static_assert(sizeof(AnalysisJob) == 40);

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, const char *name);
[[nodiscard]] std::size_t checked_product(std::size_t a, std::size_t b,
                                          const char *name);
[[nodiscard]] MetricCropBounds validate_source_and_metric(ConstImageView source,
                                                           const MetricSpec &metric);
[[nodiscard]] AxisKernelShapes axis_shapes(const AxisPlan &plan,
                                           KernelDispatchPolicy policy);
[[nodiscard]] TileSignature candidate_signature(ConstImageView source,
                                                 const CandidateAnalysis &candidate,
                                                 KernelDispatchPolicy policy);
[[nodiscard]] bool uses_specialized_pipeline(const TileSignature &signature) noexcept;
[[nodiscard]] std::size_t candidate_workspace_elements(
    ConstImageView source, const CandidateAnalysis &candidate,
    KernelDispatchPolicy policy);
[[nodiscard]] TiledBatch plan_tiles(ConstImageView source,
                                    std::span<const CandidateAnalysis> candidates,
                                    std::size_t tile_size,
                                    std::size_t workspace_limit_elements,
                                    KernelDispatchPolicy policy,
                                    std::stop_token stop = {});
[[nodiscard]] PackedTile pack_tile(ConstImageView source,
                                   std::span<const CandidateAnalysis> candidates,
                                   KernelDispatchPolicy policy);
[[nodiscard]] std::size_t packed_plan_bytes(const PackedTile &packed);
[[nodiscard]] std::vector<CandidateResult> merge_metric_partials(
    std::span<const CandidateAnalysis> candidates,
    std::span<const float> partials,
    std::size_t groups_per_candidate,
    double pixel_count);

} // namespace getnative::detail::gpu

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace getnative::vulkan_detail {

inline constexpr std::uint32_t horizontal_axes = 0U;
inline constexpr std::uint32_t vertical_axes = 1U;
inline constexpr std::uint32_t both_axes = 2U;
inline constexpr std::uint32_t horizontal_first = 0U;
inline constexpr std::uint32_t vertical_first = 1U;
inline constexpr std::uint32_t axis_descriptor_words = 12U;
inline constexpr std::uint32_t candidate_descriptor_words = 29U;
inline constexpr std::uint32_t maximum_tile_candidates = 32U;

struct AxisPlanDescriptor {
    std::uint32_t source_size = 0U;
    std::uint32_t destination_size = 0U;
    std::uint32_t half_bandwidth = 0U;
    std::uint32_t forward_width = 0U;
    std::uint32_t forward_left_base = 0U;
    std::uint32_t forward_weights_base = 0U;
    std::uint32_t transpose_offsets_base = 0U;
    std::uint32_t transpose_indices_base = 0U;
    std::uint32_t transpose_weights_base = 0U;
    std::uint32_t lower_ld_base = 0U;
    std::uint32_t upper_l_base = 0U;
    std::uint32_t inverse_diagonal_base = 0U;
};

struct CandidateDescriptor {
    AxisPlanDescriptor horizontal;
    AxisPlanDescriptor vertical;
    std::uint32_t axes = both_axes;
    std::uint32_t forward_order = vertical_first;
    std::uint32_t workspace_base = 0U;
    std::uint32_t intermediate_elements = 0U;
    std::uint32_t native_elements = 0U;
};

static_assert(std::is_trivially_copyable_v<AxisPlanDescriptor>);
static_assert(std::is_trivially_copyable_v<CandidateDescriptor>);
static_assert(sizeof(AxisPlanDescriptor) == axis_descriptor_words * sizeof(std::uint32_t));
static_assert(sizeof(CandidateDescriptor) == candidate_descriptor_words * sizeof(std::uint32_t));
static_assert(offsetof(CandidateDescriptor, workspace_base) == 26U * sizeof(std::uint32_t));

} // namespace getnative::vulkan_detail

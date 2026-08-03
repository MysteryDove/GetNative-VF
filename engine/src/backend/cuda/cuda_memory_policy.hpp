#pragma once

#include <algorithm>
#include <cstddef>

namespace getnative::cuda_detail {

inline constexpr std::size_t cuda_default_workspace_bytes =
    640ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t cuda_maximum_explicit_bytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL;
inline constexpr std::size_t cuda_memory_reserve_floor_bytes =
    512ULL * 1024ULL * 1024ULL;

struct CudaMemoryBudget {
    std::size_t initial_free_bytes = 0U;
    std::size_t reserve_bytes = 0U;
    std::size_t engine_budget_bytes = 0U;
    std::size_t per_slot_budget_bytes = 0U;
    std::size_t workspace_limit_bytes = 0U;
    bool workspace_limit_clamped = false;
};

[[nodiscard]] constexpr CudaMemoryBudget make_cuda_memory_budget(
    std::size_t free_bytes, std::size_t execution_slots,
    std::size_t requested_workspace_elements) noexcept {
    CudaMemoryBudget result;
    result.initial_free_bytes = free_bytes;
    if (free_bytes == 0U || execution_slots == 0U) return result;

    const std::size_t reserve_floor = std::min(
        cuda_memory_reserve_floor_bytes, free_bytes / 2U);
    result.reserve_bytes = std::max(reserve_floor, free_bytes / 8U);
    result.reserve_bytes = std::min(result.reserve_bytes, free_bytes / 2U);
    result.engine_budget_bytes = free_bytes - result.reserve_bytes;
    result.per_slot_budget_bytes = result.engine_budget_bytes / execution_slots;

    const std::size_t safe_workspace_bytes =
        result.per_slot_budget_bytes - result.per_slot_budget_bytes / 5U;
    const std::size_t maximum_workspace_elements = std::min(
        cuda_default_workspace_bytes / sizeof(float),
        cuda_maximum_explicit_bytes / sizeof(float));
    const bool requested_limit_clamped = requested_workspace_elements != 0U
        && requested_workspace_elements > maximum_workspace_elements;
    const std::size_t desired_workspace_elements =
        requested_workspace_elements == 0U
        ? cuda_default_workspace_bytes / sizeof(float)
        : std::min(requested_workspace_elements, maximum_workspace_elements);
    const std::size_t desired_workspace_bytes =
        desired_workspace_elements * sizeof(float);
    result.workspace_limit_bytes = std::min(
        desired_workspace_bytes, safe_workspace_bytes);
    result.workspace_limit_bytes -=
        result.workspace_limit_bytes % sizeof(float);
    result.workspace_limit_clamped = requested_limit_clamped
        || result.workspace_limit_bytes < desired_workspace_bytes;
    return result;
}

} // namespace getnative::cuda_detail

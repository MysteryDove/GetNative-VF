#pragma once

#include <cstdint>

namespace getnative::cli {

enum class AutomaticBackend : std::uint8_t { cpu, cuda, vulkan };

// Auto is capability-driven: CUDA and discrete Vulkan support p=1..4, and CPU
// is the universal fallback.
[[nodiscard]] constexpr AutomaticBackend choose_automatic_backend(
    std::uint32_t p_norm, bool cuda_available, bool vulkan_available,
    bool vulkan_is_discrete) noexcept {
    if (cuda_available && p_norm >= 1U && p_norm <= 4U) {
        return AutomaticBackend::cuda;
    }
    if (vulkan_available && vulkan_is_discrete && p_norm >= 1U && p_norm <= 4U) {
        return AutomaticBackend::vulkan;
    }
    return AutomaticBackend::cpu;
}

} // namespace getnative::cli

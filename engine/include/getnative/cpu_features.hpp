#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace getnative {

enum class CpuIsa : std::uint8_t {
    scalar,
    sse2,
    avx2,
    avx512,
};

enum class CpuIsaRequest : std::uint8_t {
    automatic,
    scalar,
    sse2,
    avx2,
    avx512,
};

struct CpuIdRegisters {
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
};

// Raw, injectable host state used by the x86 capability evaluator. xcr0_valid
// records whether OSXSAVE authorized reading XCR0; an arbitrary xcr0 value is
// never trusted without it.
struct CpuFeatureSnapshot {
    bool x86 = false;
    std::uint32_t maximum_basic_leaf = 0;
    std::array<char, 13> vendor{};
    CpuIdRegisters leaf1{};
    CpuIdRegisters leaf7_subleaf0{};
    std::uint64_t xcr0 = 0;
    bool xcr0_valid = false;
    std::uint32_t logical_processor_count = 0;
    std::uint32_t family = 0;
    std::uint32_t model = 0;
    std::uint32_t stepping = 0;
};

struct CpuIsaSet {
    bool scalar = true;
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;

    [[nodiscard]] constexpr bool contains(CpuIsa isa) const noexcept {
        switch (isa) {
        case CpuIsa::scalar: return scalar;
        case CpuIsa::sse2: return sse2;
        case CpuIsa::avx2: return avx2;
        case CpuIsa::avx512: return avx512;
        }
        return false;
    }
};

struct CpuDispatchInfo {
    CpuFeatureSnapshot snapshot{};
    CpuIsaSet compiled{};
    CpuIsaSet available{};
    CpuIsaRequest request = CpuIsaRequest::automatic;
    CpuIsa selected = CpuIsa::scalar;
    bool request_available = true;
    bool forced = false;
    // Production CPU path allows FMA; not a multi-mode surface.
    std::string_view math_mode = "production";
    std::string_view selection_reason{};
};

[[nodiscard]] constexpr std::string_view cpu_isa_name(CpuIsa isa) noexcept {
    switch (isa) {
    case CpuIsa::scalar: return "scalar";
    case CpuIsa::sse2: return "sse2";
    case CpuIsa::avx2: return "avx2";
    case CpuIsa::avx512: return "avx512";
    }
    return "scalar";
}

[[nodiscard]] constexpr std::string_view cpu_isa_request_name(
    CpuIsaRequest request) noexcept {
    switch (request) {
    case CpuIsaRequest::automatic: return "auto";
    case CpuIsaRequest::scalar: return "scalar";
    case CpuIsaRequest::sse2: return "sse2";
    case CpuIsaRequest::avx2: return "avx2";
    case CpuIsaRequest::avx512: return "avx512";
    }
    return "auto";
}

[[nodiscard]] std::optional<CpuIsaRequest> parse_cpu_isa_request(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view cpu_vendor(const CpuFeatureSnapshot &snapshot) noexcept;

[[nodiscard]] CpuIsaSet compiled_cpu_isa_set() noexcept;
[[nodiscard]] CpuFeatureSnapshot detect_cpu_feature_snapshot() noexcept;
[[nodiscard]] const CpuFeatureSnapshot &host_cpu_feature_snapshot() noexcept;

// avx512_benchmark_approved is deliberately explicit. Native automatic
// dispatch passes false until the current CPU signature has 5%/3% gate data.
[[nodiscard]] CpuDispatchInfo evaluate_cpu_dispatch(
    const CpuFeatureSnapshot &snapshot,
    CpuIsaSet compiled,
    CpuIsaRequest request = CpuIsaRequest::automatic,
    bool avx512_benchmark_approved = false) noexcept;
[[nodiscard]] CpuDispatchInfo cpu_dispatch_info(
    CpuIsaRequest request = CpuIsaRequest::automatic) noexcept;
[[nodiscard]] CpuIsa require_cpu_isa(CpuIsaRequest request);

} // namespace getnative

#include "getnative/cpu_features.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#ifndef GETNATIVE_X86_SSE2_COMPILED
#define GETNATIVE_X86_SSE2_COMPILED 0
#endif
#ifndef GETNATIVE_X86_AVX2_COMPILED
#define GETNATIVE_X86_AVX2_COMPILED 0
#endif
#ifndef GETNATIVE_X86_AVX512_COMPILED
#define GETNATIVE_X86_AVX512_COMPILED 0
#endif

namespace getnative {
namespace {

constexpr std::uint32_t bit(std::uint32_t index) noexcept {
    return std::uint32_t{1} << index;
}

[[nodiscard]] CpuIdRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    CpuIdRegisters result{};
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int registers[4]{};
    __cpuidex(registers, static_cast<int>(leaf), static_cast<int>(subleaf));
    result.eax = static_cast<std::uint32_t>(registers[0]);
    result.ebx = static_cast<std::uint32_t>(registers[1]);
    result.ecx = static_cast<std::uint32_t>(registers[2]);
    result.edx = static_cast<std::uint32_t>(registers[3]);
#elif defined(__i386__) || defined(__x86_64__)
    __cpuid_count(leaf, subleaf, result.eax, result.ebx, result.ecx, result.edx);
#else
    (void)leaf;
    (void)subleaf;
#endif
    return result;
}

[[nodiscard]] std::uint64_t read_xcr0() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return _xgetbv(0);
#elif defined(__i386__) || defined(__x86_64__)
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32U) | low;
#else
    return 0;
#endif
}

void decode_signature(CpuFeatureSnapshot &snapshot) noexcept {
    const std::uint32_t signature = snapshot.leaf1.eax;
    const std::uint32_t base_family = (signature >> 8U) & 0xFU;
    const std::uint32_t base_model = (signature >> 4U) & 0xFU;
    const std::uint32_t extended_family = (signature >> 20U) & 0xFFU;
    const std::uint32_t extended_model = (signature >> 16U) & 0xFU;
    snapshot.family = base_family == 0xFU
        ? base_family + extended_family : base_family;
    snapshot.model = (base_family == 0x6U || base_family == 0xFU)
        ? base_model + (extended_model << 4U) : base_model;
    snapshot.stepping = signature & 0xFU;
}

[[nodiscard]] CpuIsa requested_isa(CpuIsaRequest request) noexcept {
    switch (request) {
    case CpuIsaRequest::scalar: return CpuIsa::scalar;
    case CpuIsaRequest::sse2: return CpuIsa::sse2;
    case CpuIsaRequest::avx2: return CpuIsa::avx2;
    case CpuIsaRequest::avx512: return CpuIsa::avx512;
    case CpuIsaRequest::automatic: break;
    }
    return CpuIsa::scalar;
}

} // namespace

std::optional<CpuIsaRequest> parse_cpu_isa_request(std::string_view value) noexcept {
    if (value == "auto") return CpuIsaRequest::automatic;
    if (value == "scalar") return CpuIsaRequest::scalar;
    if (value == "sse2") return CpuIsaRequest::sse2;
    if (value == "avx2") return CpuIsaRequest::avx2;
    if (value == "avx512" || value == "avx-512") return CpuIsaRequest::avx512;
    return std::nullopt;
}

std::string_view cpu_vendor(const CpuFeatureSnapshot &snapshot) noexcept {
    std::size_t length = 0;
    while (length < snapshot.vendor.size() && snapshot.vendor[length] != '\0') {
        ++length;
    }
    return {snapshot.vendor.data(), length};
}

CpuIsaSet compiled_cpu_isa_set() noexcept {
    return {
        true,
        GETNATIVE_X86_SSE2_COMPILED != 0,
        GETNATIVE_X86_AVX2_COMPILED != 0,
        GETNATIVE_X86_AVX512_COMPILED != 0,
    };
}

CpuFeatureSnapshot detect_cpu_feature_snapshot() noexcept {
    CpuFeatureSnapshot snapshot{};
#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
    snapshot.x86 = true;
    const CpuIdRegisters leaf0 = cpuid(0, 0);
    snapshot.maximum_basic_leaf = leaf0.eax;
    std::memcpy(snapshot.vendor.data(), &leaf0.ebx, sizeof(leaf0.ebx));
    std::memcpy(snapshot.vendor.data() + 4, &leaf0.edx, sizeof(leaf0.edx));
    std::memcpy(snapshot.vendor.data() + 8, &leaf0.ecx, sizeof(leaf0.ecx));
    snapshot.vendor[12] = '\0';
    if (snapshot.maximum_basic_leaf >= 1U) {
        snapshot.leaf1 = cpuid(1, 0);
        decode_signature(snapshot);
        if ((snapshot.leaf1.ecx & bit(27)) != 0U) {
            snapshot.xcr0 = read_xcr0();
            snapshot.xcr0_valid = true;
        }
    }
    if (snapshot.maximum_basic_leaf >= 7U) {
        snapshot.leaf7_subleaf0 = cpuid(7, 0);
    }
#endif
    snapshot.logical_processor_count = std::thread::hardware_concurrency();
    return snapshot;
}

const CpuFeatureSnapshot &host_cpu_feature_snapshot() noexcept {
    static const CpuFeatureSnapshot snapshot = detect_cpu_feature_snapshot();
    return snapshot;
}

CpuDispatchInfo evaluate_cpu_dispatch(
    const CpuFeatureSnapshot &snapshot,
    CpuIsaSet compiled,
    CpuIsaRequest request,
    bool avx512_benchmark_approved) noexcept {
    compiled.scalar = true;
    CpuDispatchInfo result{};
    result.snapshot = snapshot;
    result.compiled = compiled;
    result.request = request;
    result.forced = request != CpuIsaRequest::automatic;

    const bool has_leaf1 = snapshot.x86 && snapshot.maximum_basic_leaf >= 1U;
    const bool has_leaf7 = snapshot.x86 && snapshot.maximum_basic_leaf >= 7U;
    const bool has_sse2 = has_leaf1 && (snapshot.leaf1.edx & bit(26)) != 0U;
    const bool has_avx_os = has_leaf1
        && (snapshot.leaf1.ecx & bit(27)) != 0U
        && (snapshot.leaf1.ecx & bit(28)) != 0U
        && snapshot.xcr0_valid
        && (snapshot.xcr0 & 0x6U) == 0x6U;
    const bool has_avx2 = has_leaf7
        && has_avx_os
        && (snapshot.leaf7_subleaf0.ebx & bit(5)) != 0U;
    const bool has_avx512_state = has_avx2
        && (snapshot.xcr0 & 0xE6U) == 0xE6U;
    const bool has_avx512f = has_leaf7
        && (snapshot.leaf7_subleaf0.ebx & bit(16)) != 0U;

    result.available = {
        true,
        compiled.sse2 && has_sse2,
        compiled.avx2 && has_avx2,
        compiled.avx512 && has_avx512_state && has_avx512f,
    };

    if (result.forced) {
        result.selected = requested_isa(request);
        result.request_available = result.available.contains(result.selected);
        result.selection_reason = result.request_available
            ? "forced by test or benchmark"
            : "requested CPU ISA is unavailable";
        return result;
    }

    if (result.available.avx512 && avx512_benchmark_approved) {
        result.selected = CpuIsa::avx512;
        result.selection_reason = "widest benchmark-approved production tier";
    } else if (result.available.avx2) {
        result.selected = CpuIsa::avx2;
        result.selection_reason = result.available.avx512
            ? "avx512 not benchmark-approved"
            : "widest available production tier";
    } else if (result.available.sse2) {
        result.selected = CpuIsa::sse2;
        result.selection_reason = "AVX2 unavailable; SSE2 production fallback";
    } else {
        result.selected = CpuIsa::scalar;
        result.selection_reason = "no compiled x86 SIMD tier is available";
    }
    return result;
}

CpuDispatchInfo cpu_dispatch_info(CpuIsaRequest request) noexcept {
    // AVX-512 remains force-only until a CPU-signature-specific approval table
    // is populated from the required same-host benchmark artifacts.
    return evaluate_cpu_dispatch(
        host_cpu_feature_snapshot(), compiled_cpu_isa_set(), request, false);
}

CpuIsa require_cpu_isa(CpuIsaRequest request) {
    const CpuDispatchInfo info = cpu_dispatch_info(request);
    if (!info.request_available) {
        throw std::runtime_error(
            "requested CPU ISA is unavailable: "
            + std::string{cpu_isa_request_name(request)});
    }
    return info.selected;
}

} // namespace getnative

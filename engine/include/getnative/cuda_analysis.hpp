#pragma once

#include "getnative/cpu_analysis.hpp"
#include "getnative/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace getnative {

inline constexpr std::uint32_t cuda_minimum_p_norm = 1U;
inline constexpr std::uint32_t cuda_maximum_p_norm = 4U;

struct CudaDeviceInfo {
    std::int32_t ordinal = -1;
    std::string name;
    std::string uuid;
    std::int32_t compute_capability_major = 0;
    std::int32_t compute_capability_minor = 0;
    std::int32_t driver_version = 0;
    std::size_t total_memory_bytes = 0;
    std::int32_t maximum_threads_per_block = 0;
    bool backend_compatible = false;
    std::string incompatibility_reason;
    std::int32_t multiprocessor_count = 0;
    std::int32_t maximum_threads_per_multiprocessor = 0;
    std::int32_t registers_per_multiprocessor = 0;
    std::int32_t shared_memory_per_block_bytes = 0;
    std::int32_t shared_memory_per_multiprocessor_bytes = 0;
    std::int32_t warp_size = 0;
    std::int32_t clock_rate_khz = 0;
    std::int32_t memory_clock_rate_khz = 0;
    std::int32_t memory_bus_width_bits = 0;
    std::int32_t l2_cache_bytes = 0;
};

struct CudaRuntimeProbe {
    bool driver_loaded = false;
    bool initialized = false;
    bool device_available = false;
    std::string reason;
    std::vector<CudaDeviceInfo> devices;
};

// The implementation deliberately exposes only one executable variant.
// The other names remain reserved so a future specialization cannot silently
// change the baseline selected by existing callers.
enum class CudaKernelVariant : std::uint8_t {
    automatic,
    cpp_generic,
    cpp_specialized,
    architecture_specific,
    inline_ptx,
};

struct CudaKernelResourceInfo {
    std::string name;
    std::int32_t register_count = 0;
    std::int32_t static_shared_bytes = 0;
    std::int32_t local_bytes = 0;
    std::int32_t constant_bytes = 0;
    std::int32_t binary_version = 0;
    std::int32_t ptx_version = 0;
};

[[nodiscard]] constexpr std::string_view cuda_kernel_variant_name(
    CudaKernelVariant variant) noexcept {
    switch (variant) {
    case CudaKernelVariant::automatic: return "automatic";
    case CudaKernelVariant::cpp_generic: return "cpp-generic";
    case CudaKernelVariant::cpp_specialized: return "cpp-specialized";
    case CudaKernelVariant::architecture_specific: return "architecture-specific";
    case CudaKernelVariant::inline_ptx: return "inline-ptx";
    }
    return "unknown";
}

struct CudaRuntimeTelemetry {
    std::size_t kernel_launch_count = 0;
    std::size_t analyzed_candidate_count = 0;
    std::size_t tile_count = 0;
    std::size_t buffer_allocation_count = 0;
    std::size_t plan_cache_hits = 0;
    std::size_t plan_cache_misses = 0;
    std::size_t host_plan_cache_hits = 0;
    std::size_t host_plan_cache_misses = 0;
    std::size_t host_plan_cache_bytes = 0;
    std::size_t source_cache_hits = 0;
    std::size_t source_cache_misses = 0;
    std::size_t source_upload_bytes = 0;
    std::size_t source_upload_count = 0;
    std::size_t source_conversion_bytes = 0;
    std::size_t source_conversion_count = 0;
    std::size_t source_transpose_count = 0;
    std::size_t plan_upload_bytes = 0;
    std::size_t result_readback_bytes = 0;
    std::size_t workspace_bytes = 0;
    std::size_t pinned_staging_bytes = 0;
    std::size_t peak_workspace_elements = 0;
    std::size_t initial_device_free_bytes = 0;
    std::size_t device_memory_reserve_bytes = 0;
    std::size_t device_memory_budget_bytes = 0;
    std::size_t per_slot_memory_budget_bytes = 0;
    std::size_t effective_workspace_limit_bytes = 0;
    bool workspace_limit_clamped = false;
    double execution_slot_wait_ms = 0.0;
    double host_pack_ms = 0.0;
    double source_staging_ms = 0.0;
    // Transfer and kernel fields below are measured with CUDA events.
    double source_upload_ms = 0.0;
    double source_conversion_ms = 0.0;
    double plan_upload_ms = 0.0;
    double source_transpose_ms = 0.0;
    double horizontal_fused_ms = 0.0;
    double inverse_horizontal_ms = 0.0;
    double inverse_vertical_ms = 0.0;
    double forward_intermediate_ms = 0.0;
    double metric_ms = 0.0;
    double kernel_ms = 0.0;
    double result_readback_ms = 0.0;
    double gpu_total_ms = 0.0;
    std::string kernel_variant = "cpp-generic";
    std::string artifact_stage = "staged-cpp";
    std::string artifact_target;
    std::string artifact_name;
    std::string artifact_hash_fnv1a64;
    std::vector<CudaKernelResourceInfo> kernel_resources;
};

struct CudaLaunchPolicy {
    std::uint32_t inverse_threads = 64U;
    std::uint32_t pixel_threads = 256U;
    std::uint32_t maximum_metric_blocks = 128U;
    bool paired_vertical = true;
};

// Keep the production launch policy explicit and shared with benchmark
// baselines. Candidate policies may still be supplied by benchmark-only code.
inline constexpr CudaLaunchPolicy cuda_default_launch_policy{};

struct CudaAnalysisOptions {
    std::string device_uuid;
    std::int32_t device_ordinal = 0;
    std::size_t workspace_limit_elements = 0;
    std::size_t execution_slots = 2;
    CudaKernelVariant kernel_variant = CudaKernelVariant::automatic;
    CudaLaunchPolicy launch_policy = cuda_default_launch_policy;
};

enum class CudaLumaFormat : std::uint8_t {
    nv12,
    p010,
    p016,
    yuv444p8,
    yuv444p16,
};

enum class CudaColorRange : std::uint8_t {
    limited,
    full,
};

// Opaque CUDA handles keep the public API independent of CUDA headers while
// still making ownership and synchronization explicit. The producer context
// must be the exact context returned by native_context().
struct CudaLumaFrameView {
    std::uintptr_t device_pointer = 0U;
    std::size_t pitch_bytes = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    CudaLumaFormat format = CudaLumaFormat::nv12;
    std::int32_t bit_depth = 8;
    CudaColorRange range = CudaColorRange::limited;
    std::uintptr_t context = 0U;
    std::uintptr_t producer_stream = 0U;
};

[[nodiscard]] CudaRuntimeProbe cuda_runtime_probe() noexcept;
[[nodiscard]] bool cuda_backend_available() noexcept;
[[nodiscard]] std::vector<CudaDeviceInfo> enumerate_cuda_devices();
[[nodiscard]] std::int32_t cuda_minimum_compute_capability() noexcept;
[[nodiscard]] std::string_view cuda_compiled_artifact_target() noexcept;

class CudaAnalysisEngine {
public:
    explicit CudaAnalysisEngine(CudaAnalysisOptions options = {});
    ~CudaAnalysisEngine();

    CudaAnalysisEngine(const CudaAnalysisEngine &) = delete;
    CudaAnalysisEngine &operator=(const CudaAnalysisEngine &) = delete;
    CudaAnalysisEngine(CudaAnalysisEngine &&) noexcept;
    CudaAnalysisEngine &operator=(CudaAnalysisEngine &&) noexcept;

    [[nodiscard]] const CudaDeviceInfo &device_info() const noexcept;
    [[nodiscard]] const CudaAnalysisOptions &options() const noexcept;
    [[nodiscard]] std::size_t peak_workspace_elements() const noexcept;
    [[nodiscard]] std::size_t peak_working_set_bytes() const noexcept;
    [[nodiscard]] CudaRuntimeTelemetry runtime_telemetry() const;
    void reset_analysis_telemetry();
    [[nodiscard]] std::uintptr_t native_context() const noexcept;
    [[nodiscard]] std::uintptr_t native_decode_stream() const noexcept;

    // Validates the complete per-frame device working set for a media task
    // before decoding starts. This does not submit work or acquire a slot.
    void preflight_axis_batch(
        ConstImageView dimensions,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::size_t concurrency) const;

    // Generic staged CUDA C++ path. Axis recurrences remain serial only along
    // their dependency direction; candidates and orthogonal rows/columns run
    // in parallel. Reconstruction is fused into the p=1..4 metric.
    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_f32(
        ConstImageView source, std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {});

    // Converts the decoder-owned luma plane directly into the resident F32
    // source buffer. No frame-sized payload is staged through host memory.
    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_cuda_luma(
        const CudaLumaFrameView &source,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {});

private:
    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_impl(
        ConstImageView source, const CudaLumaFrameView *cuda_luma,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace getnative

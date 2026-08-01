#pragma once

#include "getnative/cpu_analysis.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace getnative {

struct CudaDeviceInfo {
    std::int32_t ordinal = -1;
    std::string name;
    std::string uuid;
    std::int32_t compute_capability_major = 0;
    std::int32_t compute_capability_minor = 0;
    std::int32_t driver_version = 0;
    std::size_t total_memory_bytes = 0;
    std::int32_t maximum_threads_per_block = 0;
};

struct CudaRuntimeProbe {
    bool driver_loaded = false;
    bool initialized = false;
    bool device_available = false;
    std::string reason;
    std::vector<CudaDeviceInfo> devices;
};

enum class CudaKernelVariant : std::uint8_t {
    automatic,
    cpp_generic,
    cpp_specialized,
    architecture_specific,
    inline_ptx,
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

// Counters and timings accumulate until reset_analysis_telemetry(). Retained
// byte counts describe current capacities and survive a telemetry reset.
struct CudaRuntimeTelemetry {
    std::size_t buffer_allocation_count = 0;
    std::size_t buffer_allocation_bytes = 0;
    std::size_t working_buffer_allocation_count = 0;
    std::size_t working_buffer_allocation_bytes = 0;
    std::size_t working_buffer_reuse_count = 0;
    std::size_t working_buffer_active_bytes = 0;
    std::size_t working_buffer_retained_bytes = 0;
    std::size_t working_buffer_peak_active_bytes = 0;
    std::size_t working_buffer_peak_retained_bytes = 0;
    std::size_t queued_plan_peak_bytes = 0;
    std::size_t total_peak_explicit_bytes = 0;
    std::size_t stream_submission_count = 0;
    std::size_t stream_completion_count = 0;
    std::size_t plan_upload_bytes = 0;
    std::size_t source_upload_bytes = 0;
    std::size_t partial_readback_bytes = 0;
    std::size_t analyzed_tile_count = 0;
    std::size_t generic_tile_count = 0;
    std::size_t specialized_tile_count = 0;
    double buffer_allocation_ms = 0.0;
    double working_buffer_allocation_ms = 0.0;
    double module_load_ms = 0.0;
    double plan_pack_ms = 0.0;
    double source_upload_ms = 0.0;
    double plan_upload_ms = 0.0;
    double buffer_wiring_ms = 0.0;
    double inverse_h_ms = 0.0;
    double inverse_v_ms = 0.0;
    double first_forward_ms = 0.0;
    double metric_ms = 0.0;
    double gpu_execution_ms = 0.0;
    double partial_readback_ms = 0.0;
    double cpu_merge_ms = 0.0;
    std::size_t module_artifact_bytes = 0;
    std::int32_t module_binary_version = 0;
    std::int32_t module_ptx_version = 0;
    std::int32_t maximum_kernel_register_count = 0;
    std::int32_t maximum_kernel_static_shared_bytes = 0;
    bool ptx_jit_forced = false;
    std::vector<std::string> created_kernel_names;
    std::string module_artifact_name;
    std::string module_artifact_hash_fnv1a64;
    std::string module_compile_flags;
    std::string module_path_provenance;
    std::string module_jit_info_log;
    std::string module_jit_error_log;
};

struct CudaAnalysisOptions {
    // An empty UUID selects device_ordinal. A non-empty UUID is authoritative.
    std::string device_uuid;
    std::int32_t device_ordinal = 0;
    std::size_t tile_size = 32;
    std::size_t reduction_groups_per_candidate = 8;
    std::size_t workspace_limit_elements = 0;
    std::size_t inverse_threads_per_block = 32;
    CudaKernelVariant kernel_variant = CudaKernelVariant::automatic;
    bool reuse_working_buffers = true;
    std::size_t retained_working_buffer_limit_bytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL;
    // All explicit device allocations for a call must stay strictly below this
    // ceiling. The default encodes the handover's "< 2 GiB" requirement.
    std::size_t maximum_total_working_set_bytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL;
};

[[nodiscard]] CudaRuntimeProbe cuda_runtime_probe() noexcept;
[[nodiscard]] bool cuda_backend_available() noexcept;
[[nodiscard]] std::vector<CudaDeviceInfo> enumerate_cuda_devices();

// Consumes caller-owned immutable AxisPlans. Calls are serialized so one
// engine can safely retain and reuse its context, stream, and working buffers.
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
    void trim_working_buffers();

    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_f32(
        ConstImageView source, std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace getnative

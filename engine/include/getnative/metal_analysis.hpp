#pragma once

#include "getnative/cpu_analysis.hpp"
#include "getnative/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace getnative {

inline constexpr std::uint32_t metal_minimum_p_norm = 1U;
inline constexpr std::uint32_t metal_maximum_p_norm = 4U;

struct MetalDeviceInfo {
    std::string name;
    std::uint64_t registry_id = 0;
    std::size_t maximum_buffer_bytes = 0;
    bool unified_memory = false;
};

enum class MetalKernelDispatchPolicy : std::uint8_t {
    automatic,
    generic_only,
    required_specialized,
};

// Counters and timings accumulate until reset_analysis_telemetry(). Retained bytes
// are a current-capacity gauge and therefore survive a telemetry reset.
struct MetalRuntimeTelemetry {
    std::size_t buffer_allocation_count = 0;
    std::size_t buffer_allocation_bytes = 0;
    std::size_t working_buffer_allocation_count = 0;
    std::size_t working_buffer_allocation_bytes = 0;
    std::size_t working_buffer_reuse_count = 0;
    std::size_t working_buffer_active_bytes = 0;
    std::size_t working_buffer_retained_bytes = 0;
    std::size_t working_buffer_peak_active_bytes = 0;
    std::size_t working_buffer_peak_retained_bytes = 0;
    std::size_t command_buffer_submission_count = 0;
    std::size_t command_buffer_completion_count = 0;
    std::size_t source_direct_write_bytes = 0;
    std::size_t source_legacy_copy_bytes = 0;
    std::size_t plan_direct_write_bytes = 0;
    std::size_t plan_legacy_copy_bytes = 0;
    std::size_t plan_upload_bytes = 0;
    std::size_t analyzed_tile_count = 0;
    std::size_t generic_tile_count = 0;
    std::size_t specialized_tile_count = 0;
    double buffer_allocation_ms = 0.0;
    double working_buffer_allocation_ms = 0.0;
    double source_upload_ms = 0.0;
    double plan_upload_ms = 0.0;
    double source_pack_ms = 0.0;
    double plan_pack_ms = 0.0;
    double buffer_wiring_ms = 0.0;
    double pipeline_creation_ms = 0.0;
    double gpu_execution_ms = 0.0;
    double execution_slot_wait_ms = 0.0;
    bool external_source_zero_copy = false;
    bool shared_uma_path = false;
    std::string fallback_reason;
    std::vector<std::string> created_pipeline_names;
};

struct MetalAnalysisOptions {
    std::size_t tile_size = 32;
    std::size_t reduction_groups_per_candidate = 8;
    std::size_t workspace_limit_elements = 0;
    bool profile_split_kernels = false;
    std::size_t inverse_threads_per_threadgroup = 32;
    MetalKernelDispatchPolicy kernel_dispatch = MetalKernelDispatchPolicy::automatic;
    // Reuse source, workspace, and metric-partial buffers across serialized calls.
    // Requests above the ceiling use transient buffers without changing correctness.
    bool reuse_working_buffers = true;
    std::size_t retained_working_buffer_limit_bytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::size_t execution_slots = 2U;
    // Diagnostic compatibility paths used for direct-vs-legacy conformance.
    bool direct_plan_pack = true;
    bool direct_source_write = true;
};

struct MetalLumaFrameView {
    std::uintptr_t pixel_buffer = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t bit_depth = 8;
    std::string surface_format;
    std::string range = "unknown";
};

[[nodiscard]] bool metal_backend_available() noexcept;

// Accepts single- and two-axis plans through half-bandwidth 15 / forward width 16 and p=1..4.
// Calls are serialized so one engine can safely be reused.
class MetalAnalysisEngine {
public:
    explicit MetalAnalysisEngine(MetalAnalysisOptions options = {});
    ~MetalAnalysisEngine();

    MetalAnalysisEngine(const MetalAnalysisEngine &) = delete;
    MetalAnalysisEngine &operator=(const MetalAnalysisEngine &) = delete;
    MetalAnalysisEngine(MetalAnalysisEngine &&) noexcept;
    MetalAnalysisEngine &operator=(MetalAnalysisEngine &&) noexcept;

    [[nodiscard]] const MetalDeviceInfo &device_info() const noexcept;
    [[nodiscard]] const MetalAnalysisOptions &options() const noexcept;
    [[nodiscard]] std::size_t peak_workspace_elements() const noexcept;
    // Peak bytes across all explicitly allocated Metal buffers, including queued plan tiles.
    [[nodiscard]] std::size_t peak_working_set_bytes() const noexcept;
    [[nodiscard]] MetalRuntimeTelemetry runtime_telemetry() const;
    // Preserves immutable pipeline-creation telemetry and resets per-analysis counters.
    void reset_analysis_telemetry();
    // Releases grow-to-fit source, workspace, and metric buffers. A concurrent
    // analysis finishes before the buffers are released.
    void trim_working_buffers();

    void preflight_axis_batch(
        ConstImageView dimensions,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::size_t concurrency) const;

    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_f32(
        ConstImageView source, std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {},
        const std::function<void(std::size_t completed, std::size_t total)> &progress = {});

    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_metal_luma(
        const MetalLumaFrameView &source,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace getnative

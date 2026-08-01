#pragma once

#include "getnative/cpu_analysis.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace getnative {

inline constexpr std::size_t vulkan_device_uuid_size = 16;
using VulkanDeviceUuid = std::array<std::uint8_t, vulkan_device_uuid_size>;

struct VulkanPciAddress {
    std::uint32_t domain = 0;
    std::uint32_t bus = 0;
    std::uint32_t device = 0;
    std::uint32_t function = 0;

    friend bool operator==(const VulkanPciAddress &, const VulkanPciAddress &) = default;
};

struct VulkanFloatControls {
    bool denorm_preserve_f32 = false;
    bool denorm_flush_to_zero_f32 = false;
    bool signed_zero_inf_nan_preserve_f32 = false;
    bool rounding_mode_rte_f32 = false;
    bool rounding_mode_rtz_f32 = false;
};

struct VulkanDeviceInfo {
    std::string name;
    std::string stable_selector;
    VulkanDeviceUuid uuid{};
    std::optional<VulkanPciAddress> pci_address;
    std::size_t ordinal = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t device_type = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t compute_queue_family = 0;
    std::uint32_t maximum_compute_workgroup_invocations = 0;
    std::uint32_t maximum_storage_buffer_range = 0;
    std::uint32_t maximum_push_constant_bytes = 0;
    std::uint64_t maximum_device_heap_bytes = 0;
    std::uint64_t non_coherent_atom_size = 1;
    float timestamp_period_ns = 0.0F;
    bool timestamp_compute_and_graphics = false;
    bool has_host_coherent_staging = false;
    bool has_non_coherent_staging = false;
    // Device meets Vulkan 1.2 compute limits for the single production analysis path.
    // This is not a multi math-mode flag (no strict/fast-math dual paths).
    bool production_compute_supported = false;
    VulkanFloatControls float_controls{};
};

struct VulkanDeviceSelector {
    std::optional<VulkanDeviceUuid> uuid;
    std::optional<VulkanPciAddress> pci_address;
    std::optional<std::size_t> ordinal;
    std::string exact_name;
};

struct VulkanBackendProbe {
    bool loader_available = false;
    bool instance_available = false;
    std::vector<VulkanDeviceInfo> devices;
    std::string unavailable_reason;
};

enum class VulkanKernelDispatchPolicy : std::uint8_t {
    automatic,
    generic_only,
    required_specialized,
};

// Counters and timings accumulate until reset_analysis_telemetry(). Retained
// byte fields are current-capacity gauges and survive a telemetry reset.
struct VulkanRuntimeTelemetry {
    std::size_t buffer_allocation_count = 0;
    std::size_t buffer_allocation_bytes = 0;
    std::size_t device_buffer_allocation_bytes = 0;
    std::size_t host_buffer_allocation_bytes = 0;
    std::size_t working_buffer_allocation_count = 0;
    std::size_t working_buffer_allocation_bytes = 0;
    std::size_t working_buffer_reuse_count = 0;
    std::size_t working_buffer_active_bytes = 0;
    std::size_t working_buffer_retained_bytes = 0;
    std::size_t working_buffer_peak_active_bytes = 0;
    std::size_t working_buffer_peak_retained_bytes = 0;
    std::size_t peak_device_bytes = 0;
    std::size_t peak_host_bytes = 0;
    std::size_t peak_total_explicit_bytes = 0;
    std::size_t queue_submission_count = 0;
    std::size_t queue_completion_count = 0;
    std::size_t flush_count = 0;
    std::size_t invalidate_count = 0;
    std::size_t flushed_bytes = 0;
    std::size_t invalidated_bytes = 0;
    std::size_t plan_upload_bytes = 0;
    std::size_t partial_readback_bytes = 0;
    std::size_t analyzed_tile_count = 0;
    std::size_t generic_tile_count = 0;
    std::size_t specialized_tile_count = 0;
    std::size_t validation_error_count = 0;
    std::size_t validation_warning_count = 0;
    double buffer_allocation_ms = 0.0;
    double plan_pack_ms = 0.0;
    double source_upload_ms = 0.0;
    double plan_upload_ms = 0.0;
    double descriptor_wiring_ms = 0.0;
    double partial_readback_ms = 0.0;
    double cpu_merge_ms = 0.0;
    double pipeline_creation_ms = 0.0;
    double gpu_execution_ms = 0.0;
    double inverse_image_ms = 0.0;
    double inverse_matrix_ms = 0.0;
    double first_forward_ms = 0.0;
    double metric_ms = 0.0;
    bool gpu_timestamps_available = false;
    bool used_non_coherent_upload = false;
    bool used_non_coherent_readback = false;
    std::vector<std::string> created_pipeline_names;
};

struct VulkanAnalysisOptions {
    std::size_t tile_size = 32;
    std::size_t reduction_groups_per_candidate = 8;
    std::size_t workspace_limit_elements = 0;
    std::size_t inverse_threads_per_workgroup = 32;
    VulkanKernelDispatchPolicy kernel_dispatch = VulkanKernelDispatchPolicy::automatic;
    bool enable_validation = false;
    bool profile_split_kernels = false;
    bool reuse_working_buffers = true;
    std::size_t retained_working_buffer_limit_bytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL;
    VulkanDeviceSelector device_selector{};
    std::wstring loader_path = L"vulkan-1.dll";
};

[[nodiscard]] VulkanBackendProbe probe_vulkan_backend(
    std::wstring_view loader_path = L"vulkan-1.dll",
    bool enable_validation = false);
[[nodiscard]] bool vulkan_backend_available() noexcept;

// Consumes immutable Float32 AxisPlan objects. Calls are serialized so one
// engine can be safely reused after success, cancellation, or host-side error.
class VulkanAnalysisEngine {
public:
    explicit VulkanAnalysisEngine(VulkanAnalysisOptions options = {});
    ~VulkanAnalysisEngine();

    VulkanAnalysisEngine(const VulkanAnalysisEngine &) = delete;
    VulkanAnalysisEngine &operator=(const VulkanAnalysisEngine &) = delete;
    VulkanAnalysisEngine(VulkanAnalysisEngine &&) noexcept;
    VulkanAnalysisEngine &operator=(VulkanAnalysisEngine &&) noexcept;

    [[nodiscard]] const VulkanDeviceInfo &device_info() const noexcept;
    [[nodiscard]] const VulkanAnalysisOptions &options() const noexcept;
    [[nodiscard]] std::size_t peak_workspace_elements() const noexcept;
    [[nodiscard]] std::size_t peak_working_set_bytes() const noexcept;
    [[nodiscard]] VulkanRuntimeTelemetry runtime_telemetry() const;
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

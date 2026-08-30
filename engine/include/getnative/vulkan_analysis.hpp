#pragma once

#include "getnative/cpu_analysis.hpp"
#include "getnative/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace getnative {

inline constexpr std::uint32_t vulkan_minimum_p_norm = 1U;
inline constexpr std::uint32_t vulkan_maximum_p_norm = 4U;
inline constexpr std::int32_t vulkan_automatic_device_index = -1;

enum class VulkanDeviceType : std::uint8_t {
    discrete_gpu,
    integrated_gpu,
    virtual_gpu,
    cpu,
    other,
};

[[nodiscard]] constexpr const char *vulkan_device_type_name(
    VulkanDeviceType type) noexcept {
    switch (type) {
    case VulkanDeviceType::discrete_gpu: return "discrete_gpu";
    case VulkanDeviceType::integrated_gpu: return "integrated_gpu";
    case VulkanDeviceType::virtual_gpu: return "virtual_gpu";
    case VulkanDeviceType::cpu: return "cpu";
    case VulkanDeviceType::other: return "other";
    }
    return "other";
}

struct VulkanDeviceInfo {
    std::int32_t index = -1;
    std::string name;
    std::string uuid;
    std::uint32_t api_version = 0U;
    std::uint32_t driver_version = 0U;
    std::uint32_t vendor_id = 0U;
    std::uint32_t device_id = 0U;
    VulkanDeviceType device_type = VulkanDeviceType::other;
    std::size_t device_local_memory_bytes = 0U;
    std::size_t maximum_storage_buffer_bytes = 0U;
    std::uint32_t maximum_compute_workgroup_invocations = 0U;
    bool backend_compatible = false;
    bool video_decode_available = false;
    std::vector<std::string> video_decode_codecs;
    std::string video_decode_reason;
    std::string incompatibility_reason;
};

struct VulkanRuntimeProbe {
    bool instance_created = false;
    bool device_available = false;
    std::string reason;
    std::vector<VulkanDeviceInfo> devices;
};

struct VulkanRuntimeTelemetry {
    std::size_t command_buffer_submission_count = 0U;
    std::size_t kernel_dispatch_count = 0U;
    std::size_t analyzed_candidate_count = 0U;
    std::size_t tile_count = 0U;
    std::size_t buffer_allocation_count = 0U;
    std::size_t plan_upload_bytes = 0U;
    std::size_t source_upload_bytes = 0U;
    std::size_t source_conversion_bytes = 0U;
    std::size_t source_conversion_count = 0U;
    std::size_t result_readback_bytes = 0U;
    std::size_t workspace_bytes = 0U;
    std::size_t peak_workspace_elements = 0U;
    std::size_t peak_working_set_bytes = 0U;
    std::size_t validation_error_count = 0U;
    double execution_slot_wait_ms = 0.0;
    double host_pack_ms = 0.0;
    double source_conversion_ms = 0.0;
    double gpu_execution_ms = 0.0;
};

struct VulkanNativeContextInfo {
    std::uintptr_t instance = 0U;
    std::uintptr_t physical_device = 0U;
    std::uintptr_t device = 0U;
    std::uintptr_t compute_queue = 0U;
    std::uint32_t compute_queue_family = 0U;
    std::uint32_t decode_queue_family = 0U;
    std::uint32_t video_codec_operations = 0U;
    std::uint32_t instance_api_version = 0U;
    bool timeline_semaphore = false;
    std::vector<std::string> enabled_device_extensions;
};

// The caller retains the decoded image and its timeline semaphore until this
// synchronous analysis call returns. mark_submitted updates FFmpeg's AVVkFrame
// synchronization state and releases its frame lock immediately after queue
// submission; release_without_submit is the exception-path counterpart.
struct VulkanLumaFrameView {
    std::uintptr_t image = 0U;
    std::uint32_t image_format = 0U;
    std::uint32_t view_format = 0U;
    std::uint32_t aspect_mask = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t bit_depth = 8;
    std::int32_t normalized_sample_bits = 8;
    std::uint32_t layout = 0U;
    std::uint32_t access = 0U;
    std::uint32_t queue_family = 0U;
    std::uintptr_t semaphore = 0U;
    std::uint64_t semaphore_value = 0U;
    void *sync_opaque = nullptr;
    void (*mark_submitted)(void *opaque, std::uint32_t layout,
                           std::uint32_t access, std::uint32_t queue_family,
                           std::uint64_t semaphore_value) = nullptr;
    void (*release_without_submit)(void *opaque) = nullptr;
};

struct VulkanAnalysisOptions {
    std::string device_uuid;
    std::int32_t device_index = vulkan_automatic_device_index;
    std::size_t workspace_limit_elements = 0U;
    std::size_t execution_slots = 2U;
    std::uint32_t metric_groups_per_candidate = 128U;
    bool enable_validation = false;
    bool force_non_coherent = false;
};

[[nodiscard]] VulkanRuntimeProbe vulkan_runtime_probe() noexcept;
[[nodiscard]] bool vulkan_backend_available() noexcept;
[[nodiscard]] std::vector<VulkanDeviceInfo> enumerate_vulkan_devices();
[[nodiscard]] std::int32_t select_default_vulkan_device_index(
    std::span<const VulkanDeviceInfo> devices) noexcept;

// The Vulkan path intentionally matches the CUDA batch contract while keeping
// scheduling in the caller. It supports H, V, and H+V plans through
// half-bandwidth 15 / forward width 16, and the strict thresholded p=1 metric.
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
    [[nodiscard]] const VulkanNativeContextInfo &native_context() const noexcept;
    void lock_native_queue();
    void unlock_native_queue() noexcept;

    // Validates storage-buffer and aggregate device-memory requirements for
    // a media task before decoding starts. No command buffer is submitted.
    void preflight_axis_batch(
        ConstImageView dimensions,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::size_t concurrency) const;

    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_f32(
        ConstImageView source, std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {},
        GpuStageProfile profile = GpuStageProfile::off);
    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_vulkan_luma(
        const VulkanLumaFrameView &source,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop = {},
        GpuStageProfile profile = GpuStageProfile::off);

private:
    [[nodiscard]] std::vector<CandidateResult> analyze_axis_batch_impl(
        ConstImageView source, const VulkanLumaFrameView *device_source,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop, GpuStageProfile profile);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace getnative

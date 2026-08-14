#include "getnative/vulkan_analysis.hpp"

#include "getnative_vulkan_forward_spv.hpp"
#include "getnative_vulkan_inverse_spv.hpp"
#include "getnative_vulkan_luma_spv.hpp"
#include "getnative_vulkan_metric_spv.hpp"
#include "getnative_vulkan_transpose_spv.hpp"
#include "vulkan_abi.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace getnative {
namespace {

constexpr std::size_t default_workspace_bytes = 512U * 1024U * 1024U;

[[nodiscard]] VulkanDeviceType device_type_from_vk(
    VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return VulkanDeviceType::discrete_gpu;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return VulkanDeviceType::integrated_gpu;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return VulkanDeviceType::virtual_gpu;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return VulkanDeviceType::cpu;
    default:
        return VulkanDeviceType::other;
    }
}

[[nodiscard]] constexpr std::uint8_t device_type_rank(
    VulkanDeviceType type) noexcept {
    switch (type) {
    case VulkanDeviceType::discrete_gpu: return 0U;
    case VulkanDeviceType::integrated_gpu: return 1U;
    case VulkanDeviceType::virtual_gpu: return 2U;
    case VulkanDeviceType::cpu: return 3U;
    case VulkanDeviceType::other: return 4U;
    }
    return 4U;
}

[[nodiscard]] bool better_default_device(
    const VulkanDeviceInfo &left, const VulkanDeviceInfo &right) noexcept {
    if (left.backend_compatible != right.backend_compatible) {
        return left.backend_compatible;
    }
    const std::uint8_t left_rank = device_type_rank(left.device_type);
    const std::uint8_t right_rank = device_type_rank(right.device_type);
    if (left_rank != right_rank) return left_rank < right_rank;
    if (left.device_local_memory_bytes != right.device_local_memory_bytes) {
        return left.device_local_memory_bytes > right.device_local_memory_bytes;
    }
    return left.index < right.index;
}

[[nodiscard]] const char *vk_result_name(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "unknown VkResult";
    }
}

void check_vk(VkResult result, const char *operation) {
    if (result == VK_SUCCESS) return;
    throw std::runtime_error(
        std::string{operation} + " failed with " + vk_result_name(result)
        + " (" + std::to_string(static_cast<int>(result)) + ")");
}

[[nodiscard]] std::size_t checked_add(
    std::size_t left, std::size_t right, const char *label) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::length_error(std::string{label} + " exceeds addressable memory");
    }
    return left + right;
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left, std::size_t right, const char *label) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::length_error(std::string{label} + " exceeds addressable memory");
    }
    return left * right;
}

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, const char *label) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string{label} + " exceeds Vulkan shader ABI limits");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t divide_up(
    std::uint32_t value, std::uint32_t divisor) noexcept {
    return value / divisor + (value % divisor != 0U ? 1U : 0U);
}

template <class Handle>
[[nodiscard]] Handle native_handle(std::uintptr_t value) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}

template <class Handle>
[[nodiscard]] std::uintptr_t native_value(Handle value) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<std::uintptr_t>(value);
    } else {
        return static_cast<std::uintptr_t>(value);
    }
}

[[nodiscard]] bool has_instance_layer(const char *name) {
    std::uint32_t count = 0U;
    check_vk(vkEnumerateInstanceLayerProperties(&count, nullptr),
             "vkEnumerateInstanceLayerProperties");
    std::vector<VkLayerProperties> layers(count);
    check_vk(vkEnumerateInstanceLayerProperties(&count, layers.data()),
             "vkEnumerateInstanceLayerProperties");
    return std::ranges::any_of(layers, [name](const VkLayerProperties &layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

[[nodiscard]] bool has_instance_extension(const char *name) {
    std::uint32_t count = 0U;
    check_vk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
             "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> extensions(count);
    check_vk(vkEnumerateInstanceExtensionProperties(
                 nullptr, &count, extensions.data()),
             "vkEnumerateInstanceExtensionProperties");
    return std::ranges::any_of(
        extensions, [name](const VkExtensionProperties &extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

[[nodiscard]] std::string uuid_string(const std::uint8_t *bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(32U, '0');
    for (std::size_t index = 0U; index < VK_UUID_SIZE; ++index) {
        result[index * 2U] = hex[bytes[index] >> 4U];
        result[index * 2U + 1U] = hex[bytes[index] & 0x0fU];
    }
    return result;
}

struct InstanceHandle {
    VkInstance value = VK_NULL_HANDLE;
    std::uint32_t api_version = VK_API_VERSION_1_0;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = nullptr;

    ~InstanceHandle() {
        if (messenger != VK_NULL_HANDLE && destroy_messenger != nullptr) {
            destroy_messenger(value, messenger, nullptr);
        }
        if (value != VK_NULL_HANDLE) vkDestroyInstance(value, nullptr);
    }
    InstanceHandle() = default;
    InstanceHandle(const InstanceHandle &) = delete;
    InstanceHandle &operator=(const InstanceHandle &) = delete;
};

VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user_data) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
        && user_data != nullptr) {
        static_cast<std::atomic<std::size_t> *>(user_data)->fetch_add(
            1U, std::memory_order_relaxed);
    }
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "GetNative Vulkan validation: %s\n",
                     data != nullptr && data->pMessage != nullptr
                         ? data->pMessage : "unknown message");
    }
    return VK_FALSE;
}

void create_instance(InstanceHandle &instance, bool validation,
                     std::atomic<std::size_t> *validation_errors) {
    const char *validation_layer = "VK_LAYER_KHRONOS_validation";
    if (validation && !has_instance_layer(validation_layer)) {
        throw std::runtime_error(
            "Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
    }
    const bool debug_utils = validation
        && has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const std::array<const char *, 1U> layers{validation_layer};
    const std::array<const char *, 1U> extensions{
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    if (const auto enumerate_version =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"))) {
        check_vk(enumerate_version(&loader_version), "vkEnumerateInstanceVersion");
    }
    instance.api_version = std::min(loader_version, VK_API_VERSION_1_3);
    const VkApplicationInfo application{
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
        "GetNative", VK_MAKE_API_VERSION(0, 0, 1, 0),
        "GetNative Vulkan analysis", VK_MAKE_API_VERSION(0, 0, 1, 0),
        instance.api_version,
    };
    const VkInstanceCreateInfo create_info{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0U, &application,
        validation ? 1U : 0U, validation ? layers.data() : nullptr,
        debug_utils ? 1U : 0U, debug_utils ? extensions.data() : nullptr,
    };
    check_vk(vkCreateInstance(&create_info, nullptr, &instance.value),
             "vkCreateInstance");

    if (!debug_utils) return;
    const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance.value, "vkCreateDebugUtilsMessengerEXT"));
    instance.destroy_messenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance.value, "vkDestroyDebugUtilsMessengerEXT"));
    if (create_messenger == nullptr || instance.destroy_messenger == nullptr) return;
    const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        nullptr,
        0U,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        validation_callback,
        validation_errors,
    };
    check_vk(create_messenger(
                 instance.value, &messenger_info, nullptr, &instance.messenger),
             "vkCreateDebugUtilsMessengerEXT");
}

struct DeviceRecord {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VulkanDeviceInfo info;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::optional<std::uint32_t> compute_queue_family;
    std::optional<std::uint32_t> decode_queue_family;
    VkVideoCodecOperationFlagsKHR video_codec_operations = 0U;
    bool timeline_semaphore = false;
    std::vector<std::string> enabled_video_extensions;
};

[[nodiscard]] std::vector<VkExtensionProperties> device_extensions(
    VkPhysicalDevice physical) {
    std::uint32_t count = 0U;
    check_vk(vkEnumerateDeviceExtensionProperties(
                 physical, nullptr, &count, nullptr),
             "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> result(count);
    check_vk(vkEnumerateDeviceExtensionProperties(
                 physical, nullptr, &count, result.data()),
             "vkEnumerateDeviceExtensionProperties");
    return result;
}

[[nodiscard]] bool has_device_extension(
    std::span<const VkExtensionProperties> extensions, const char *name) {
    return std::ranges::any_of(
        extensions, [name](const VkExtensionProperties &extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

[[nodiscard]] std::vector<DeviceRecord> device_records(VkInstance instance) {
    std::uint32_t count = 0U;
    check_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
             "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> physical_devices(count);
    check_vk(vkEnumeratePhysicalDevices(
                 instance, &count, physical_devices.data()),
             "vkEnumeratePhysicalDevices");

    std::vector<DeviceRecord> result;
    result.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
        DeviceRecord record;
        record.physical = physical_devices[index];
        VkPhysicalDeviceIDProperties id_properties{};
        id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &id_properties;
        vkGetPhysicalDeviceProperties2(record.physical, &properties2);
        record.properties = properties2.properties;
        vkGetPhysicalDeviceMemoryProperties(record.physical, &record.memory);

        std::uint32_t family_count = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties2(
            record.physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties2> families(family_count);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video_families(family_count);
        for (std::uint32_t family = 0U; family < family_count; ++family) {
            families[family].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            video_families[family].sType =
                VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            families[family].pNext = &video_families[family];
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(
            record.physical, &family_count, families.data());
        for (std::uint32_t family = 0U; family < family_count; ++family) {
            const VkQueueFamilyProperties &properties =
                families[family].queueFamilyProperties;
            if (properties.queueCount != 0U
                && (properties.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                record.compute_queue_family = family;
            }
            if (properties.queueCount != 0U
                && (properties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0U
                && video_families[family].videoCodecOperations != 0U) {
                record.decode_queue_family = family;
                record.video_codec_operations =
                    video_families[family].videoCodecOperations;
            }
        }
        for (std::uint32_t family = 0U; family < family_count; ++family) {
            const VkQueueFamilyProperties &properties =
                families[family].queueFamilyProperties;
            if (properties.queueCount != 0U
                && (properties.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U
                && (properties.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                record.compute_queue_family = family;
                break;
            }
        }

        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
        timeline.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &timeline;
        vkGetPhysicalDeviceFeatures2(record.physical, &features);
        record.timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;

        const std::vector<VkExtensionProperties> extensions =
            device_extensions(record.physical);
        const bool video_base_extensions =
            has_device_extension(extensions, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME)
            && has_device_extension(
                extensions, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        if (video_base_extensions) {
            record.enabled_video_extensions = {
                VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
            };
            const auto add_codec = [&](VkVideoCodecOperationFlagBitsKHR operation,
                                       const char *extension) {
                const VkVideoCodecOperationFlagsKHR operation_flags =
                    static_cast<VkVideoCodecOperationFlagsKHR>(operation);
                if ((record.video_codec_operations & operation_flags) != 0U
                    && has_device_extension(extensions, extension)) {
                    record.enabled_video_extensions.emplace_back(extension);
                } else {
                    record.video_codec_operations &= ~operation_flags;
                }
            };
            add_codec(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
                      VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
            add_codec(VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
                      VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
            add_codec(VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
                      VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
#if defined(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)
            add_codec(VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
                      VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
#endif
        } else {
            record.video_codec_operations = 0U;
        }

        VulkanDeviceInfo &info = record.info;
        info.index = static_cast<std::int32_t>(index);
        info.name = record.properties.deviceName;
        info.uuid = uuid_string(id_properties.deviceUUID);
        info.api_version = record.properties.apiVersion;
        info.driver_version = record.properties.driverVersion;
        info.vendor_id = record.properties.vendorID;
        info.device_id = record.properties.deviceID;
        info.device_type = device_type_from_vk(record.properties.deviceType);
        info.maximum_storage_buffer_bytes = static_cast<std::size_t>(
            record.properties.limits.maxStorageBufferRange);
        info.maximum_compute_workgroup_invocations =
            record.properties.limits.maxComputeWorkGroupInvocations;
        const auto add_video_codec = [&](VkVideoCodecOperationFlagBitsKHR operation,
                                         const char *name) {
            if ((record.video_codec_operations
                 & static_cast<VkVideoCodecOperationFlagsKHR>(operation)) != 0U) {
                info.video_decode_codecs.emplace_back(name);
            }
        };
        add_video_codec(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "h264");
        add_video_codec(VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "hevc");
        add_video_codec(VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "av1");
#if defined(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)
        add_video_codec(VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "vp9");
#endif
        if (VK_API_VERSION_MAJOR(info.api_version) < 1U
            || (VK_API_VERSION_MAJOR(info.api_version) == 1U
                && VK_API_VERSION_MINOR(info.api_version) < 3U)) {
            info.video_decode_reason = "Vulkan Video requires Vulkan 1.3";
        } else if (!record.timeline_semaphore) {
            info.video_decode_reason = "timeline semaphores are unavailable";
        } else if (!record.decode_queue_family.has_value()) {
            info.video_decode_reason = "no Vulkan Video decode queue family";
        } else if (record.video_codec_operations == 0U) {
            info.video_decode_reason = "no supported Vulkan Video codec extension";
        } else {
            info.video_decode_available = true;
        }
        for (std::uint32_t heap = 0U; heap < record.memory.memoryHeapCount; ++heap) {
            if ((record.memory.memoryHeaps[heap].flags
                 & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
                info.device_local_memory_bytes = checked_add(
                    info.device_local_memory_bytes,
                    static_cast<std::size_t>(record.memory.memoryHeaps[heap].size),
                    "Vulkan device memory size");
            }
        }

        auto incompatible = [&](std::string reason) {
            if (info.incompatibility_reason.empty()) {
                info.incompatibility_reason = std::move(reason);
            }
        };
        if (VK_API_VERSION_MAJOR(info.api_version) < 1U
            || (VK_API_VERSION_MAJOR(info.api_version) == 1U
                && VK_API_VERSION_MINOR(info.api_version) < 2U)) {
            incompatible("Vulkan 1.2 is required");
        }
        if (!record.compute_queue_family.has_value()) {
            incompatible("no compute queue family");
        }
        if (record.properties.limits.maxComputeWorkGroupInvocations < 256U
            || record.properties.limits.maxComputeWorkGroupSize[0] < 256U) {
            incompatible("compute workgroups with 256 invocations are required");
        }
        if (record.properties.limits.maxComputeWorkGroupCount[1]
            < vulkan_detail::maximum_tile_candidates) {
            incompatible("compute Y dimension is too small for candidate tiles");
        }
        if (record.properties.limits.maxPushConstantsSize < 128U) {
            incompatible("128-byte push constants are required");
        }
        if (record.properties.limits.maxStorageBufferRange < 1024U * 1024U) {
            incompatible("maxStorageBufferRange is below 1 MiB");
        }
        info.backend_compatible = info.incompatibility_reason.empty();
        result.push_back(std::move(record));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return better_default_device(left.info, right.info);
    });
    return result;
}

class Context {
public:
    Context(const VulkanAnalysisOptions &options, std::size_t slot_count) {
        create_instance(instance, options.enable_validation, &validation_errors);
        instance_api_version = instance.api_version;
        std::vector<DeviceRecord> records = device_records(instance.value);
        auto selected = records.end();
        if (!options.device_uuid.empty()) {
            selected = std::find_if(records.begin(), records.end(), [&](const auto &record) {
                return record.info.uuid == options.device_uuid;
            });
        } else if (options.device_index == vulkan_automatic_device_index) {
            selected = std::find_if(records.begin(), records.end(), [](const auto &record) {
                return record.info.backend_compatible;
            });
        } else {
            selected = std::find_if(records.begin(), records.end(), [&](const auto &record) {
                return record.info.index == options.device_index;
            });
        }
        if (selected == records.end()) {
            throw std::runtime_error("the requested Vulkan device was not found");
        }
        if (!selected->info.backend_compatible) {
            throw std::runtime_error(
                "the requested Vulkan device is incompatible: "
                + selected->info.incompatibility_reason);
        }
        physical = selected->physical;
        info = selected->info;
        properties = selected->properties;
        memory = selected->memory;
        queue_family = *selected->compute_queue_family;
        decode_queue_family = selected->decode_queue_family.value_or(queue_family);
        video_codec_operations = selected->video_codec_operations;
        enabled_device_extensions = selected->enabled_video_extensions;
        timeline_semaphore = selected->timeline_semaphore;
        std::uint32_t family_count = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_properties(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical, &family_count, queue_properties.data());
        if (queue_family < queue_properties.size()) {
            timestamp_valid_bits =
                queue_properties[queue_family].timestampValidBits;
        }

        constexpr float priority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        queue_infos.push_back(VkDeviceQueueCreateInfo{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0U,
            queue_family, 1U, &priority});
        if (decode_queue_family != queue_family
            && selected->decode_queue_family.has_value()) {
            queue_infos.push_back(VkDeviceQueueCreateInfo{
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0U,
                decode_queue_family, 1U, &priority});
        }
        std::vector<const char *> extension_names;
        extension_names.reserve(enabled_device_extensions.size());
        for (const std::string &extension : enabled_device_extensions) {
            extension_names.push_back(extension.c_str());
        }
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
        timeline.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timeline.timelineSemaphore = timeline_semaphore ? VK_TRUE : VK_FALSE;
        const VkDeviceCreateInfo device_info{
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            timeline_semaphore ? &timeline : nullptr, 0U,
            static_cast<std::uint32_t>(queue_infos.size()), queue_infos.data(),
            0U, nullptr, static_cast<std::uint32_t>(extension_names.size()),
            extension_names.data(), nullptr};
        check_vk(vkCreateDevice(physical, &device_info, nullptr, &device),
                 "vkCreateDevice");
        vkGetDeviceQueue(device, queue_family, 0U, &queue);

        try {
            std::array<VkDescriptorSetLayoutBinding, 5U> bindings{};
            for (std::uint32_t index = 0U; index < 4U; ++index) {
                bindings[index] = VkDescriptorSetLayoutBinding{
                    index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            }
            bindings[4] = VkDescriptorSetLayoutBinding{
                4U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            const VkDescriptorSetLayoutCreateInfo descriptor_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0U,
                static_cast<std::uint32_t>(bindings.size()), bindings.data()};
            check_vk(vkCreateDescriptorSetLayout(
                         device, &descriptor_info, nullptr, &descriptor_layout),
                     "vkCreateDescriptorSetLayout");
            const VkPushConstantRange push_range{
                VK_SHADER_STAGE_COMPUTE_BIT, 0U, 128U};
            const VkPipelineLayoutCreateInfo pipeline_layout_info{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0U,
                1U, &descriptor_layout, 1U, &push_range};
            check_vk(vkCreatePipelineLayout(
                         device, &pipeline_layout_info, nullptr, &pipeline_layout),
                     "vkCreatePipelineLayout");
            VkPipelineCacheCreateInfo cache_info{};
            cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            check_vk(vkCreatePipelineCache(
                         device, &cache_info, nullptr, &pipeline_cache),
                     "vkCreatePipelineCache");

            transpose_pipeline = create_pipeline(
                vulkan_detail::embedded::getnative_vulkan_transpose_spv,
                sizeof(vulkan_detail::embedded::getnative_vulkan_transpose_spv));
            inverse_pipeline = create_pipeline(
                vulkan_detail::embedded::getnative_vulkan_inverse_spv,
                sizeof(vulkan_detail::embedded::getnative_vulkan_inverse_spv));
            forward_pipeline = create_pipeline(
                vulkan_detail::embedded::getnative_vulkan_forward_spv,
                sizeof(vulkan_detail::embedded::getnative_vulkan_forward_spv));
            metric_pipeline = create_pipeline(
                vulkan_detail::embedded::getnative_vulkan_metric_spv,
                sizeof(vulkan_detail::embedded::getnative_vulkan_metric_spv));
            luma_pipeline = create_pipeline(
                vulkan_detail::embedded::getnative_vulkan_luma_spv,
                sizeof(vulkan_detail::embedded::getnative_vulkan_luma_spv));

            const VkSamplerCreateInfo sampler_info{
                VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0U,
                VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                VK_SAMPLER_MIPMAP_MODE_NEAREST,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                0.0F, VK_FALSE, 1.0F, VK_FALSE,
                VK_COMPARE_OP_NEVER, 0.0F, 0.0F,
                VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
            check_vk(vkCreateSampler(
                         device, &sampler_info, nullptr, &luma_sampler),
                     "vkCreateSampler(luma)");

            const std::array<VkDescriptorPoolSize, 2U> pool_sizes{{
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 checked_u32(slot_count * 4U, "Vulkan storage descriptor count")},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 checked_u32(slot_count, "Vulkan luma descriptor count")},
            }};
            const VkDescriptorPoolCreateInfo pool_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0U,
                checked_u32(slot_count, "Vulkan slot count"),
                static_cast<std::uint32_t>(pool_sizes.size()), pool_sizes.data()};
            check_vk(vkCreateDescriptorPool(
                         device, &pool_info, nullptr, &descriptor_pool),
                     "vkCreateDescriptorPool");
        } catch (...) {
            destroy_device_objects();
            throw;
        }
    }

    ~Context() { destroy_device_objects(); }
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    [[nodiscard]] std::uint32_t memory_type(
        std::uint32_t bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) const {
        std::optional<std::uint32_t> fallback;
        for (std::uint32_t index = 0U; index < memory.memoryTypeCount; ++index) {
            if ((bits & (1U << index)) == 0U) continue;
            const VkMemoryPropertyFlags flags = memory.memoryTypes[index].propertyFlags;
            if ((flags & required) != required) continue;
            if ((flags & preferred) == preferred) return index;
            if (!fallback.has_value()) fallback = index;
        }
        if (fallback.has_value()) return *fallback;
        throw std::runtime_error("no compatible Vulkan memory type");
    }

    void submit(VkCommandBuffer command, VkFence fence,
                const VulkanLumaFrameView *source = nullptr) {
        VkTimelineSemaphoreSubmitInfo timeline_info{};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        std::uint64_t signal_value = 0U;
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (source != nullptr) {
            semaphore = native_handle<VkSemaphore>(source->semaphore);
            if (source->semaphore_value
                == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Vulkan frame semaphore value overflow");
            }
            signal_value = source->semaphore_value + 1U;
            timeline_info = VkTimelineSemaphoreSubmitInfo{
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,
                1U, &source->semaphore_value, 1U, &signal_value};
        }
        const VkSubmitInfo submit_info{
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
            source != nullptr ? &timeline_info : nullptr,
            source != nullptr ? 1U : 0U,
            source != nullptr ? &semaphore : nullptr,
            source != nullptr ? &wait_stage : nullptr,
            1U, &command,
            source != nullptr ? 1U : 0U,
            source != nullptr ? &semaphore : nullptr};
        std::scoped_lock lock(queue_mutex);
        check_vk(vkQueueSubmit(queue, 1U, &submit_info, fence), "vkQueueSubmit");
    }

    void lock_queue() { queue_mutex.lock(); }
    void unlock_queue() noexcept { queue_mutex.unlock(); }

    InstanceHandle instance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0U;
    std::uint32_t decode_queue_family = 0U;
    VkVideoCodecOperationFlagsKHR video_codec_operations = 0U;
    std::uint32_t instance_api_version = VK_API_VERSION_1_0;
    bool timeline_semaphore = false;
    std::uint32_t timestamp_valid_bits = 0U;
    std::vector<std::string> enabled_device_extensions;
    VulkanDeviceInfo info;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    VkPipeline transpose_pipeline = VK_NULL_HANDLE;
    VkPipeline inverse_pipeline = VK_NULL_HANDLE;
    VkPipeline forward_pipeline = VK_NULL_HANDLE;
    VkPipeline metric_pipeline = VK_NULL_HANDLE;
    VkPipeline luma_pipeline = VK_NULL_HANDLE;
    VkSampler luma_sampler = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::atomic<std::size_t> validation_errors{0U};

private:
    [[nodiscard]] VkPipeline create_pipeline(
        const std::uint32_t *code, std::size_t bytes) const {
        const VkShaderModuleCreateInfo module_info{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0U,
            bytes, code};
        VkShaderModule module = VK_NULL_HANDLE;
        check_vk(vkCreateShaderModule(device, &module_info, nullptr, &module),
                 "vkCreateShaderModule");
        try {
            const VkPipelineShaderStageCreateInfo stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0U,
                VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            const VkComputePipelineCreateInfo pipeline_info{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0U,
                stage, pipeline_layout, VK_NULL_HANDLE, -1};
            VkPipeline pipeline = VK_NULL_HANDLE;
            check_vk(vkCreateComputePipelines(
                         device, pipeline_cache, 1U, &pipeline_info,
                         nullptr, &pipeline),
                     "vkCreateComputePipelines");
            vkDestroyShaderModule(device, module, nullptr);
            return pipeline;
        } catch (...) {
            vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
    }

    void destroy_device_objects() noexcept {
        if (device == VK_NULL_HANDLE) return;
        (void)vkDeviceWaitIdle(device);
        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        }
        if (metric_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, metric_pipeline, nullptr);
        }
        if (luma_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, luma_pipeline, nullptr);
        }
        if (luma_sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, luma_sampler, nullptr);
        }
        if (forward_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, forward_pipeline, nullptr);
        }
        if (inverse_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, inverse_pipeline, nullptr);
        }
        if (transpose_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, transpose_pipeline, nullptr);
        }
        if (pipeline_cache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(device, pipeline_cache, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        }
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    std::mutex queue_mutex;
};

class Buffer {
public:
    Buffer() = default;
    ~Buffer() { reset(); }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept { swap(other); }
    Buffer &operator=(Buffer &&other) noexcept {
        if (this != &other) {
            Buffer temporary(std::move(other));
            swap(temporary);
        }
        return *this;
    }

    void reserve(Context &context, VkDeviceSize bytes, VkBufferUsageFlags usage,
                 VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                 bool force_non_coherent, std::size_t &allocation_count,
                 VkDeviceSize growth_ceiling = 0U) {
        if (bytes == 0U) bytes = 4U;
        if (capacity_ >= bytes && usage_ == usage && required_ == required) return;
        constexpr VkDeviceSize maximum_growth = 128U * 1024U * 1024U;
        const VkDeviceSize growth = capacity_ == 0U
            ? std::min(bytes / 4U, maximum_growth)
            : std::min(capacity_ / 2U, maximum_growth);
        VkDeviceSize target = bytes;
        const VkDeviceSize base = capacity_ == 0U ? bytes : capacity_;
        if (growth <= std::numeric_limits<VkDeviceSize>::max() - base) {
            target = std::max(target, base + growth);
        }
        if (growth_ceiling != 0U) target = std::min(target, growth_ceiling);
        target = std::max(target, bytes);
        Buffer replacement;
        replacement.context_ = &context;
        replacement.capacity_ = target;
        replacement.usage_ = usage;
        replacement.required_ = required;
        const VkBufferCreateInfo buffer_info{
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0U, target, usage,
            VK_SHARING_MODE_EXCLUSIVE, 0U, nullptr};
        check_vk(vkCreateBuffer(
                     context.device, &buffer_info, nullptr, &replacement.buffer_),
                 "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context.device, replacement.buffer_, &requirements);
        replacement.allocation_size_ = requirements.size;
        const std::uint32_t type = context.memory_type(
            requirements.memoryTypeBits, required, preferred);
        const VkMemoryAllocateInfo allocation_info{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
            requirements.size, type};
        check_vk(vkAllocateMemory(
                     context.device, &allocation_info, nullptr, &replacement.memory_),
                 "vkAllocateMemory");
        check_vk(vkBindBufferMemory(
                     context.device, replacement.buffer_, replacement.memory_, 0U),
                 "vkBindBufferMemory");
        const VkMemoryPropertyFlags flags =
            context.memory.memoryTypes[type].propertyFlags;
        replacement.coherent_ =
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U
            && !force_non_coherent;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
            check_vk(vkMapMemory(
                         context.device, replacement.memory_, 0U,
                         requirements.size, 0U, &replacement.mapped_),
                     "vkMapMemory");
        }
        swap(replacement);
        ++allocation_count;
    }

    void flush() const {
        if (mapped_ == nullptr || coherent_) return;
        const VkMappedMemoryRange range{
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,
            memory_, 0U, VK_WHOLE_SIZE};
        check_vk(vkFlushMappedMemoryRanges(context_->device, 1U, &range),
                 "vkFlushMappedMemoryRanges");
    }

    void invalidate() const {
        if (mapped_ == nullptr || coherent_) return;
        const VkMappedMemoryRange range{
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,
            memory_, 0U, VK_WHOLE_SIZE};
        check_vk(vkInvalidateMappedMemoryRanges(context_->device, 1U, &range),
                 "vkInvalidateMappedMemoryRanges");
    }

    [[nodiscard]] VkBuffer get() const noexcept { return buffer_; }
    [[nodiscard]] void *mapped() const noexcept { return mapped_; }
    [[nodiscard]] VkDeviceSize capacity() const noexcept { return capacity_; }

private:
    void reset() noexcept {
        if (context_ == nullptr) return;
        if (mapped_ != nullptr) vkUnmapMemory(context_->device, memory_);
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(context_->device, buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) vkFreeMemory(context_->device, memory_, nullptr);
        context_ = nullptr;
        buffer_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        mapped_ = nullptr;
        capacity_ = 0U;
        allocation_size_ = 0U;
    }

    void swap(Buffer &other) noexcept {
        std::swap(context_, other.context_);
        std::swap(buffer_, other.buffer_);
        std::swap(memory_, other.memory_);
        std::swap(mapped_, other.mapped_);
        std::swap(capacity_, other.capacity_);
        std::swap(allocation_size_, other.allocation_size_);
        std::swap(usage_, other.usage_);
        std::swap(required_, other.required_);
        std::swap(coherent_, other.coherent_);
    }

    Context *context_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void *mapped_ = nullptr;
    VkDeviceSize capacity_ = 0U;
    VkDeviceSize allocation_size_ = 0U;
    VkBufferUsageFlags usage_ = 0U;
    VkMemoryPropertyFlags required_ = 0U;
    bool coherent_ = false;
};

class ImageViewHandle {
public:
    ImageViewHandle() = default;
    ~ImageViewHandle() { reset(); }
    ImageViewHandle(const ImageViewHandle &) = delete;
    ImageViewHandle &operator=(const ImageViewHandle &) = delete;

    void create(VkDevice device, const VulkanLumaFrameView &source) {
        reset();
        device_ = device;
        const VkImageViewCreateInfo view_info{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            nullptr,
            0U,
            native_handle<VkImage>(source.image),
            VK_IMAGE_VIEW_TYPE_2D,
            static_cast<VkFormat>(source.view_format),
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            {static_cast<VkImageAspectFlags>(source.aspect_mask), 0U, 1U, 0U, 1U},
        };
        check_vk(vkCreateImageView(device_, &view_info, nullptr, &value_),
                 "vkCreateImageView(decoded luma)");
    }

    [[nodiscard]] VkImageView get() const noexcept { return value_; }

private:
    void reset() noexcept {
        if (value_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, value_, nullptr);
        }
        device_ = VK_NULL_HANDLE;
        value_ = VK_NULL_HANDLE;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkImageView value_ = VK_NULL_HANDLE;
};

struct ExecutionSlot {
    explicit ExecutionSlot(Context &context) : context_(&context) {
        const VkCommandPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            context.queue_family};
        check_vk(vkCreateCommandPool(
                     context.device, &pool_info, nullptr, &command_pool),
                 "vkCreateCommandPool");
        try {
            const VkCommandBufferAllocateInfo command_info{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1U};
            check_vk(vkAllocateCommandBuffers(
                         context.device, &command_info, &command),
                     "vkAllocateCommandBuffers");
            const VkFenceCreateInfo fence_info{
                VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                VK_FENCE_CREATE_SIGNALED_BIT};
            check_vk(vkCreateFence(
                         context.device, &fence_info, nullptr, &fence),
                     "vkCreateFence");
            if (context.timestamp_valid_bits != 0U) {
                const VkQueryPoolCreateInfo query_info{
                    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0U,
                    VK_QUERY_TYPE_TIMESTAMP, 2U, 0U};
                check_vk(vkCreateQueryPool(
                             context.device, &query_info, nullptr,
                             &conversion_query_pool),
                         "vkCreateQueryPool(luma timestamps)");
            }
            const VkDescriptorSetAllocateInfo descriptor_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                context.descriptor_pool, 1U, &context.descriptor_layout};
            check_vk(vkAllocateDescriptorSets(
                         context.device, &descriptor_info, &descriptor_set),
                     "vkAllocateDescriptorSets");
        } catch (...) {
            if (conversion_query_pool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(
                    context.device, conversion_query_pool, nullptr);
            }
            if (fence != VK_NULL_HANDLE) vkDestroyFence(context.device, fence, nullptr);
            vkDestroyCommandPool(context.device, command_pool, nullptr);
            throw;
        }
    }

    ~ExecutionSlot() {
        if (conversion_query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(
                context_->device, conversion_query_pool, nullptr);
        }
        if (fence != VK_NULL_HANDLE) vkDestroyFence(context_->device, fence, nullptr);
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_->device, command_pool, nullptr);
        }
    }

    Context *context_ = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkQueryPool conversion_query_pool = VK_NULL_HANDLE;
    Buffer host_plan;
    Buffer host_source;
    Buffer host_partials;
    Buffer device_plan;
    Buffer device_source;
    Buffer device_workspace;
    Buffer device_partials;
};

void validate_axis_plan(const AxisPlan &plan) {
    if (!plan.valid() || plan.half_bandwidth < 0 || plan.half_bandwidth > 15
        || plan.forward_width <= 0 || plan.forward_width > 16) {
        throw std::invalid_argument(
            "Vulkan requires a valid plan with half-bandwidth <= 15 and forward width <= 16");
    }
    const std::size_t rows = static_cast<std::size_t>(plan.source_size);
    const std::size_t width = static_cast<std::size_t>(plan.forward_width);
    for (std::size_t row = 0U; row < rows; ++row) {
        const std::int32_t left = plan.forward_indices[row * width];
        for (std::size_t tap = 0U; tap < width; ++tap) {
            const std::int64_t expected = static_cast<std::int64_t>(left)
                + static_cast<std::int64_t>(tap);
            if (expected < 0 || expected >= plan.destination_size
                || plan.forward_indices[row * width + tap] != expected) {
                throw std::invalid_argument("Vulkan received an invalid forward plan row");
            }
        }
    }
    if (plan.transpose_offsets.empty() || plan.transpose_offsets.front() != 0U
        || plan.transpose_offsets.back() != plan.transpose_indices.size()
        || plan.transpose_indices.size() != plan.transpose_weights.size()) {
        throw std::invalid_argument("Vulkan received invalid transpose plan arrays");
    }
    for (std::size_t index = 1U; index < plan.transpose_offsets.size(); ++index) {
        if (plan.transpose_offsets[index] < plan.transpose_offsets[index - 1U]) {
            throw std::invalid_argument(
                "Vulkan received non-monotonic transpose offsets");
        }
    }
    if (std::ranges::any_of(plan.transpose_indices, [&](std::int32_t index) {
            return index < 0 || index >= plan.source_size;
        })) {
        throw std::invalid_argument("Vulkan received an invalid transpose index");
    }
    const auto finite = [](const auto &values) {
        return std::ranges::all_of(values, [](float value) {
            return std::isfinite(value);
        });
    };
    if (!finite(plan.forward_weights) || !finite(plan.transpose_weights)
        || !finite(plan.lower_ld) || !finite(plan.upper_l)
        || !finite(plan.inverse_diagonal)) {
        throw std::invalid_argument("Vulkan received non-finite plan coefficients");
    }
}

template <class Value>
[[nodiscard]] std::uint32_t append_words(
    std::vector<std::uint32_t> &destination, const std::vector<Value> &source) {
    static_assert(sizeof(Value) == sizeof(std::uint32_t));
    const std::uint32_t base = checked_u32(destination.size(), "Vulkan plan offset");
    destination.reserve(checked_add(
        destination.size(), source.size(), "Vulkan packed plan"));
    for (const Value value : source) {
        destination.push_back(std::bit_cast<std::uint32_t>(value));
    }
    return base;
}

[[nodiscard]] vulkan_detail::AxisPlanDescriptor pack_axis(
    std::vector<std::uint32_t> &words, const AxisPlan &plan) {
    validate_axis_plan(plan);
    vulkan_detail::AxisPlanDescriptor descriptor;
    descriptor.source_size = static_cast<std::uint32_t>(plan.source_size);
    descriptor.destination_size = static_cast<std::uint32_t>(plan.destination_size);
    descriptor.half_bandwidth = static_cast<std::uint32_t>(plan.half_bandwidth);
    descriptor.forward_width = static_cast<std::uint32_t>(plan.forward_width);

    std::vector<std::int32_t> forward_left(
        static_cast<std::size_t>(plan.source_size));
    std::vector<float> forward_weights(plan.forward_weights.size());
    const std::size_t rows = forward_left.size();
    const std::size_t width = static_cast<std::size_t>(plan.forward_width);
    for (std::size_t row = 0U; row < rows; ++row) {
        forward_left[row] = plan.forward_indices[row * width];
        for (std::size_t tap = 0U; tap < width; ++tap) {
            forward_weights[tap * rows + row] = plan.forward_weights[row * width + tap];
        }
    }
    descriptor.forward_left_base = append_words(words, forward_left);
    descriptor.forward_weights_base = append_words(words, forward_weights);
    descriptor.transpose_offsets_base = append_words(words, plan.transpose_offsets);
    descriptor.transpose_indices_base = append_words(words, plan.transpose_indices);
    descriptor.transpose_weights_base = append_words(words, plan.transpose_weights);
    descriptor.lower_ld_base = append_words(words, plan.lower_ld);
    descriptor.upper_l_base = append_words(words, plan.upper_l);
    descriptor.inverse_diagonal_base = append_words(words, plan.inverse_diagonal);
    return descriptor;
}

struct PackedBatch {
    struct Tile {
        std::size_t first_candidate = 0U;
        std::size_t candidate_count = 0U;
        std::size_t workspace_elements = 0U;
        std::size_t maximum_vertical_vectors = 0U;
        std::size_t maximum_forward_elements = 0U;
        bool has_horizontal = false;
        bool has_vertical = false;
        bool has_both = false;
    };
    std::vector<std::uint32_t> plan_words;
    std::vector<Tile> tiles;
    std::size_t workspace_elements = 0U;
    bool has_horizontal = false;
};

[[nodiscard]] std::size_t packed_axis_word_count(const AxisPlan &plan) {
    std::size_t result = static_cast<std::size_t>(plan.source_size);
    for (const std::size_t elements : {
             plan.forward_weights.size(), plan.transpose_offsets.size(),
             plan.transpose_indices.size(), plan.transpose_weights.size(),
             plan.lower_ld.size(), plan.upper_l.size(),
             plan.inverse_diagonal.size(),
         }) {
        result = checked_add(result, elements, "Vulkan packed plan");
    }
    return result;
}

[[nodiscard]] std::uint32_t axes_code(AnalysisAxes axes) {
    switch (axes) {
    case AnalysisAxes::horizontal: return vulkan_detail::horizontal_axes;
    case AnalysisAxes::vertical: return vulkan_detail::vertical_axes;
    case AnalysisAxes::both: return vulkan_detail::both_axes;
    }
    throw std::invalid_argument("Vulkan candidate has an invalid axes value");
}

[[nodiscard]] PackedBatch pack_batch(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    std::size_t workspace_limit_elements) {
    PackedBatch packed;
    const std::size_t descriptor_words = checked_product(
        candidates.size(), static_cast<std::size_t>(vulkan_detail::candidate_descriptor_words),
        "Vulkan candidate descriptors");
    std::size_t total_plan_words = descriptor_words;
    for (const CandidateAnalysis &candidate : candidates) {
        if (candidate.axes != AnalysisAxes::vertical && candidate.horizontal) {
            total_plan_words = checked_add(
                total_plan_words, packed_axis_word_count(*candidate.horizontal),
                "Vulkan packed plan");
        }
        if (candidate.axes != AnalysisAxes::horizontal && candidate.vertical) {
            total_plan_words = checked_add(
                total_plan_words, packed_axis_word_count(*candidate.vertical),
                "Vulkan packed plan");
        }
    }
    (void)checked_u32(total_plan_words, "Vulkan packed plan words");
    packed.plan_words.reserve(total_plan_words);
    packed.plan_words.resize(descriptor_words);
    PackedBatch::Tile tile;
    tile.first_candidate = 0U;

    for (std::size_t index = 0U; index < candidates.size(); ++index) {
        const CandidateAnalysis &candidate = candidates[index];
        vulkan_detail::CandidateDescriptor descriptor;
        descriptor.axes = axes_code(candidate.axes);
        std::size_t intermediate = 0U;
        std::size_t native = 0U;
        if (candidate.axes != AnalysisAxes::vertical) {
            if (!candidate.horizontal
                || candidate.horizontal->source_size != source.width) {
                throw std::invalid_argument(
                    "Vulkan candidate has no matching horizontal plan");
            }
            descriptor.horizontal = pack_axis(
                packed.plan_words, *candidate.horizontal);
            intermediate = checked_product(
                static_cast<std::size_t>(source.height),
                static_cast<std::size_t>(candidate.horizontal->destination_size),
                "Vulkan horizontal intermediate");
        }
        if (candidate.axes != AnalysisAxes::horizontal) {
            if (!candidate.vertical
                || candidate.vertical->source_size != source.height) {
                throw std::invalid_argument(
                    "Vulkan candidate has no matching vertical plan");
            }
            descriptor.vertical = pack_axis(packed.plan_words, *candidate.vertical);
            native = checked_product(
                static_cast<std::size_t>(candidate.vertical->destination_size),
                candidate.axes == AnalysisAxes::both
                    ? static_cast<std::size_t>(candidate.horizontal->destination_size)
                    : static_cast<std::size_t>(source.width),
                "Vulkan native image");
        }
        std::size_t forward_elements = 0U;
        if (candidate.axes == AnalysisAxes::both) {
            descriptor.forward_order =
                select_forward_order(*candidate.horizontal, *candidate.vertical)
                    == ForwardOrder::horizontal_first
                ? vulkan_detail::horizontal_first
                : vulkan_detail::vertical_first;
            const std::size_t horizontal_first = checked_product(
                static_cast<std::size_t>(source.width),
                static_cast<std::size_t>(candidate.vertical->destination_size),
                "Vulkan forward intermediate");
            intermediate = std::max(intermediate, horizontal_first);
            forward_elements = descriptor.forward_order == vulkan_detail::vertical_first
                ? checked_product(
                    static_cast<std::size_t>(source.height),
                    static_cast<std::size_t>(candidate.horizontal->destination_size),
                    "Vulkan vertical-first forward intermediate")
                : horizontal_first;
        }
        const std::size_t total = checked_add(
            intermediate, native, "Vulkan candidate workspace");
        if (total > workspace_limit_elements) {
            throw std::length_error(
                "Vulkan candidate exceeds the effective workspace limit");
        }
        const bool tile_full = tile.candidate_count != 0U
            && (tile.candidate_count >= vulkan_detail::maximum_tile_candidates
                || total > workspace_limit_elements - tile.workspace_elements);
        if (tile_full) {
            packed.workspace_elements = std::max(
                packed.workspace_elements, tile.workspace_elements);
            packed.tiles.push_back(tile);
            tile = {};
            tile.first_candidate = index;
        }
        descriptor.workspace_base = checked_u32(
            tile.workspace_elements, "Vulkan workspace offset");
        descriptor.intermediate_elements = checked_u32(
            intermediate, "Vulkan intermediate size");
        descriptor.native_elements = checked_u32(native, "Vulkan native size");
        tile.workspace_elements = checked_add(
            tile.workspace_elements, total, "Vulkan tile workspace");
        tile.maximum_forward_elements = std::max(
            tile.maximum_forward_elements, forward_elements);
        if (candidate.axes != AnalysisAxes::horizontal) {
            tile.maximum_vertical_vectors = std::max(
                tile.maximum_vertical_vectors,
                candidate.axes == AnalysisAxes::both
                    ? static_cast<std::size_t>(candidate.horizontal->destination_size)
                    : static_cast<std::size_t>(source.width));
        }
        ++tile.candidate_count;
        tile.has_horizontal = tile.has_horizontal
            || candidate.axes != AnalysisAxes::vertical;
        tile.has_vertical = tile.has_vertical
            || candidate.axes != AnalysisAxes::horizontal;
        tile.has_both = tile.has_both || candidate.axes == AnalysisAxes::both;
        packed.has_horizontal = packed.has_horizontal || tile.has_horizontal;

        std::memcpy(
            packed.plan_words.data()
                + index * vulkan_detail::candidate_descriptor_words,
            &descriptor, sizeof(descriptor));
    }
    if (tile.candidate_count != 0U) {
        packed.workspace_elements = std::max(
            packed.workspace_elements, tile.workspace_elements);
        packed.tiles.push_back(tile);
    }
    return packed;
}

void validate_source_and_metric(ConstImageView source, const MetricSpec &metric,
                                bool require_host_data = true) {
    if ((require_host_data && source.data == nullptr)
        || source.width <= 0 || source.height <= 0
        || (require_host_data && source.stride < source.width)) {
        throw std::invalid_argument("invalid Vulkan source image");
    }
    const std::int64_t horizontal_crop = static_cast<std::int64_t>(metric.crop_left)
        + static_cast<std::int64_t>(metric.crop_right);
    const std::int64_t vertical_crop = static_cast<std::int64_t>(metric.crop_top)
        + static_cast<std::int64_t>(metric.crop_bottom);
    if (metric.crop_left < 0 || metric.crop_right < 0
        || metric.crop_top < 0 || metric.crop_bottom < 0
        || horizontal_crop >= source.width || vertical_crop >= source.height
        || !std::isfinite(metric.threshold) || metric.threshold < 0.0F) {
        throw std::invalid_argument("invalid Vulkan metric configuration");
    }
    if (metric.norm != 1U) {
        throw std::invalid_argument("Vulkan currently supports only p=1");
    }
}

void command_barrier(
    VkCommandBuffer command, VkPipelineStageFlags source_stage,
    VkAccessFlags source_access, VkPipelineStageFlags destination_stage,
    VkAccessFlags destination_access) {
    const VkMemoryBarrier barrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        source_access, destination_access};
    vkCmdPipelineBarrier(
        command, source_stage, destination_stage, 0U,
        1U, &barrier, 0U, nullptr, 0U, nullptr);
}

template <class Function>
class ScopeExit {
public:
    explicit ScopeExit(Function function) : function_(std::move(function)) {}
    ~ScopeExit() { function_(); }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    Function function_;
};

} // namespace

struct VulkanAnalysisEngine::Impl {
    explicit Impl(VulkanAnalysisOptions requested) : options(std::move(requested)) {
        if (options.execution_slots == 0U || options.execution_slots > 8U) {
            throw std::invalid_argument("Vulkan execution_slots must be in [1, 8]");
        }
        if (options.metric_groups_per_candidate == 0U
            || options.metric_groups_per_candidate > 128U) {
            throw std::invalid_argument(
                "Vulkan metric_groups_per_candidate must be in [1, 128]");
        }
        context = std::make_unique<Context>(options, options.execution_slots);
        native_context_info.instance = native_value(context->instance.value);
        native_context_info.physical_device = native_value(context->physical);
        native_context_info.device = native_value(context->device);
        native_context_info.compute_queue = native_value(context->queue);
        native_context_info.compute_queue_family = context->queue_family;
        native_context_info.decode_queue_family = context->decode_queue_family;
        native_context_info.video_codec_operations =
            context->video_codec_operations;
        native_context_info.instance_api_version = context->instance_api_version;
        native_context_info.timeline_semaphore = context->timeline_semaphore;
        native_context_info.enabled_device_extensions =
            context->enabled_device_extensions;
        slots.reserve(options.execution_slots);
        for (std::size_t index = 0U; index < options.execution_slots; ++index) {
            slots.push_back(std::make_unique<ExecutionSlot>(*context));
        }
        slot_busy.assign(options.execution_slots, false);
        const std::size_t storage_limit = context->info.maximum_storage_buffer_bytes;
        const std::size_t default_limit = std::min(default_workspace_bytes, storage_limit);
        effective_workspace_limit_elements = options.workspace_limit_elements == 0U
            ? default_limit / sizeof(float)
            : std::min(options.workspace_limit_elements, storage_limit / sizeof(float));
        if (effective_workspace_limit_elements == 0U) {
            throw std::runtime_error("Vulkan effective workspace limit is empty");
        }
    }

    [[nodiscard]] std::size_t acquire_slot(std::stop_token stop) {
        std::unique_lock lock(slot_mutex);
        while (true) {
            if (stop.stop_requested()) {
                throw std::runtime_error("Vulkan analysis cancelled while waiting for a slot");
            }
            const auto available = std::find(slot_busy.begin(), slot_busy.end(), false);
            if (available != slot_busy.end()) {
                const std::size_t index = static_cast<std::size_t>(
                    std::distance(slot_busy.begin(), available));
                *available = true;
                lock.unlock();
                try {
                    check_vk(vkWaitForFences(
                                 context->device, 1U, &slots[index]->fence,
                                 VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
                             "vkWaitForFences");
                } catch (...) {
                    release_slot(index);
                    throw;
                }
                return index;
            }
            slot_available.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    void release_slot(std::size_t index) noexcept {
        {
            std::scoped_lock lock(slot_mutex);
            slot_busy[index] = false;
        }
        slot_available.notify_one();
    }

    void add_telemetry(const VulkanRuntimeTelemetry &delta) {
        std::scoped_lock lock(telemetry_mutex);
        telemetry.command_buffer_submission_count +=
            delta.command_buffer_submission_count;
        telemetry.kernel_dispatch_count += delta.kernel_dispatch_count;
        telemetry.analyzed_candidate_count += delta.analyzed_candidate_count;
        telemetry.tile_count += delta.tile_count;
        telemetry.buffer_allocation_count += delta.buffer_allocation_count;
        telemetry.plan_upload_bytes += delta.plan_upload_bytes;
        telemetry.source_upload_bytes += delta.source_upload_bytes;
        telemetry.source_conversion_bytes += delta.source_conversion_bytes;
        telemetry.source_conversion_count += delta.source_conversion_count;
        telemetry.result_readback_bytes += delta.result_readback_bytes;
        telemetry.workspace_bytes = delta.workspace_bytes;
        telemetry.execution_slot_wait_ms += delta.execution_slot_wait_ms;
        telemetry.host_pack_ms += delta.host_pack_ms;
        telemetry.source_conversion_ms += delta.source_conversion_ms;
        telemetry.gpu_execution_ms += delta.gpu_execution_ms;
        peak_workspace = std::max(peak_workspace, delta.peak_workspace_elements);
        peak_working_set = std::max(peak_working_set, delta.peak_working_set_bytes);
        telemetry.peak_workspace_elements = peak_workspace;
        telemetry.peak_working_set_bytes = peak_working_set;
    }

    VulkanAnalysisOptions options;
    VulkanNativeContextInfo native_context_info;
    std::unique_ptr<Context> context;
    std::vector<std::unique_ptr<ExecutionSlot>> slots;
    std::vector<bool> slot_busy;
    std::mutex slot_mutex;
    std::condition_variable slot_available;
    std::size_t effective_workspace_limit_elements = 0U;
    mutable std::mutex telemetry_mutex;
    VulkanRuntimeTelemetry telemetry;
    std::size_t peak_workspace = 0U;
    std::size_t peak_working_set = 0U;
};

VulkanRuntimeProbe vulkan_runtime_probe() noexcept {
    VulkanRuntimeProbe probe;
    try {
        InstanceHandle instance;
        create_instance(instance, false, nullptr);
        probe.instance_created = true;
        std::vector<DeviceRecord> records = device_records(instance.value);
        probe.devices.reserve(records.size());
        for (DeviceRecord &record : records) {
            probe.device_available = probe.device_available
                || record.info.backend_compatible;
            probe.devices.push_back(std::move(record.info));
        }
        if (!probe.device_available) {
            probe.reason = probe.devices.empty()
                ? "no Vulkan physical devices were found"
                : "no Vulkan device satisfies the compute backend requirements";
        }
    } catch (const std::exception &error) {
        probe.reason = error.what();
    } catch (...) {
        probe.reason = "unknown Vulkan runtime probe failure";
    }
    return probe;
}

bool vulkan_backend_available() noexcept {
    return vulkan_runtime_probe().device_available;
}

std::vector<VulkanDeviceInfo> enumerate_vulkan_devices() {
    InstanceHandle instance;
    create_instance(instance, false, nullptr);
    std::vector<DeviceRecord> records = device_records(instance.value);
    std::vector<VulkanDeviceInfo> result;
    result.reserve(records.size());
    for (DeviceRecord &record : records) result.push_back(std::move(record.info));
    return result;
}

std::int32_t select_default_vulkan_device_index(
    std::span<const VulkanDeviceInfo> devices) noexcept {
    const auto selected = std::min_element(
        devices.begin(), devices.end(), [](const auto &left, const auto &right) {
            return better_default_device(left, right);
        });
    return selected != devices.end() && selected->backend_compatible
        ? selected->index : vulkan_automatic_device_index;
}

VulkanAnalysisEngine::VulkanAnalysisEngine(VulkanAnalysisOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

VulkanAnalysisEngine::~VulkanAnalysisEngine() = default;
VulkanAnalysisEngine::VulkanAnalysisEngine(VulkanAnalysisEngine &&) noexcept = default;
VulkanAnalysisEngine &VulkanAnalysisEngine::operator=(
    VulkanAnalysisEngine &&) noexcept = default;

const VulkanDeviceInfo &VulkanAnalysisEngine::device_info() const noexcept {
    return impl_->context->info;
}

const VulkanAnalysisOptions &VulkanAnalysisEngine::options() const noexcept {
    return impl_->options;
}

std::size_t VulkanAnalysisEngine::peak_workspace_elements() const noexcept {
    std::scoped_lock lock(impl_->telemetry_mutex);
    return impl_->peak_workspace;
}

std::size_t VulkanAnalysisEngine::peak_working_set_bytes() const noexcept {
    std::scoped_lock lock(impl_->telemetry_mutex);
    return impl_->peak_working_set;
}

VulkanRuntimeTelemetry VulkanAnalysisEngine::runtime_telemetry() const {
    std::scoped_lock lock(impl_->telemetry_mutex);
    VulkanRuntimeTelemetry result = impl_->telemetry;
    result.validation_error_count =
        impl_->context->validation_errors.load(std::memory_order_relaxed);
    return result;
}

void VulkanAnalysisEngine::reset_analysis_telemetry() {
    std::scoped_lock lock(impl_->telemetry_mutex);
    impl_->telemetry = {};
    impl_->telemetry.peak_workspace_elements = impl_->peak_workspace;
    impl_->telemetry.peak_working_set_bytes = impl_->peak_working_set;
    impl_->context->validation_errors.store(0U, std::memory_order_relaxed);
}

const VulkanNativeContextInfo &VulkanAnalysisEngine::native_context() const noexcept {
    return impl_->native_context_info;
}

void VulkanAnalysisEngine::lock_native_queue() {
    impl_->context->lock_queue();
}

void VulkanAnalysisEngine::unlock_native_queue() noexcept {
    impl_->context->unlock_queue();
}

void VulkanAnalysisEngine::preflight_axis_batch(
    ConstImageView dimensions,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::size_t concurrency) const {
    validate_source_and_metric(dimensions, metric, false);
    if (concurrency == 0U || concurrency > impl_->options.execution_slots) {
        throw std::length_error(
            "Vulkan execution slots cannot satisfy the requested concurrency");
    }
    if (candidates.empty()) return;

    const PackedBatch packed = pack_batch(
        dimensions, candidates, impl_->effective_workspace_limit_elements);
    const std::size_t source_bytes = checked_product(
        checked_product(
            static_cast<std::size_t>(dimensions.width),
            static_cast<std::size_t>(dimensions.height), "Vulkan source image"),
        sizeof(float), "Vulkan source buffer");
    const std::size_t device_source_bytes = checked_product(
        source_bytes, 2U, "Vulkan source buffers");
    const std::size_t plan_bytes = checked_product(
        packed.plan_words.size(), sizeof(std::uint32_t), "Vulkan plan buffer");
    const std::size_t workspace_bytes = checked_product(
        packed.workspace_elements, sizeof(float), "Vulkan workspace buffer");
    const std::size_t metric_groups = std::min<std::size_t>(
        impl_->options.metric_groups_per_candidate,
        (checked_product(
             static_cast<std::size_t>(
                 dimensions.width - metric.crop_left - metric.crop_right),
             static_cast<std::size_t>(
                 dimensions.height - metric.crop_top - metric.crop_bottom),
             "Vulkan metric pixels") + 255U) / 256U);
    const std::size_t partial_bytes = checked_product(
        checked_product(
            candidates.size(), metric_groups, "Vulkan metric partials"),
        sizeof(float), "Vulkan partial buffer");
    const std::size_t storage_limit =
        impl_->context->info.maximum_storage_buffer_bytes;
    if (plan_bytes > storage_limit || device_source_bytes > storage_limit
        || workspace_bytes > storage_limit || partial_bytes > storage_limit) {
        throw std::length_error(
            "Vulkan storage buffers cannot satisfy the requested concurrency");
    }
    const std::size_t working_set = checked_add(
        checked_add(plan_bytes, device_source_bytes, "Vulkan working set"),
        checked_add(workspace_bytes, partial_bytes, "Vulkan working set"),
        "Vulkan working set");
    const std::size_t aggregate = checked_product(
        working_set, concurrency, "Vulkan concurrent working set");
    const std::size_t local_memory =
        impl_->context->info.device_local_memory_bytes;
    const std::size_t device_budget = local_memory - local_memory / 8U;
    if (aggregate > device_budget) {
        throw std::length_error(
            "Vulkan device memory cannot satisfy the requested concurrency");
    }
}

std::vector<CandidateResult> VulkanAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    return analyze_axis_batch_impl(source, nullptr, candidates, metric, stop);
}

std::vector<CandidateResult> VulkanAnalysisEngine::analyze_axis_batch_vulkan_luma(
    const VulkanLumaFrameView &source,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    const ConstImageView geometry{
        nullptr, source.width, source.height, source.width};
    return analyze_axis_batch_impl(
        geometry, &source, candidates, metric, stop);
}

std::vector<CandidateResult> VulkanAnalysisEngine::analyze_axis_batch_impl(
    ConstImageView source, const VulkanLumaFrameView *device_source,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    bool frame_released = false;
    ScopeExit release_frame{[&] {
        if (device_source != nullptr && !frame_released
            && device_source->release_without_submit != nullptr) {
            device_source->release_without_submit(device_source->sync_opaque);
        }
    }};
    validate_source_and_metric(source, metric, device_source == nullptr);
    if (device_source != nullptr) {
        if (device_source->image == 0U || device_source->semaphore == 0U
            || device_source->view_format == VK_FORMAT_UNDEFINED
            || device_source->aspect_mask == 0U
            || device_source->bit_depth <= 0 || device_source->bit_depth > 16
            || device_source->normalized_sample_bits <= 0
            || device_source->normalized_sample_bits > 16
            || device_source->mark_submitted == nullptr
            || device_source->release_without_submit == nullptr) {
            throw std::invalid_argument("invalid Vulkan decoded luma frame");
        }
        if (!impl_->context->info.video_decode_available
            || !impl_->context->timeline_semaphore) {
            throw std::invalid_argument(
                "Vulkan decoded luma input requires the shared video device");
        }
    }
    if (candidates.empty()) return {};
    if (stop.stop_requested()) throw std::runtime_error("Vulkan analysis cancelled");

    VulkanRuntimeTelemetry delta;
    const auto wait_start = std::chrono::steady_clock::now();
    const std::size_t slot_index = impl_->acquire_slot(stop);
    delta.execution_slot_wait_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wait_start).count();
    ExecutionSlot &slot = *impl_->slots[slot_index];
    ScopeExit release_slot{[&] { impl_->release_slot(slot_index); }};

    const auto pack_start = std::chrono::steady_clock::now();
    PackedBatch packed = pack_batch(
        source, candidates, impl_->effective_workspace_limit_elements);
    delta.host_pack_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - pack_start).count();
    if (stop.stop_requested()) throw std::runtime_error("Vulkan analysis cancelled");

    const std::size_t source_elements = checked_product(
        static_cast<std::size_t>(source.width),
        static_cast<std::size_t>(source.height), "Vulkan source image");
    const std::size_t source_bytes = checked_product(
        source_elements, sizeof(float), "Vulkan source buffer");
    const std::size_t device_source_bytes = checked_product(
        source_bytes, 2U, "Vulkan source and transpose buffer");
    const std::size_t plan_bytes = checked_product(
        packed.plan_words.size(), sizeof(std::uint32_t), "Vulkan plan buffer");
    const std::size_t workspace_bytes = checked_product(
        packed.workspace_elements, sizeof(float), "Vulkan workspace buffer");
    const std::size_t metric_groups = std::min<std::size_t>(
        impl_->options.metric_groups_per_candidate,
        (checked_product(
             static_cast<std::size_t>(source.width - metric.crop_left - metric.crop_right),
             static_cast<std::size_t>(source.height - metric.crop_top - metric.crop_bottom),
             "Vulkan metric pixels") + 255U) / 256U);
    const std::size_t partial_count = checked_product(
        candidates.size(), metric_groups, "Vulkan metric partials");
    const std::size_t partial_bytes = checked_product(
        partial_count, sizeof(float), "Vulkan partial buffer");
    const std::size_t storage_limit = impl_->context->info.maximum_storage_buffer_bytes;
    if (plan_bytes > storage_limit || device_source_bytes > storage_limit
        || workspace_bytes > storage_limit || partial_bytes > storage_limit) {
        throw std::length_error(
            "Vulkan storage buffer exceeds maxStorageBufferRange");
    }

    std::size_t allocations = 0U;
    constexpr VkBufferUsageFlags host_usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkBufferUsageFlags device_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    slot.host_plan.reserve(
        *impl_->context, plan_bytes, host_usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        impl_->options.force_non_coherent, allocations);
    if (device_source == nullptr) {
        slot.host_source.reserve(
            *impl_->context, source_bytes, host_usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            impl_->options.force_non_coherent, allocations);
    }
    slot.host_partials.reserve(
        *impl_->context, partial_bytes, host_usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        impl_->options.force_non_coherent, allocations);
    slot.device_plan.reserve(
        *impl_->context, plan_bytes, device_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, false, allocations,
        static_cast<VkDeviceSize>(storage_limit));
    slot.device_source.reserve(
        *impl_->context, device_source_bytes, device_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, false, allocations,
        static_cast<VkDeviceSize>(storage_limit));
    slot.device_workspace.reserve(
        *impl_->context, workspace_bytes, device_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, false, allocations,
        static_cast<VkDeviceSize>(impl_->effective_workspace_limit_elements
                                  * sizeof(float)));
    slot.device_partials.reserve(
        *impl_->context, partial_bytes, device_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, false, allocations,
        static_cast<VkDeviceSize>(storage_limit));
    delta.buffer_allocation_count = allocations;

    std::memcpy(slot.host_plan.mapped(), packed.plan_words.data(), plan_bytes);
    if (device_source == nullptr) {
        float *staged_source = static_cast<float *>(slot.host_source.mapped());
        for (std::int32_t row = 0; row < source.height; ++row) {
            std::memcpy(
                staged_source + static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(source.width),
                source.data + static_cast<std::ptrdiff_t>(row) * source.stride,
                static_cast<std::size_t>(source.width) * sizeof(float));
        }
        slot.host_source.flush();
    }
    slot.host_plan.flush();

    ImageViewHandle luma_view;
    if (device_source != nullptr) {
        luma_view.create(impl_->context->device, *device_source);
    }

    const std::array<VkDescriptorBufferInfo, 4U> buffer_infos{{
        {slot.device_plan.get(), 0U, static_cast<VkDeviceSize>(plan_bytes)},
        {slot.device_source.get(), 0U, static_cast<VkDeviceSize>(device_source_bytes)},
        {slot.device_workspace.get(), 0U, static_cast<VkDeviceSize>(workspace_bytes)},
        {slot.device_partials.get(), 0U, static_cast<VkDeviceSize>(partial_bytes)},
    }};
    const VkDescriptorImageInfo luma_info{
        impl_->context->luma_sampler, luma_view.get(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, 5U> descriptor_writes{};
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        descriptor_writes[index] = VkWriteDescriptorSet{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            slot.descriptor_set, index, 0U, 1U,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,
            &buffer_infos[index], nullptr};
    }
    if (device_source != nullptr) {
        descriptor_writes[4] = VkWriteDescriptorSet{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            slot.descriptor_set, 4U, 0U, 1U,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &luma_info, nullptr, nullptr};
    }
    vkUpdateDescriptorSets(
        impl_->context->device,
        device_source != nullptr
            ? static_cast<std::uint32_t>(descriptor_writes.size()) : 4U,
        descriptor_writes.data(), 0U, nullptr);

    check_vk(vkResetCommandPool(
                 impl_->context->device, slot.command_pool, 0U),
             "vkResetCommandPool");
    const VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    check_vk(vkBeginCommandBuffer(slot.command, &begin_info),
             "vkBeginCommandBuffer");
    if (device_source != nullptr
        && slot.conversion_query_pool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(slot.command, slot.conversion_query_pool, 0U, 2U);
    }
    const VkBufferCopy plan_copy{0U, 0U, static_cast<VkDeviceSize>(plan_bytes)};
    vkCmdCopyBuffer(slot.command, slot.host_plan.get(), slot.device_plan.get(),
                    1U, &plan_copy);
    if (device_source == nullptr) {
        const VkBufferCopy source_copy{
            0U, 0U, static_cast<VkDeviceSize>(source_bytes)};
        vkCmdCopyBuffer(slot.command, slot.host_source.get(),
                        slot.device_source.get(), 1U, &source_copy);
    } else {
        const bool concurrent =
            device_source->queue_family == VK_QUEUE_FAMILY_IGNORED;
        const bool same_family =
            device_source->queue_family == impl_->context->queue_family;
        const VkImageMemoryBarrier image_barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            0U,
            VK_ACCESS_SHADER_READ_BIT,
            static_cast<VkImageLayout>(device_source->layout),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            concurrent || same_family ? VK_QUEUE_FAMILY_IGNORED
                                      : device_source->queue_family,
            concurrent || same_family ? VK_QUEUE_FAMILY_IGNORED
                                      : impl_->context->queue_family,
            native_handle<VkImage>(device_source->image),
            {static_cast<VkImageAspectFlags>(device_source->aspect_mask),
             0U, 1U, 0U, 1U},
        };
        vkCmdPipelineBarrier(
            slot.command,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0U, 0U, nullptr, 0U, nullptr, 1U, &image_barrier);
    }
    command_barrier(
        slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindDescriptorSets(
        slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
        impl_->context->pipeline_layout, 0U, 1U, &slot.descriptor_set,
        0U, nullptr);

    std::array<std::uint32_t, 32U> push{};
    push[0] = static_cast<std::uint32_t>(source.width);
    push[1] = static_cast<std::uint32_t>(source.height);
    push[2] = checked_u32(source_elements, "Vulkan source elements");
    push[6] = checked_u32(metric_groups, "Vulkan metric group count");
    push[7] = static_cast<std::uint32_t>(metric.crop_left);
    push[8] = static_cast<std::uint32_t>(metric.crop_right);
    push[9] = static_cast<std::uint32_t>(metric.crop_top);
    push[10] = static_cast<std::uint32_t>(metric.crop_bottom);
    push[11] = std::bit_cast<std::uint32_t>(metric.threshold);
    if (device_source != nullptr) {
        push[13] = static_cast<std::uint32_t>(device_source->bit_depth);
        push[14] = static_cast<std::uint32_t>(
            device_source->normalized_sample_bits);
    }
    const auto write_push = [&] {
        vkCmdPushConstants(
            slot.command, impl_->context->pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0U,
            static_cast<std::uint32_t>(sizeof(push)), push.data());
    };
    std::size_t dispatches = 0U;
    if (device_source != nullptr) {
        if (slot.conversion_query_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(
                slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                slot.conversion_query_pool, 0U);
        }
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->context->luma_pipeline);
        write_push();
        vkCmdDispatch(
            slot.command,
            divide_up(static_cast<std::uint32_t>(source.width), 16U),
            divide_up(static_cast<std::uint32_t>(source.height), 16U), 1U);
        ++dispatches;
        command_barrier(
            slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        if (slot.conversion_query_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(
                slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                slot.conversion_query_pool, 1U);
        }
    }
    if (packed.has_horizontal) {
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->context->transpose_pipeline);
        write_push();
        vkCmdDispatch(
            slot.command,
            divide_up(static_cast<std::uint32_t>(source.width), 32U),
            divide_up(static_cast<std::uint32_t>(source.height), 32U), 1U);
        ++dispatches;
        command_barrier(
            slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    }

    for (const PackedBatch::Tile &tile : packed.tiles) {
        push[3] = checked_u32(tile.first_candidate, "Vulkan tile offset");
        push[4] = checked_u32(tile.candidate_count, "Vulkan tile size");
        if (tile.has_horizontal) {
            push[12] = 0U;
            vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              impl_->context->inverse_pipeline);
            write_push();
            vkCmdDispatch(
                slot.command,
                divide_up(static_cast<std::uint32_t>(source.height), 64U),
                push[4], 1U);
            ++dispatches;
            command_barrier(
                slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (tile.has_vertical) {
            push[12] = 1U;
            vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              impl_->context->inverse_pipeline);
            write_push();
            vkCmdDispatch(
                slot.command,
                divide_up(checked_u32(
                    tile.maximum_vertical_vectors,
                    "Vulkan vertical vector count"), 64U),
                push[4], 1U);
            ++dispatches;
            command_barrier(
                slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (tile.has_both) {
            vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              impl_->context->forward_pipeline);
            write_push();
            vkCmdDispatch(
                slot.command,
                divide_up(checked_u32(
                    tile.maximum_forward_elements,
                    "Vulkan forward intermediate"), 256U),
                push[4], 1U);
            ++dispatches;
            command_barrier(
                slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        }
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->context->metric_pipeline);
        write_push();
        vkCmdDispatch(slot.command, push[6], push[4], 1U);
        ++dispatches;
        command_barrier(
            slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    command_barrier(
        slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy partial_copy{0U, 0U, static_cast<VkDeviceSize>(partial_bytes)};
    vkCmdCopyBuffer(slot.command, slot.device_partials.get(), slot.host_partials.get(),
                    1U, &partial_copy);
    check_vk(vkEndCommandBuffer(slot.command), "vkEndCommandBuffer");

    check_vk(vkResetFences(impl_->context->device, 1U, &slot.fence),
             "vkResetFences");
    const auto gpu_start = std::chrono::steady_clock::now();
    impl_->context->submit(slot.command, slot.fence, device_source);
    if (device_source != nullptr) {
        const std::uint32_t final_queue_family =
            device_source->queue_family == VK_QUEUE_FAMILY_IGNORED
            ? VK_QUEUE_FAMILY_IGNORED : impl_->context->queue_family;
        device_source->mark_submitted(
            device_source->sync_opaque,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT,
            final_queue_family,
            device_source->semaphore_value + 1U);
        frame_released = true;
    }
    check_vk(vkWaitForFences(
                 impl_->context->device, 1U, &slot.fence, VK_TRUE,
                 std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences");
    delta.gpu_execution_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - gpu_start).count();
    slot.host_partials.invalidate();
    if (device_source != nullptr
        && slot.conversion_query_pool != VK_NULL_HANDLE) {
        std::array<std::uint64_t, 2U> timestamps{};
        check_vk(vkGetQueryPoolResults(
                     impl_->context->device, slot.conversion_query_pool,
                     0U, 2U, sizeof(timestamps), timestamps.data(),
                     sizeof(std::uint64_t),
                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                 "vkGetQueryPoolResults(luma timestamps)");
        const std::uint32_t valid_bits =
            impl_->context->timestamp_valid_bits;
        const std::uint64_t elapsed_ticks = valid_bits >= 64U
            ? timestamps[1] - timestamps[0]
            : (timestamps[1] - timestamps[0])
                & ((std::uint64_t{1U} << valid_bits) - 1U);
        delta.source_conversion_ms = static_cast<double>(elapsed_ticks)
            * static_cast<double>(impl_->context->properties.limits.timestampPeriod)
            / 1'000'000.0;
    }
    if (stop.stop_requested()) throw std::runtime_error("Vulkan analysis cancelled");

    const float *partials = static_cast<const float *>(slot.host_partials.mapped());
    const double pixel_count = static_cast<double>(
        source.width - metric.crop_left - metric.crop_right)
        * static_cast<double>(source.height - metric.crop_top - metric.crop_bottom);
    std::vector<CandidateResult> results;
    results.reserve(candidates.size());
    for (std::size_t candidate = 0U; candidate < candidates.size(); ++candidate) {
        double sum = 0.0;
        for (std::size_t group = 0U; group < metric_groups; ++group) {
            sum += static_cast<double>(partials[candidate * metric_groups + group]);
        }
        results.push_back({candidates[candidate].id, sum / pixel_count});
    }

    delta.command_buffer_submission_count = 1U;
    delta.kernel_dispatch_count = dispatches;
    delta.analyzed_candidate_count = candidates.size();
    delta.tile_count = packed.tiles.size();
    delta.plan_upload_bytes = plan_bytes;
    delta.source_upload_bytes = device_source == nullptr ? source_bytes : 0U;
    delta.source_conversion_bytes = device_source != nullptr ? source_bytes : 0U;
    delta.source_conversion_count = device_source != nullptr ? 1U : 0U;
    delta.result_readback_bytes = partial_bytes;
    delta.workspace_bytes = workspace_bytes;
    delta.peak_workspace_elements = packed.workspace_elements;
    delta.peak_working_set_bytes = checked_add(
        checked_add(plan_bytes, device_source_bytes, "Vulkan working set"),
        checked_add(workspace_bytes, partial_bytes, "Vulkan working set"),
        "Vulkan working set");
    impl_->add_telemetry(delta);
    return results;
}

} // namespace getnative

#include "getnative/vulkan_analysis.hpp"

#include "../gpu/gpu_batch.hpp"
#include "getnative_vulkan_forward_axis_matrix_spv.hpp"
#include "getnative_vulkan_inverse_axis_matrix_spv.hpp"
#include "getnative_vulkan_inverse_axis_spv.hpp"
#include "getnative_vulkan_metric_axis_p1_spv.hpp"
#include "vulkan_loader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace getnative {
namespace {

namespace gpu = detail::gpu;
using vulkan_detail::check_vk;

constexpr std::uint32_t descriptor_binding_count = 12;
constexpr std::uint32_t reduction_width = 256;
constexpr std::uint32_t timestamp_query_count = 5;

enum class PipelineStage : std::uint8_t {
    image_inverse,
    matrix_inverse,
    matrix_forward,
    metric,
    horizontal_first_metric,
};

constexpr std::array<gpu::KernelShape, 5> all_shapes{
    gpu::KernelShape::bandwidth3,
    gpu::KernelShape::bandwidth7,
    gpu::KernelShape::bandwidth11,
    gpu::KernelShape::bandwidth15,
    gpu::KernelShape::generic,
};

[[nodiscard]] gpu::KernelDispatchPolicy dispatch_policy(
    VulkanKernelDispatchPolicy policy) noexcept {
    switch (policy) {
    case VulkanKernelDispatchPolicy::automatic:
        return gpu::KernelDispatchPolicy::automatic;
    case VulkanKernelDispatchPolicy::generic_only:
        return gpu::KernelDispatchPolicy::generic_only;
    case VulkanKernelDispatchPolicy::required_specialized:
        return gpu::KernelDispatchPolicy::required_specialized;
    }
    return gpu::KernelDispatchPolicy::automatic;
}

[[nodiscard]] std::size_t shape_index(gpu::KernelShape shape) noexcept {
    switch (shape) {
    case gpu::KernelShape::bandwidth3: return 0;
    case gpu::KernelShape::bandwidth7: return 1;
    case gpu::KernelShape::bandwidth11: return 2;
    case gpu::KernelShape::bandwidth15: return 3;
    case gpu::KernelShape::generic: return 4;
    }
    return 4;
}

[[nodiscard]] std::size_t stage_index(PipelineStage stage) noexcept {
    return static_cast<std::size_t>(stage);
}

[[nodiscard]] const char *shape_name(gpu::KernelShape shape) noexcept {
    switch (shape) {
    case gpu::KernelShape::bandwidth3: return "b3";
    case gpu::KernelShape::bandwidth7: return "b7";
    case gpu::KernelShape::bandwidth11: return "b11";
    case gpu::KernelShape::bandwidth15: return "b15";
    case gpu::KernelShape::generic: return "generic";
    }
    return "generic";
}

[[nodiscard]] const char *stage_name(PipelineStage stage) noexcept {
    switch (stage) {
    case PipelineStage::image_inverse: return "inverse_axis";
    case PipelineStage::matrix_inverse: return "inverse_axis_matrix";
    case PipelineStage::matrix_forward: return "forward_axis_matrix";
    case PipelineStage::metric: return "metric_axis_p1";
    case PipelineStage::horizontal_first_metric:
        return "metric_axis_p1_horizontal_first";
    }
    return "unknown";
}

[[nodiscard]] std::uint32_t fixed_half_bandwidth(gpu::KernelShape shape) noexcept {
    switch (shape) {
    case gpu::KernelShape::bandwidth3: return 1;
    case gpu::KernelShape::bandwidth7: return 3;
    case gpu::KernelShape::bandwidth11: return 5;
    case gpu::KernelShape::bandwidth15: return 7;
    case gpu::KernelShape::generic: return 0;
    }
    return 0;
}

[[nodiscard]] std::uint32_t fixed_forward_width(gpu::KernelShape shape) noexcept {
    switch (shape) {
    case gpu::KernelShape::bandwidth3: return 2;
    case gpu::KernelShape::bandwidth7: return 4;
    case gpu::KernelShape::bandwidth11: return 6;
    case gpu::KernelShape::bandwidth15: return 8;
    case gpu::KernelShape::generic: return 0;
    }
    return 0;
}

[[nodiscard]] std::size_t checked_add(std::size_t lhs, std::size_t rhs,
                                      std::string_view name) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        throw std::length_error(std::string{name} + " size overflow");
    }
    return lhs + rhs;
}

[[nodiscard]] VkDeviceSize checked_device_size(std::size_t value,
                                               std::string_view name) {
    if constexpr (sizeof(std::size_t) > sizeof(VkDeviceSize)) {
        if (value > std::numeric_limits<VkDeviceSize>::max()) {
            throw std::length_error(std::string{name} + " exceeds VkDeviceSize");
        }
    }
    return static_cast<VkDeviceSize>(value);
}

[[nodiscard]] std::uint32_t ceil_div_u32(std::size_t value, std::size_t divisor,
                                         std::string_view name) {
    if (divisor == 0) {
        throw std::invalid_argument(std::string{name} + " divisor is zero");
    }
    const std::size_t groups = value / divisor
        + static_cast<std::size_t>(value % divisor != 0);
    return gpu::checked_u32(groups, std::string{name}.c_str());
}

[[nodiscard]] std::string uuid_selector(const VulkanDeviceUuid &uuid) {
    std::ostringstream stream;
    stream << "uuid:" << std::hex << std::setfill('0');
    for (const std::uint8_t byte : uuid) {
        stream << std::setw(2) << static_cast<unsigned>(byte);
    }
    return stream.str();
}

[[nodiscard]] bool has_name(std::span<const VkLayerProperties> values,
                            std::string_view name) {
    return std::ranges::any_of(values, [&](const VkLayerProperties &value) {
        return name == value.layerName;
    });
}

[[nodiscard]] bool has_name(std::span<const VkExtensionProperties> values,
                            std::string_view name) {
    return std::ranges::any_of(values, [&](const VkExtensionProperties &value) {
        return name == value.extensionName;
    });
}

template <class Value, class Enumerate>
[[nodiscard]] std::vector<Value> enumerate_vector(Enumerate &&enumerate,
                                                   std::string_view operation) {
    std::uint32_t count = 0;
    VkResult result = enumerate(&count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        check_vk(result, operation);
    }
    for (;;) {
        std::vector<Value> values(count);
        result = enumerate(&count, values.data());
        if (result == VK_SUCCESS) {
            values.resize(count);
            return values;
        }
        if (result != VK_INCOMPLETE) {
            check_vk(result, operation);
        }
    }
}

struct ValidationState {
    std::atomic_size_t errors{0};
    std::atomic_size_t warnings{0};
};

VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT *, void *user_data) {
    auto *state = static_cast<ValidationState *>(user_data);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        state->errors.fetch_add(1, std::memory_order_relaxed);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        state->warnings.fetch_add(1, std::memory_order_relaxed);
    }
    return VK_FALSE;
}

[[nodiscard]] VkDebugUtilsMessengerCreateInfoEXT debug_messenger_info(
    ValidationState *state) {
    VkDebugUtilsMessengerCreateInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = validation_callback;
    info.pUserData = state;
    return info;
}

class InstanceContext {
public:
    InstanceContext(std::wstring_view loader_path, bool enable_validation)
        : loader(loader_path), validation_enabled(enable_validation) {
        std::uint32_t loader_version = VK_API_VERSION_1_0;
        if (loader.global().enumerate_instance_version != nullptr) {
            check_vk(loader.global().enumerate_instance_version(&loader_version),
                     "vkEnumerateInstanceVersion");
        }
        if (loader_version < VK_API_VERSION_1_2) {
            throw std::runtime_error("Vulkan 1.2 loader support is required");
        }

        std::vector<const char *> layers;
        std::vector<const char *> extensions;
        if (enable_validation) {
            const auto available_layers = enumerate_vector<VkLayerProperties>(
                [&](std::uint32_t *count, VkLayerProperties *values) {
                    return loader.global().enumerate_instance_layer_properties(count, values);
                }, "vkEnumerateInstanceLayerProperties");
            if (!has_name(available_layers, "VK_LAYER_KHRONOS_validation")) {
                throw std::runtime_error("Vulkan validation layer is unavailable");
            }
            const auto available_extensions = enumerate_vector<VkExtensionProperties>(
                [&](std::uint32_t *count, VkExtensionProperties *values) {
                    return loader.global().enumerate_instance_extension_properties(
                        nullptr, count, values);
                }, "vkEnumerateInstanceExtensionProperties");
            if (!has_name(available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                throw std::runtime_error("Vulkan debug utils extension is unavailable");
            }
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "GetNative VF";
        application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.pEngineName = "getnative-engine";
        application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.apiVersion = VK_API_VERSION_1_2;

        VkDebugUtilsMessengerCreateInfoEXT debug_info = debug_messenger_info(&validation);
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pNext = enable_validation ? &debug_info : nullptr;
        create.pApplicationInfo = &application;
        create.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        create.ppEnabledLayerNames = layers.data();
        create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        check_vk(loader.global().create_instance(&create, nullptr, &instance),
                 "vkCreateInstance");
        try {
            vk = loader.load_instance(instance);
            if (enable_validation) {
                if (vk.create_debug_messenger == nullptr
                    || vk.destroy_debug_messenger == nullptr) {
                    throw std::runtime_error(
                        "Vulkan debug utils functions are unavailable");
                }
                check_vk(vk.create_debug_messenger(
                             instance, &debug_info, nullptr, &debug_messenger),
                         "vkCreateDebugUtilsMessengerEXT");
            }
        } catch (...) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
                loader.instance_proc(instance, "vkDestroyInstance"));
            if (destroy != nullptr) {
                destroy(instance, nullptr);
            }
            instance = VK_NULL_HANDLE;
            throw;
        }
    }

    ~InstanceContext() {
        if (debug_messenger != VK_NULL_HANDLE && vk.destroy_debug_messenger != nullptr) {
            vk.destroy_debug_messenger(instance, debug_messenger, nullptr);
        }
        if (instance != VK_NULL_HANDLE && vk.destroy_instance != nullptr) {
            vk.destroy_instance(instance, nullptr);
        }
    }

    vulkan_detail::VulkanLoader loader;
    VkInstance instance = VK_NULL_HANDLE;
    vulkan_detail::InstanceDispatch vk{};
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    ValidationState validation{};
    bool validation_enabled = false;
};

struct PhysicalCandidate {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VulkanDeviceInfo info{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t timestamp_valid_bits = 0;
};

[[nodiscard]] std::vector<PhysicalCandidate> enumerate_candidates(
    InstanceContext &context) {
    const auto devices = enumerate_vector<VkPhysicalDevice>(
        [&](std::uint32_t *count, VkPhysicalDevice *values) {
            return context.vk.enumerate_physical_devices(
                context.instance, count, values);
        }, "vkEnumeratePhysicalDevices");
    std::vector<PhysicalCandidate> candidates;
    candidates.reserve(devices.size());
    for (std::size_t ordinal = 0; ordinal < devices.size(); ++ordinal) {
        PhysicalCandidate candidate;
        candidate.handle = devices[ordinal];

        const auto device_extensions = enumerate_vector<VkExtensionProperties>(
            [&](std::uint32_t *count, VkExtensionProperties *values) {
                return context.vk.enumerate_device_extension_properties(
                    candidate.handle, nullptr, count, values);
            }, "vkEnumerateDeviceExtensionProperties");
        const bool has_pci = has_name(device_extensions,
                                      VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);

        VkPhysicalDevicePCIBusInfoPropertiesEXT pci{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        VkPhysicalDeviceFloatControlsProperties float_controls{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
        VkPhysicalDeviceIDProperties id{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        id.pNext = &float_controls;
        float_controls.pNext = has_pci ? &pci : nullptr;
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &id;
        context.vk.get_physical_device_properties2(candidate.handle, &properties2);
        candidate.properties = properties2.properties;
        context.vk.get_memory_properties(candidate.handle, &candidate.memory);

        std::uint32_t queue_count = 0;
        context.vk.get_queue_family_properties(candidate.handle, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        context.vk.get_queue_family_properties(
            candidate.handle, &queue_count, queues.data());
        queues.resize(queue_count);
        std::optional<std::uint32_t> compute_queue;
        for (std::uint32_t index = 0; index < queue_count; ++index) {
            if (queues[index].queueCount == 0
                || (queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
                continue;
            }
            if (!compute_queue.has_value()
                || ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0
                    && (queues[*compute_queue].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)) {
                compute_queue = index;
            }
        }

        VulkanDeviceInfo &info = candidate.info;
        info.name = candidate.properties.deviceName;
        std::copy_n(id.deviceUUID, vulkan_device_uuid_size, info.uuid.begin());
        info.stable_selector = uuid_selector(info.uuid);
        info.ordinal = ordinal;
        info.vendor_id = candidate.properties.vendorID;
        info.device_id = candidate.properties.deviceID;
        info.device_type = static_cast<std::uint32_t>(candidate.properties.deviceType);
        info.api_version = candidate.properties.apiVersion;
        info.driver_version = candidate.properties.driverVersion;
        info.compute_queue_family = compute_queue.value_or(
            std::numeric_limits<std::uint32_t>::max());
        info.maximum_compute_workgroup_invocations =
            candidate.properties.limits.maxComputeWorkGroupInvocations;
        info.maximum_storage_buffer_range =
            candidate.properties.limits.maxStorageBufferRange;
        info.maximum_push_constant_bytes =
            candidate.properties.limits.maxPushConstantsSize;
        info.non_coherent_atom_size =
            candidate.properties.limits.nonCoherentAtomSize;
        info.timestamp_period_ns = candidate.properties.limits.timestampPeriod;
        info.timestamp_compute_and_graphics =
            candidate.properties.limits.timestampComputeAndGraphics == VK_TRUE;
        info.float_controls = {
            float_controls.shaderDenormPreserveFloat32 == VK_TRUE,
            float_controls.shaderDenormFlushToZeroFloat32 == VK_TRUE,
            float_controls.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE,
            float_controls.shaderRoundingModeRTEFloat32 == VK_TRUE,
            float_controls.shaderRoundingModeRTZFloat32 == VK_TRUE,
        };
        if (has_pci) {
            info.pci_address = VulkanPciAddress{
                pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction};
        }

        for (std::uint32_t heap = 0; heap < candidate.memory.memoryHeapCount; ++heap) {
            if ((candidate.memory.memoryHeaps[heap].flags
                 & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                info.maximum_device_heap_bytes = std::max<std::uint64_t>(
                    info.maximum_device_heap_bytes,
                    candidate.memory.memoryHeaps[heap].size);
            }
        }
        for (std::uint32_t type = 0; type < candidate.memory.memoryTypeCount; ++type) {
            const VkMemoryPropertyFlags flags =
                candidate.memory.memoryTypes[type].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
                info.has_host_coherent_staging = info.has_host_coherent_staging
                    || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                info.has_non_coherent_staging = info.has_non_coherent_staging
                    || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
            }
        }
        if (compute_queue.has_value()) {
            candidate.timestamp_valid_bits = queues[*compute_queue].timestampValidBits;
        }
        info.production_compute_supported = compute_queue.has_value()
            && candidate.properties.apiVersion >= VK_API_VERSION_1_2
            && candidate.properties.limits.maxComputeWorkGroupInvocations
                >= reduction_width
            && candidate.properties.limits.maxComputeWorkGroupSize[0]
                >= reduction_width
            && candidate.properties.limits.maxPushConstantsSize >= sizeof(gpu::AnalysisJob)
            && candidate.properties.limits.maxPerStageDescriptorStorageBuffers >= 9
            && candidate.properties.limits.maxDescriptorSetStorageBuffers
                >= descriptor_binding_count
            && info.maximum_device_heap_bytes > 0
            && (info.has_host_coherent_staging || info.has_non_coherent_staging);
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

[[nodiscard]] bool selector_matches(const VulkanDeviceSelector &selector,
                                    const VulkanDeviceInfo &info) {
    if (selector.uuid.has_value() && selector.uuid.value() != info.uuid) {
        return false;
    }
    if (selector.pci_address.has_value()
        && selector.pci_address != info.pci_address) {
        return false;
    }
    if (selector.ordinal.has_value() && selector.ordinal.value() != info.ordinal) {
        return false;
    }
    if (!selector.exact_name.empty() && selector.exact_name != info.name) {
        return false;
    }
    return true;
}

[[nodiscard]] bool selector_is_empty(const VulkanDeviceSelector &selector) noexcept {
    return !selector.uuid.has_value() && !selector.pci_address.has_value()
        && !selector.ordinal.has_value() && selector.exact_name.empty();
}

[[nodiscard]] std::size_t select_candidate(
    const std::vector<PhysicalCandidate> &candidates,
    const VulkanDeviceSelector &selector) {
    std::optional<std::size_t> selected;
    int selected_score = std::numeric_limits<int>::min();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const VulkanDeviceInfo &info = candidates[index].info;
        if (!selector_matches(selector, info)) {
            continue;
        }
        if (!info.production_compute_supported) {
            if (!selector_is_empty(selector)) {
                throw std::runtime_error(
                    "selected Vulkan device does not meet production compute limits");
            }
            continue;
        }
        int score = 0;
        switch (static_cast<VkPhysicalDeviceType>(info.device_type)) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 4; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 2; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 1; break;
        default: break;
        }
        if (!selected.has_value() || score > selected_score) {
            selected = index;
            selected_score = score;
        }
        if (!selector_is_empty(selector)) {
            break;
        }
    }
    if (!selected.has_value()) {
        throw std::runtime_error(selector_is_empty(selector)
            ? "no production-capable Vulkan compute device is available"
            : "Vulkan device selector did not match an eligible device");
    }
    return *selected;
}

struct PlanSlice {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct PlanArenaLayout {
    PlanSlice descriptors;
    PlanSlice transpose_offsets;
    PlanSlice transpose_indices;
    PlanSlice transpose_weights;
    PlanSlice lower_ld;
    PlanSlice upper_l;
    PlanSlice inverse_diagonal;
    PlanSlice forward_left;
    PlanSlice forward_weights;
    VkDeviceSize total_bytes = 0;
};

[[nodiscard]] VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment,
                                    std::string_view name) {
    if (alignment == 0) {
        throw std::invalid_argument(std::string{name} + " alignment is zero");
    }
    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0) return value;
    const VkDeviceSize padding = alignment - remainder;
    if (padding > std::numeric_limits<VkDeviceSize>::max() - value) {
        throw std::length_error(std::string{name} + " alignment overflow");
    }
    return value + padding;
}

template <class Value>
void append_slice(PlanSlice &slice, VkDeviceSize &cursor,
                  const std::vector<Value> &values, VkDeviceSize alignment,
                  std::string_view name) {
    if (values.empty()) {
        throw std::invalid_argument(std::string{"Vulkan packed "} + std::string{name}
                                    + " buffer is empty");
    }
    cursor = align_up(cursor, alignment, name);
    slice.offset = cursor;
    slice.size = checked_device_size(
        gpu::checked_product(values.size(), sizeof(Value), std::string{name}.c_str()),
        name);
    if (slice.size > std::numeric_limits<VkDeviceSize>::max() - cursor) {
        throw std::length_error(std::string{name} + " arena overflow");
    }
    cursor += slice.size;
}

[[nodiscard]] PlanArenaLayout plan_arena_layout(const gpu::PackedTile &packed,
                                                VkDeviceSize alignment) {
    PlanArenaLayout layout;
    VkDeviceSize cursor = 0;
    append_slice(layout.descriptors, cursor, packed.descriptors, alignment,
                 "descriptor");
    append_slice(layout.transpose_offsets, cursor, packed.transpose_offsets, alignment,
                 "transpose offset");
    append_slice(layout.transpose_indices, cursor, packed.transpose_indices, alignment,
                 "transpose index");
    append_slice(layout.transpose_weights, cursor, packed.transpose_weights, alignment,
                 "transpose weight");
    append_slice(layout.lower_ld, cursor, packed.lower_ld, alignment, "lower factor");
    append_slice(layout.upper_l, cursor, packed.upper_l, alignment, "upper factor");
    append_slice(layout.inverse_diagonal, cursor, packed.inverse_diagonal, alignment,
                 "inverse diagonal");
    append_slice(layout.forward_left, cursor, packed.forward_left, alignment,
                 "forward left");
    append_slice(layout.forward_weights, cursor, packed.forward_weights, alignment,
                 "forward weight");
    layout.total_bytes = cursor;
    return layout;
}

template <class Value>
void copy_slice(void *destination, const PlanSlice &slice,
                const std::vector<Value> &values) {
    std::memcpy(static_cast<std::byte *>(destination)
                    + static_cast<std::size_t>(slice.offset),
                values.data(), static_cast<std::size_t>(slice.size));
}

void copy_plan_arena(void *destination, const PlanArenaLayout &layout,
                     const gpu::PackedTile &packed) {
    copy_slice(destination, layout.descriptors, packed.descriptors);
    copy_slice(destination, layout.transpose_offsets, packed.transpose_offsets);
    copy_slice(destination, layout.transpose_indices, packed.transpose_indices);
    copy_slice(destination, layout.transpose_weights, packed.transpose_weights);
    copy_slice(destination, layout.lower_ld, packed.lower_ld);
    copy_slice(destination, layout.upper_l, packed.upper_l);
    copy_slice(destination, layout.inverse_diagonal, packed.inverse_diagonal);
    copy_slice(destination, layout.forward_left, packed.forward_left);
    copy_slice(destination, layout.forward_weights, packed.forward_weights);
}

[[nodiscard]] std::vector<std::uint32_t> shader_words(
    const unsigned char *bytes, std::size_t byte_count, std::string_view name) {
    if (byte_count == 0 || byte_count % sizeof(std::uint32_t) != 0) {
        throw std::runtime_error("embedded Vulkan " + std::string{name}
                                 + " SPIR-V has an invalid size");
    }
    std::vector<std::uint32_t> words(byte_count / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes, byte_count);
    if (words.front() != 0x07230203U) {
        throw std::runtime_error("embedded Vulkan " + std::string{name}
                                 + " SPIR-V has an invalid magic number");
    }
    return words;
}

} // namespace

namespace {

struct BufferResource {
    BufferResource() = default;
    ~BufferResource() { reset(); }

    BufferResource(const BufferResource &) = delete;
    BufferResource &operator=(const BufferResource &) = delete;

    BufferResource(BufferResource &&other) noexcept { *this = std::move(other); }
    BufferResource &operator=(BufferResource &&other) noexcept {
        if (this == &other) return *this;
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        vk = std::exchange(other.vk, nullptr);
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        mapped = std::exchange(other.mapped, nullptr);
        buffer_bytes = std::exchange(other.buffer_bytes, 0);
        allocation_bytes = std::exchange(other.allocation_bytes, 0);
        memory_type_index = std::exchange(other.memory_type_index, 0);
        memory_flags = std::exchange(other.memory_flags, 0);
        host_role = std::exchange(other.host_role, false);
        return *this;
    }

    void reset() noexcept {
        if (device == VK_NULL_HANDLE || vk == nullptr) return;
        if (mapped != nullptr && memory != VK_NULL_HANDLE) {
            vk->unmap_memory(device, memory);
        }
        if (buffer != VK_NULL_HANDLE) {
            vk->destroy_buffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vk->free_memory(device, memory, nullptr);
        }
        device = VK_NULL_HANDLE;
        vk = nullptr;
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        mapped = nullptr;
        buffer_bytes = 0;
        allocation_bytes = 0;
        memory_type_index = 0;
        memory_flags = 0;
        host_role = false;
    }

    [[nodiscard]] bool host_visible() const noexcept {
        return (memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    }
    [[nodiscard]] bool coherent() const noexcept {
        return (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }
    [[nodiscard]] bool device_local() const noexcept {
        return (memory_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    }

    VkDevice device = VK_NULL_HANDLE;
    const vulkan_detail::DeviceDispatch *vk = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize buffer_bytes = 0;
    VkDeviceSize allocation_bytes = 0;
    std::uint32_t memory_type_index = 0;
    VkMemoryPropertyFlags memory_flags = 0;
    bool host_role = false;
};

struct BufferLease {
    BufferResource *resource = nullptr;
    std::unique_ptr<BufferResource> transient;

    [[nodiscard]] BufferResource &get() const noexcept { return *resource; }
};

} // namespace

struct VulkanAnalysisEngine::Impl {
    explicit Impl(VulkanAnalysisOptions requested_options)
        : options(std::move(requested_options)),
          context(options.loader_path, options.enable_validation) {
        validate_options();
        const std::vector<PhysicalCandidate> candidates = enumerate_candidates(context);
        const std::size_t selected = select_candidate(candidates, options.device_selector);
        const PhysicalCandidate &physical = candidates[selected];
        info = physical.info;
        physical_device = physical.handle;
        memory_properties = physical.memory;
        physical_properties = physical.properties;
        timestamp_valid_bits = physical.timestamp_valid_bits;
        validate_device_options();

        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_create{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = info.compute_queue_family;
        queue_create.queueCount = 1;
        queue_create.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_create.queueCreateInfoCount = 1;
        device_create.pQueueCreateInfos = &queue_create;
        check_vk(context.vk.create_device(
                     physical_device, &device_create, nullptr, &device),
                 "vkCreateDevice");
        try {
            vk = vulkan_detail::VulkanLoader::load_device(
                device, context.vk.get_device_proc_addr);
        } catch (...) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(
                context.vk.get_device_proc_addr(device, "vkDestroyDevice"));
            if (destroy != nullptr) destroy(device, nullptr);
            device = VK_NULL_HANDLE;
            throw;
        }

        try {
            vk.get_device_queue(device, info.compute_queue_family, 0, &queue);
            create_execution_objects();
            for (const PipelineStage stage : {
                     PipelineStage::image_inverse,
                     PipelineStage::matrix_inverse,
                     PipelineStage::matrix_forward,
                     PipelineStage::metric,
                     PipelineStage::horizontal_first_metric}) {
                (void)pipeline(gpu::KernelShape::generic, stage);
            }
            if (options.kernel_dispatch != VulkanKernelDispatchPolicy::generic_only) {
                for (const gpu::KernelShape shape : {
                         gpu::KernelShape::bandwidth3,
                         gpu::KernelShape::bandwidth7}) {
                    for (const PipelineStage stage : {
                             PipelineStage::image_inverse,
                             PipelineStage::matrix_inverse,
                             PipelineStage::matrix_forward,
                             PipelineStage::metric,
                             PipelineStage::horizontal_first_metric}) {
                        (void)pipeline(shape, stage);
                    }
                }
            }
        } catch (...) {
            cleanup_device();
            throw;
        }
    }

    ~Impl() { cleanup_device(); }

    void validate_options() const {
        if (options.tile_size == 0 || options.reduction_groups_per_candidate == 0
            || options.inverse_threads_per_workgroup == 0) {
            throw std::invalid_argument(
                "Vulkan execution configuration counts must be positive");
        }
        if (options.tile_size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Vulkan tile size exceeds the supported range");
        }
        constexpr std::size_t maximum_groups =
            std::numeric_limits<std::uint32_t>::max() / reduction_width;
        if (options.reduction_groups_per_candidate > maximum_groups) {
            throw std::invalid_argument(
                "Vulkan reduction group count exceeds the 32-bit schedule range");
        }
        if (options.reuse_working_buffers
            && options.retained_working_buffer_limit_bytes == 0) {
            throw std::invalid_argument(
                "Vulkan retained-buffer limit must be positive when reuse is enabled");
        }
    }

    void validate_device_options() const {
        if (options.inverse_threads_per_workgroup
                > info.maximum_compute_workgroup_invocations
            || options.inverse_threads_per_workgroup
                > physical_properties.limits.maxComputeWorkGroupSize[0]) {
            throw std::invalid_argument(
                "Vulkan inverse workgroup size exceeds the device limit");
        }
    }

    void create_execution_objects() {
        std::array<VkDescriptorSetLayoutBinding, descriptor_binding_count> bindings{};
        for (std::uint32_t binding = 0; binding < descriptor_binding_count; ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[binding].descriptorCount = 1;
            bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_layout_create{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_layout_create.bindingCount = descriptor_binding_count;
        set_layout_create.pBindings = bindings.data();
        check_vk(vk.create_descriptor_set_layout(
                     device, &set_layout_create, nullptr, &descriptor_set_layout),
                 "vkCreateDescriptorSetLayout");

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(gpu::AnalysisJob);
        VkPipelineLayoutCreateInfo pipeline_layout_create{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_create.setLayoutCount = 1;
        pipeline_layout_create.pSetLayouts = &descriptor_set_layout;
        pipeline_layout_create.pushConstantRangeCount = 1;
        pipeline_layout_create.pPushConstantRanges = &push_range;
        check_vk(vk.create_pipeline_layout(
                     device, &pipeline_layout_create, nullptr, &pipeline_layout),
                 "vkCreatePipelineLayout");

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = descriptor_binding_count;
        VkDescriptorPoolCreateInfo pool_create{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_create.maxSets = 1;
        pool_create.poolSizeCount = 1;
        pool_create.pPoolSizes = &pool_size;
        check_vk(vk.create_descriptor_pool(
                     device, &pool_create, nullptr, &descriptor_pool),
                 "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo set_allocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_allocate.descriptorPool = descriptor_pool;
        set_allocate.descriptorSetCount = 1;
        set_allocate.pSetLayouts = &descriptor_set_layout;
        check_vk(vk.allocate_descriptor_sets(device, &set_allocate, &descriptor_set),
                 "vkAllocateDescriptorSets");

        VkPipelineCacheCreateInfo cache_create{
            VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        check_vk(vk.create_pipeline_cache(
                     device, &cache_create, nullptr, &pipeline_cache),
                 "vkCreatePipelineCache");

        VkCommandPoolCreateInfo command_pool_create{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_create.queueFamilyIndex = info.compute_queue_family;
        check_vk(vk.create_command_pool(
                     device, &command_pool_create, nullptr, &command_pool),
                 "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_allocate{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1;
        check_vk(vk.allocate_command_buffers(
                     device, &command_allocate, &command_buffer),
                 "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check_vk(vk.create_fence(device, &fence_create, nullptr, &fence),
                 "vkCreateFence");

        if (timestamp_valid_bits != 0 && info.timestamp_period_ns > 0.0F) {
            VkQueryPoolCreateInfo query_create{
                VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query_create.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_create.queryCount = timestamp_query_count;
            check_vk(vk.create_query_pool(
                         device, &query_create, nullptr, &query_pool),
                     "vkCreateQueryPool");
            telemetry.gpu_timestamps_available = true;
        }
    }

    [[nodiscard]] std::pair<const unsigned char *, std::size_t> shader_bytes(
        PipelineStage stage) const noexcept {
        switch (stage) {
        case PipelineStage::image_inverse:
            return {getnative_vulkan_inverse_axis_spv,
                    getnative_vulkan_inverse_axis_spv_size};
        case PipelineStage::matrix_inverse:
            return {getnative_vulkan_inverse_axis_matrix_spv,
                    getnative_vulkan_inverse_axis_matrix_spv_size};
        case PipelineStage::matrix_forward:
            return {getnative_vulkan_forward_axis_matrix_spv,
                    getnative_vulkan_forward_axis_matrix_spv_size};
        case PipelineStage::metric:
        case PipelineStage::horizontal_first_metric:
            return {getnative_vulkan_metric_axis_p1_spv,
                    getnative_vulkan_metric_axis_p1_spv_size};
        }
        return {nullptr, 0};
    }

    [[nodiscard]] VkPipeline pipeline(gpu::KernelShape shape,
                                      PipelineStage stage) {
        VkPipeline &cached = pipelines[shape_index(shape)][stage_index(stage)];
        if (cached != VK_NULL_HANDLE) return cached;

        const std::string name = std::string{stage_name(stage)} + '_'
            + shape_name(shape);
        const auto [bytes, byte_count] = shader_bytes(stage);
        const std::vector<std::uint32_t> words = shader_words(bytes, byte_count, name);
        VkShaderModuleCreateInfo module_create{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_create.codeSize = byte_count;
        module_create.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        check_vk(vk.create_shader_module(device, &module_create, nullptr, &module),
                 "vkCreateShaderModule");

        struct SpecializationData {
            std::uint32_t half_bandwidth;
            std::uint32_t forward_width;
            std::uint32_t bandwidth7_order;
            std::uint32_t horizontal_first;
            std::uint32_t inverse_local_size;
        } data{
            fixed_half_bandwidth(shape),
            fixed_forward_width(shape),
            static_cast<std::uint32_t>(shape == gpu::KernelShape::bandwidth7),
            static_cast<std::uint32_t>(
                stage == PipelineStage::horizontal_first_metric),
            static_cast<std::uint32_t>(options.inverse_threads_per_workgroup),
        };
        std::array<VkSpecializationMapEntry, 3> inverse_entries{
            VkSpecializationMapEntry{0, offsetof(SpecializationData, half_bandwidth), 4},
            VkSpecializationMapEntry{2, offsetof(SpecializationData, bandwidth7_order), 4},
            VkSpecializationMapEntry{4, offsetof(SpecializationData, inverse_local_size), 4},
        };
        std::array<VkSpecializationMapEntry, 2> forward_entries{
            VkSpecializationMapEntry{1, offsetof(SpecializationData, forward_width), 4},
            VkSpecializationMapEntry{4, offsetof(SpecializationData, inverse_local_size), 4},
        };
        std::array<VkSpecializationMapEntry, 2> metric_entries{
            VkSpecializationMapEntry{1, offsetof(SpecializationData, forward_width), 4},
            VkSpecializationMapEntry{3, offsetof(SpecializationData, horizontal_first), 4},
        };
        VkSpecializationInfo specialization{};
        specialization.dataSize = sizeof(data);
        specialization.pData = &data;
        if (stage == PipelineStage::image_inverse
            || stage == PipelineStage::matrix_inverse) {
            specialization.mapEntryCount = static_cast<std::uint32_t>(
                inverse_entries.size());
            specialization.pMapEntries = inverse_entries.data();
        } else if (stage == PipelineStage::matrix_forward) {
            specialization.mapEntryCount = static_cast<std::uint32_t>(
                forward_entries.size());
            specialization.pMapEntries = forward_entries.data();
        } else {
            specialization.mapEntryCount = static_cast<std::uint32_t>(
                metric_entries.size());
            specialization.pMapEntries = metric_entries.data();
        }

        VkPipelineShaderStageCreateInfo shader_stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shader_stage.module = module;
        shader_stage.pName = "main";
        shader_stage.pSpecializationInfo = &specialization;
        VkComputePipelineCreateInfo pipeline_create{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_create.stage = shader_stage;
        pipeline_create.layout = pipeline_layout;
        const auto start = std::chrono::steady_clock::now();
        const VkResult result = vk.create_compute_pipelines(
            device, pipeline_cache, 1, &pipeline_create, nullptr, &cached);
        telemetry.pipeline_creation_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        vk.destroy_shader_module(device, module, nullptr);
        check_vk(result, "vkCreateComputePipelines");
        telemetry.created_pipeline_names.push_back(name);
        return cached;
    }

    [[nodiscard]] std::uint32_t find_memory_type(
        std::uint32_t type_bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) const {
        std::optional<std::uint32_t> fallback;
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount;
             ++index) {
            if ((type_bits & (1U << index)) == 0) continue;
            const VkMemoryPropertyFlags flags =
                memory_properties.memoryTypes[index].propertyFlags;
            if ((flags & required) != required) continue;
            if ((flags & preferred) == preferred) return index;
            if (!fallback.has_value()) fallback = index;
        }
        if (!fallback.has_value()) {
            throw std::runtime_error("Vulkan has no compatible memory type");
        }
        return *fallback;
    }

    void add_telemetry_bytes(std::size_t &target, VkDeviceSize bytes,
                             std::string_view name) {
        if (bytes > std::numeric_limits<std::size_t>::max()
            || static_cast<std::size_t>(bytes)
                > std::numeric_limits<std::size_t>::max() - target) {
            throw std::length_error(std::string{name} + " telemetry overflow");
        }
        target += static_cast<std::size_t>(bytes);
    }

    [[nodiscard]] BufferResource allocate_buffer(
        VkDeviceSize bytes, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
        bool host_role, bool working_buffer) {
        if (bytes == 0) {
            throw std::invalid_argument("Vulkan buffer must not be empty");
        }
        BufferResource resource;
        resource.device = device;
        resource.vk = &vk;
        resource.buffer_bytes = bytes;
        VkBufferCreateInfo buffer_create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_create.size = bytes;
        buffer_create.usage = usage;
        buffer_create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const auto start = std::chrono::steady_clock::now();
        check_vk(vk.create_buffer(device, &buffer_create, nullptr, &resource.buffer),
                 "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vk.get_buffer_memory_requirements(device, resource.buffer, &requirements);
        resource.memory_type_index = find_memory_type(
            requirements.memoryTypeBits, required, preferred);
        resource.memory_flags = memory_properties.memoryTypes[
            resource.memory_type_index].propertyFlags;
        resource.allocation_bytes = requirements.size;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = resource.memory_type_index;
        check_vk(vk.allocate_memory(device, &allocate, nullptr, &resource.memory),
                 "vkAllocateMemory");
        check_vk(vk.bind_buffer_memory(device, resource.buffer, resource.memory, 0),
                 "vkBindBufferMemory");
        if ((resource.memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            check_vk(vk.map_memory(device, resource.memory, 0, requirements.size,
                                   0, &resource.mapped),
                     "vkMapMemory");
        }
        telemetry.buffer_allocation_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        ++telemetry.buffer_allocation_count;
        add_telemetry_bytes(telemetry.buffer_allocation_bytes,
                            resource.allocation_bytes, "buffer allocation");
        if (host_role) {
            add_telemetry_bytes(telemetry.host_buffer_allocation_bytes,
                                resource.allocation_bytes, "host allocation");
        } else {
            add_telemetry_bytes(telemetry.device_buffer_allocation_bytes,
                                resource.allocation_bytes, "device allocation");
        }
        if (working_buffer) {
            ++telemetry.working_buffer_allocation_count;
            add_telemetry_bytes(telemetry.working_buffer_allocation_bytes,
                                resource.allocation_bytes,
                                "working buffer allocation");
        }
        resource.host_role = host_role;
        return resource;
    }

    [[nodiscard]] std::size_t retained_bytes() const {
        const std::array<const BufferResource *, 7> resources{
            &retained_source_device, &retained_source_staging,
            &retained_plan_device, &retained_plan_staging,
            &retained_workspace, &retained_partial, &retained_readback};
        std::size_t result = 0;
        for (const BufferResource *resource : resources) {
            result = checked_add(result,
                static_cast<std::size_t>(resource->allocation_bytes),
                "Vulkan retained buffer");
        }
        return result;
    }

    [[nodiscard]] BufferLease acquire_buffer(
        BufferResource &retained, VkDeviceSize bytes, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
        bool host_role) {
        BufferLease lease;
        if (options.reuse_working_buffers && retained.buffer != VK_NULL_HANDLE
            && retained.buffer_bytes >= bytes) {
            ++telemetry.working_buffer_reuse_count;
            lease.resource = &retained;
            return lease;
        }

        BufferResource allocated = allocate_buffer(
            bytes, usage, required, preferred, host_role, true);
        const std::size_t old_bytes = static_cast<std::size_t>(
            retained.allocation_bytes);
        const std::size_t new_bytes = static_cast<std::size_t>(
            allocated.allocation_bytes);
        const std::size_t retained_without_old = retained_bytes() - old_bytes;
        if (options.reuse_working_buffers
            && new_bytes <= options.retained_working_buffer_limit_bytes
            && retained_without_old
                <= options.retained_working_buffer_limit_bytes - new_bytes) {
            retained = std::move(allocated);
            lease.resource = &retained;
        } else {
            lease.transient = std::make_unique<BufferResource>(
                std::move(allocated));
            lease.resource = lease.transient.get();
        }
        return lease;
    }

    void update_memory_gauges(std::span<BufferLease *const> active) {
        std::size_t active_bytes = 0;
        std::size_t transient_bytes = 0;
        std::size_t transient_host = 0;
        std::size_t transient_device = 0;
        for (const BufferLease *lease : active) {
            active_bytes = checked_add(
                active_bytes,
                static_cast<std::size_t>(lease->get().allocation_bytes),
                "Vulkan active buffers");
            if (lease->transient) {
                transient_bytes = checked_add(
                    transient_bytes,
                    static_cast<std::size_t>(lease->get().allocation_bytes),
                    "Vulkan transient buffers");
                std::size_t &role = lease->get().host_role
                    ? transient_host : transient_device;
                role = checked_add(role,
                    static_cast<std::size_t>(lease->get().allocation_bytes),
                    "Vulkan transient role buffers");
            }
        }
        telemetry.working_buffer_active_bytes = active_bytes;
        telemetry.working_buffer_peak_active_bytes = std::max(
            telemetry.working_buffer_peak_active_bytes, active_bytes);
        const std::size_t retained = retained_bytes();
        telemetry.working_buffer_retained_bytes = retained;
        telemetry.working_buffer_peak_retained_bytes = std::max(
            telemetry.working_buffer_peak_retained_bytes, retained);

        std::size_t retained_host = 0;
        std::size_t retained_device = 0;
        const std::array<const BufferResource *, 7> resources{
            &retained_source_device, &retained_source_staging,
            &retained_plan_device, &retained_plan_staging,
            &retained_workspace, &retained_partial, &retained_readback};
        for (const BufferResource *resource : resources) {
            std::size_t &role = resource->host_role ? retained_host : retained_device;
            role = checked_add(role,
                static_cast<std::size_t>(resource->allocation_bytes),
                "Vulkan retained role buffers");
        }
        const std::size_t explicit_host = checked_add(
            retained_host, transient_host, "Vulkan explicit host bytes");
        const std::size_t explicit_device = checked_add(
            retained_device, transient_device, "Vulkan explicit device bytes");
        const std::size_t explicit_total = checked_add(
            retained, transient_bytes, "Vulkan explicit total bytes");
        telemetry.peak_host_bytes = std::max(
            telemetry.peak_host_bytes, explicit_host);
        telemetry.peak_device_bytes = std::max(
            telemetry.peak_device_bytes, explicit_device);
        telemetry.peak_total_explicit_bytes = std::max(
            telemetry.peak_total_explicit_bytes, explicit_total);
        peak_working_set_bytes = std::max(peak_working_set_bytes, explicit_total);
    }

    void flush(BufferResource &buffer, VkDeviceSize offset, VkDeviceSize bytes) {
        if (buffer.coherent()) return;
        const auto range = vulkan_detail::aligned_noncoherent_range(
            offset, bytes, info.non_coherent_atom_size, buffer.allocation_bytes);
        VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mapped.memory = buffer.memory;
        mapped.offset = range.offset;
        mapped.size = range.size;
        check_vk(vk.flush_mapped_memory_ranges(device, 1, &mapped),
                 "vkFlushMappedMemoryRanges");
        ++telemetry.flush_count;
        add_telemetry_bytes(telemetry.flushed_bytes, range.size, "flushed bytes");
        telemetry.used_non_coherent_upload = true;
    }

    void invalidate(BufferResource &buffer, VkDeviceSize offset,
                    VkDeviceSize bytes) {
        if (buffer.coherent()) return;
        const auto range = vulkan_detail::aligned_noncoherent_range(
            offset, bytes, info.non_coherent_atom_size, buffer.allocation_bytes);
        VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mapped.memory = buffer.memory;
        mapped.offset = range.offset;
        mapped.size = range.size;
        check_vk(vk.invalidate_mapped_memory_ranges(device, 1, &mapped),
                 "vkInvalidateMappedMemoryRanges");
        ++telemetry.invalidate_count;
        add_telemetry_bytes(telemetry.invalidated_bytes, range.size,
                            "invalidated bytes");
        telemetry.used_non_coherent_readback = true;
    }

    void wire_descriptors(const BufferResource &source,
                          VkDeviceSize source_bytes,
                          const BufferResource &plan,
                          const PlanArenaLayout &layout,
                          const BufferResource &workspace,
                          VkDeviceSize workspace_bytes,
                          const BufferResource &partial,
                          VkDeviceSize partial_bytes) {
        const std::array<PlanSlice, 9> slices{
            layout.descriptors, layout.transpose_offsets,
            layout.transpose_indices, layout.transpose_weights,
            layout.lower_ld, layout.upper_l, layout.inverse_diagonal,
            layout.forward_left, layout.forward_weights};
        std::array<VkDescriptorBufferInfo, descriptor_binding_count> infos{};
        infos[0] = {source.buffer, 0, source_bytes};
        for (std::size_t index = 0; index < slices.size(); ++index) {
            infos[index + 1] = {plan.buffer, slices[index].offset,
                                slices[index].size};
        }
        infos[10] = {workspace.buffer, 0, workspace_bytes};
        infos[11] = {partial.buffer, 0, partial_bytes};
        for (const VkDescriptorBufferInfo &buffer_info : infos) {
            if (buffer_info.range == 0
                || buffer_info.range > info.maximum_storage_buffer_range) {
                throw std::length_error(
                    "Vulkan descriptor range exceeds maxStorageBufferRange");
            }
        }
        std::array<VkWriteDescriptorSet, descriptor_binding_count> writes{};
        for (std::uint32_t binding = 0; binding < descriptor_binding_count;
             ++binding) {
            writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[binding].dstSet = descriptor_set;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &infos[binding];
        }
        const auto start = std::chrono::steady_clock::now();
        vk.update_descriptor_sets(device, descriptor_binding_count,
                                  writes.data(), 0, nullptr);
        telemetry.descriptor_wiring_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    void workspace_barrier() const {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_SHADER_WRITE_BIT;
        vk.cmd_pipeline_barrier(
            command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            1, &barrier, 0, nullptr, 0, nullptr);
    }

    void bind_and_dispatch(VkPipeline selected_pipeline,
                           const gpu::AnalysisJob &job,
                           std::uint32_t workgroups) const {
        if (workgroups == 0
            || workgroups > physical_properties.limits.maxComputeWorkGroupCount[0]) {
            throw std::length_error("Vulkan dispatch exceeds the workgroup-count limit");
        }
        vk.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                             selected_pipeline);
        vk.cmd_bind_descriptor_sets(
            command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
            0, 1, &descriptor_set, 0, nullptr);
        vk.cmd_push_constants(command_buffer, pipeline_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(job), &job);
        vk.cmd_dispatch(command_buffer, workgroups, 1, 1);
    }

    void accumulate_timestamps(std::span<const PipelineStage> stages) {
        if (query_pool == VK_NULL_HANDLE || stages.empty()) return;
        std::array<std::uint64_t, timestamp_query_count> timestamps{};
        const std::uint32_t count = static_cast<std::uint32_t>(stages.size() + 1);
        const VkResult result = vk.get_query_pool_results(
            device, query_pool, 0, count,
            count * sizeof(std::uint64_t), timestamps.data(),
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        check_vk(result, "vkGetQueryPoolResults");
        const std::uint64_t mask = timestamp_valid_bits >= 64
            ? std::numeric_limits<std::uint64_t>::max()
            : (std::uint64_t{1} << timestamp_valid_bits) - 1U;
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const std::uint64_t ticks =
                (timestamps[index + 1] - timestamps[index]) & mask;
            const double milliseconds = static_cast<double>(ticks)
                * static_cast<double>(info.timestamp_period_ns) / 1'000'000.0;
            telemetry.gpu_execution_ms += milliseconds;
            switch (stages[index]) {
            case PipelineStage::image_inverse:
                telemetry.inverse_image_ms += milliseconds;
                break;
            case PipelineStage::matrix_inverse:
                telemetry.inverse_matrix_ms += milliseconds;
                break;
            case PipelineStage::matrix_forward:
                telemetry.first_forward_ms += milliseconds;
                break;
            case PipelineStage::metric:
            case PipelineStage::horizontal_first_metric:
                telemetry.metric_ms += milliseconds;
                break;
            }
        }
    }

    [[nodiscard]] std::vector<CandidateResult> analyze(
        ConstImageView source, std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::stop_token stop) {
        if (device_lost) {
            throw std::runtime_error("Vulkan device is lost");
        }
        const gpu::MetricCropBounds crop =
            gpu::validate_source_and_metric(source, metric);
        if (candidates.empty()) return {};
        if (stop.stop_requested()) {
            throw std::runtime_error("Vulkan analysis cancelled");
        }

        const std::size_t image_elements = gpu::checked_product(
            static_cast<std::size_t>(source.width),
            static_cast<std::size_t>(source.height), "Vulkan source image");
        const std::size_t source_bytes_host = gpu::checked_product(
            image_elements, sizeof(float), "Vulkan source buffer");
        if (source_bytes_host > info.maximum_storage_buffer_range) {
            throw std::length_error(
                "Vulkan source image exceeds maxStorageBufferRange");
        }
        std::vector<float> contiguous_source;
        const float *source_data = source.data;
        if (source.stride != source.width) {
            contiguous_source.resize(image_elements);
            for (std::int32_t y = 0; y < source.height; ++y) {
                std::copy_n(source.data
                                + static_cast<std::ptrdiff_t>(y) * source.stride,
                            source.width,
                            contiguous_source.data()
                                + static_cast<std::ptrdiff_t>(y) * source.width);
            }
            source_data = contiguous_source.data();
        }

        const std::size_t device_workspace_limit =
            static_cast<std::size_t>(info.maximum_storage_buffer_range)
            / sizeof(float);
        const std::size_t requested_workspace_limit =
            options.workspace_limit_elements == 0
            ? device_workspace_limit
            : std::min(device_workspace_limit,
                       options.workspace_limit_elements);
        const std::size_t workspace_limit = std::min(
            requested_workspace_limit,
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
        const gpu::TiledBatch batch = gpu::plan_tiles(
            source, candidates, options.tile_size, workspace_limit,
            dispatch_policy(options.kernel_dispatch));
        if (stop.stop_requested()) {
            throw std::runtime_error("Vulkan analysis cancelled");
        }
        peak_workspace_elements = std::max(
            peak_workspace_elements, batch.maximum_workspace_elements);

        std::size_t maximum_tile_candidates = 0;
        for (const gpu::TileRange &tile : batch.tiles) {
            maximum_tile_candidates = std::max(
                maximum_tile_candidates, tile.end - tile.begin);
        }
        const std::size_t maximum_partial_count = gpu::checked_product(
            maximum_tile_candidates, options.reduction_groups_per_candidate,
            "Vulkan tile partials");
        const std::size_t workspace_bytes_host = gpu::checked_product(
            batch.maximum_workspace_elements, sizeof(float),
            "Vulkan workspace");
        const std::size_t partial_bytes_host = gpu::checked_product(
            maximum_partial_count, sizeof(float), "Vulkan partials");
        const VkDeviceSize source_bytes = checked_device_size(
            source_bytes_host, "Vulkan source");
        const VkDeviceSize workspace_bytes = checked_device_size(
            workspace_bytes_host, "Vulkan workspace");
        const VkDeviceSize partial_bytes = checked_device_size(
            partial_bytes_host, "Vulkan partials");

        BufferLease source_device = acquire_buffer(
            retained_source_device, source_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        BufferLease source_staging = acquire_buffer(
            retained_source_staging, source_bytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);
        BufferLease workspace = acquire_buffer(
            retained_workspace, workspace_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        BufferLease partial = acquire_buffer(
            retained_partial, partial_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        BufferLease readback = acquire_buffer(
            retained_readback, partial_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);
        std::array<BufferLease *, 5> base_active{
            &source_device, &source_staging, &workspace, &partial, &readback};
        update_memory_gauges(base_active);

        try {
            const auto source_upload_start = std::chrono::steady_clock::now();
            std::memcpy(source_staging.get().mapped, source_data, source_bytes_host);
            flush(source_staging.get(), 0, source_bytes);
            telemetry.source_upload_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - source_upload_start).count();

            const std::size_t all_partial_count = gpu::checked_product(
                candidates.size(), options.reduction_groups_per_candidate,
                "Vulkan complete partials");
            std::vector<float> all_partials(all_partial_count);
            bool upload_source = true;

            for (const gpu::TileRange &tile : batch.tiles) {
                if (stop.stop_requested()) {
                    throw std::runtime_error("Vulkan analysis cancelled");
                }
                const auto tile_candidates = candidates.subspan(
                    tile.begin, tile.end - tile.begin);
                const auto pack_start = std::chrono::steady_clock::now();
                const gpu::PackedTile packed = gpu::pack_tile(
                    source, tile_candidates,
                    dispatch_policy(options.kernel_dispatch));
                const PlanArenaLayout arena = plan_arena_layout(
                    packed,
                    physical_properties.limits.minStorageBufferOffsetAlignment);
                telemetry.plan_pack_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - pack_start).count();
                if (packed.workspace_elements != tile.workspace_elements) {
                    throw std::logic_error(
                        "Vulkan packed workspace differs from tile planning");
                }

                BufferLease plan_device = acquire_buffer(
                    retained_plan_device, arena.total_bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
                BufferLease plan_staging = acquire_buffer(
                    retained_plan_staging, arena.total_bytes,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);
                std::array<BufferLease *, 7> tile_active{
                    &source_device, &source_staging, &workspace, &partial,
                    &readback, &plan_device, &plan_staging};
                update_memory_gauges(tile_active);

                const auto plan_upload_start = std::chrono::steady_clock::now();
                copy_plan_arena(plan_staging.get().mapped, arena, packed);
                flush(plan_staging.get(), 0, arena.total_bytes);
                telemetry.plan_upload_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - plan_upload_start).count();
                add_telemetry_bytes(telemetry.plan_upload_bytes,
                                    arena.total_bytes, "plan upload bytes");

                const std::size_t tile_candidate_count = tile.end - tile.begin;
                const std::size_t tile_partial_count = gpu::checked_product(
                    tile_candidate_count, options.reduction_groups_per_candidate,
                    "Vulkan tile partial count");
                const VkDeviceSize tile_partial_bytes = checked_device_size(
                    gpu::checked_product(tile_partial_count, sizeof(float),
                                         "Vulkan tile partial bytes"),
                    "Vulkan tile partial bytes");
                wire_descriptors(
                    source_device.get(), source_bytes,
                    plan_device.get(), arena,
                    workspace.get(), checked_device_size(
                        tile.workspace_elements * sizeof(float),
                        "Vulkan tile workspace range"),
                    partial.get(), tile_partial_bytes);

                check_vk(vk.reset_fences(device, 1, &fence), "vkResetFences");
                check_vk(vk.reset_command_buffer(command_buffer, 0),
                         "vkResetCommandBuffer");
                VkCommandBufferBeginInfo begin{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check_vk(vk.begin_command_buffer(command_buffer, &begin),
                         "vkBeginCommandBuffer");
                if (query_pool != VK_NULL_HANDLE) {
                    vk.cmd_reset_query_pool(command_buffer, query_pool, 0,
                                            timestamp_query_count);
                    vk.cmd_write_timestamp(command_buffer,
                                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                           query_pool, 0);
                }

                std::array<VkBufferMemoryBarrier, 2> upload_barriers{};
                std::uint32_t upload_barrier_count = 0;
                if (upload_source) {
                    const VkBufferCopy source_copy{0, 0, source_bytes};
                    vk.cmd_copy_buffer(command_buffer,
                                       source_staging.get().buffer,
                                       source_device.get().buffer,
                                       1, &source_copy);
                    VkBufferMemoryBarrier &barrier =
                        upload_barriers[upload_barrier_count++];
                    barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.buffer = source_device.get().buffer;
                    barrier.offset = 0;
                    barrier.size = source_bytes;
                }
                const VkBufferCopy plan_copy{0, 0, arena.total_bytes};
                vk.cmd_copy_buffer(command_buffer,
                                   plan_staging.get().buffer,
                                   plan_device.get().buffer,
                                   1, &plan_copy);
                VkBufferMemoryBarrier &plan_barrier =
                    upload_barriers[upload_barrier_count++];
                plan_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                plan_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                plan_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                plan_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                plan_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                plan_barrier.buffer = plan_device.get().buffer;
                plan_barrier.offset = 0;
                plan_barrier.size = arena.total_bytes;
                vk.cmd_pipeline_barrier(
                    command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                    0, nullptr, upload_barrier_count, upload_barriers.data(),
                    0, nullptr);

                const gpu::AnalysisJob base_job{
                    static_cast<std::uint32_t>(source.width),
                    static_cast<std::uint32_t>(source.height),
                    crop.left, crop.right, crop.top, crop.bottom,
                    metric.threshold,
                    static_cast<std::uint32_t>(
                        options.reduction_groups_per_candidate),
                    static_cast<std::uint32_t>(tile_candidate_count),
                    0,
                };
                std::array<PipelineStage, 4> executed{};
                std::size_t executed_count = 0;
                const auto mark_stage = [&](PipelineStage stage) {
                    executed[executed_count] = stage;
                    ++executed_count;
                    if (query_pool != VK_NULL_HANDLE) {
                        vk.cmd_write_timestamp(
                            command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            query_pool, static_cast<std::uint32_t>(executed_count));
                    }
                };

                if (tile.signature.axes != AnalysisAxes::both) {
                    const bool horizontal =
                        tile.signature.axes == AnalysisAxes::horizontal;
                    const gpu::KernelShape inverse_shape = horizontal
                        ? tile.signature.horizontal_inverse_shape
                        : tile.signature.vertical_inverse_shape;
                    const gpu::KernelShape forward_shape = horizontal
                        ? tile.signature.horizontal_forward_shape
                        : tile.signature.vertical_forward_shape;
                    gpu::AnalysisJob inverse_job = base_job;
                    inverse_job.maximum_vector_count = packed.maximum_vector_count;
                    const std::size_t inverse_count = gpu::checked_product(
                        tile_candidate_count, packed.maximum_vector_count,
                        "Vulkan inverse dispatch");
                    bind_and_dispatch(
                        pipeline(inverse_shape, PipelineStage::image_inverse),
                        inverse_job,
                        ceil_div_u32(inverse_count,
                                     options.inverse_threads_per_workgroup,
                                     "Vulkan inverse dispatch"));
                    mark_stage(PipelineStage::image_inverse);
                    workspace_barrier();
                    gpu::AnalysisJob metric_job = base_job;
                    metric_job.maximum_vector_count = 0;
                    bind_and_dispatch(
                        pipeline(forward_shape, PipelineStage::metric),
                        metric_job,
                        gpu::checked_u32(tile_partial_count,
                                         "Vulkan metric dispatch"));
                    mark_stage(PipelineStage::metric);
                } else {
                    gpu::AnalysisJob horizontal_job = base_job;
                    horizontal_job.maximum_vector_count =
                        static_cast<std::uint32_t>(source.height);
                    const std::size_t horizontal_count = gpu::checked_product(
                        tile_candidate_count,
                        static_cast<std::size_t>(source.height),
                        "Vulkan two-axis horizontal inverse dispatch");
                    bind_and_dispatch(
                        pipeline(tile.signature.horizontal_inverse_shape,
                                 PipelineStage::image_inverse),
                        horizontal_job,
                        ceil_div_u32(horizontal_count,
                                     options.inverse_threads_per_workgroup,
                                     "Vulkan horizontal inverse dispatch"));
                    mark_stage(PipelineStage::image_inverse);
                    workspace_barrier();

                    gpu::AnalysisJob vertical_job = base_job;
                    vertical_job.maximum_vector_count = packed.maximum_native_width;
                    const std::size_t vertical_count = gpu::checked_product(
                        tile_candidate_count, packed.maximum_native_width,
                        "Vulkan two-axis vertical inverse dispatch");
                    bind_and_dispatch(
                        pipeline(tile.signature.vertical_inverse_shape,
                                 PipelineStage::matrix_inverse),
                        vertical_job,
                        ceil_div_u32(vertical_count,
                                     options.inverse_threads_per_workgroup,
                                     "Vulkan vertical inverse dispatch"));
                    mark_stage(PipelineStage::matrix_inverse);
                    workspace_barrier();

                    const bool vertical_first = tile.signature.forward_order
                        == ForwardOrder::vertical_first;
                    gpu::AnalysisJob forward_job = base_job;
                    forward_job.maximum_vector_count = vertical_first
                        ? packed.maximum_native_width
                        : packed.maximum_native_height;
                    forward_job.crop_left = vertical_first
                        ? static_cast<std::uint32_t>(tile_candidate_count) : 0;
                    const std::size_t forward_count = gpu::checked_product(
                        tile_candidate_count, forward_job.maximum_vector_count,
                        "Vulkan first forward dispatch");
                    const gpu::KernelShape first_forward_shape = vertical_first
                        ? tile.signature.vertical_forward_shape
                        : tile.signature.horizontal_forward_shape;
                    bind_and_dispatch(
                        pipeline(first_forward_shape,
                                 PipelineStage::matrix_forward),
                        forward_job,
                        ceil_div_u32(forward_count,
                                     options.inverse_threads_per_workgroup,
                                     "Vulkan first forward dispatch"));
                    mark_stage(PipelineStage::matrix_forward);
                    workspace_barrier();

                    gpu::AnalysisJob metric_job = base_job;
                    metric_job.maximum_vector_count = vertical_first
                        ? 0 : static_cast<std::uint32_t>(tile_candidate_count);
                    const PipelineStage metric_stage = vertical_first
                        ? PipelineStage::metric
                        : PipelineStage::horizontal_first_metric;
                    const gpu::KernelShape final_shape = vertical_first
                        ? tile.signature.horizontal_forward_shape
                        : tile.signature.vertical_forward_shape;
                    bind_and_dispatch(
                        pipeline(final_shape, metric_stage), metric_job,
                        gpu::checked_u32(tile_partial_count,
                                         "Vulkan metric dispatch"));
                    mark_stage(metric_stage);
                }

                VkBufferMemoryBarrier readback_barrier{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                readback_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                readback_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                readback_barrier.buffer = partial.get().buffer;
                readback_barrier.offset = 0;
                readback_barrier.size = tile_partial_bytes;
                vk.cmd_pipeline_barrier(
                    command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    0, nullptr, 1, &readback_barrier, 0, nullptr);
                const VkBufferCopy partial_copy{0, 0, tile_partial_bytes};
                vk.cmd_copy_buffer(command_buffer, partial.get().buffer,
                                   readback.get().buffer, 1, &partial_copy);
                check_vk(vk.end_command_buffer(command_buffer),
                         "vkEndCommandBuffer");

                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &command_buffer;
                bool submitted = false;
                bool completed = false;
                try {
                    const VkResult submit_result = vk.queue_submit(
                        queue, 1, &submit, fence);
                    if (submit_result == VK_ERROR_DEVICE_LOST) device_lost = true;
                    check_vk(submit_result, "vkQueueSubmit");
                    submitted = true;
                    ++telemetry.queue_submission_count;
                    const VkResult wait_result = vk.wait_for_fences(
                        device, 1, &fence, VK_TRUE,
                        std::numeric_limits<std::uint64_t>::max());
                    if (wait_result == VK_ERROR_DEVICE_LOST) device_lost = true;
                    check_vk(wait_result, "vkWaitForFences");
                    completed = true;
                    ++telemetry.queue_completion_count;
                } catch (...) {
                    const std::exception_ptr original = std::current_exception();
                    if (submitted && !completed && !device_lost) {
                        const VkResult drain = vk.queue_wait_idle(queue);
                        if (drain == VK_SUCCESS) {
                            ++telemetry.queue_completion_count;
                        } else if (drain == VK_ERROR_DEVICE_LOST) {
                            device_lost = true;
                        }
                    }
                    std::rethrow_exception(original);
                }
                upload_source = false;
                if (stop.stop_requested()) {
                    throw std::runtime_error("Vulkan analysis cancelled");
                }

                const auto readback_start = std::chrono::steady_clock::now();
                invalidate(readback.get(), 0, tile_partial_bytes);
                const std::size_t partial_offset = gpu::checked_product(
                    tile.begin, options.reduction_groups_per_candidate,
                    "Vulkan partial result offset");
                std::memcpy(all_partials.data() + partial_offset,
                            readback.get().mapped,
                            static_cast<std::size_t>(tile_partial_bytes));
                telemetry.partial_readback_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - readback_start).count();
                add_telemetry_bytes(telemetry.partial_readback_bytes,
                                    tile_partial_bytes, "partial readback bytes");
                accumulate_timestamps(std::span<const PipelineStage>{
                    executed.data(), executed_count});
                ++telemetry.analyzed_tile_count;
                if (gpu::uses_specialized_pipeline(tile.signature)) {
                    ++telemetry.specialized_tile_count;
                } else {
                    ++telemetry.generic_tile_count;
                }
            }

            const auto merge_start = std::chrono::steady_clock::now();
            std::vector<CandidateResult> results = gpu::merge_metric_partials(
                candidates, all_partials,
                options.reduction_groups_per_candidate, crop.pixel_count);
            telemetry.cpu_merge_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - merge_start).count();
            telemetry.working_buffer_active_bytes = 0;
            telemetry.working_buffer_retained_bytes = retained_bytes();
            return results;
        } catch (...) {
            telemetry.working_buffer_active_bytes = 0;
            telemetry.working_buffer_retained_bytes = retained_bytes();
            throw;
        }
    }

    void cleanup_device() noexcept {
        if (device == VK_NULL_HANDLE || vk.destroy_device == nullptr) return;
        (void)vk.device_wait_idle(device);
        clear_retained_buffers();
        for (auto &shape_pipelines : pipelines) {
            for (VkPipeline &value : shape_pipelines) {
                if (value != VK_NULL_HANDLE) {
                    vk.destroy_pipeline(device, value, nullptr);
                    value = VK_NULL_HANDLE;
                }
            }
        }
        if (query_pool != VK_NULL_HANDLE) {
            vk.destroy_query_pool(device, query_pool, nullptr);
            query_pool = VK_NULL_HANDLE;
        }
        if (fence != VK_NULL_HANDLE) {
            vk.destroy_fence(device, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
        if (command_pool != VK_NULL_HANDLE) {
            vk.destroy_command_pool(device, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
            command_buffer = VK_NULL_HANDLE;
        }
        if (pipeline_cache != VK_NULL_HANDLE) {
            vk.destroy_pipeline_cache(device, pipeline_cache, nullptr);
            pipeline_cache = VK_NULL_HANDLE;
        }
        if (descriptor_pool != VK_NULL_HANDLE) {
            vk.destroy_descriptor_pool(device, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
            descriptor_set = VK_NULL_HANDLE;
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vk.destroy_pipeline_layout(device, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }
        if (descriptor_set_layout != VK_NULL_HANDLE) {
            vk.destroy_descriptor_set_layout(device, descriptor_set_layout, nullptr);
            descriptor_set_layout = VK_NULL_HANDLE;
        }
        vk.destroy_device(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    VulkanAnalysisOptions options;
    InstanceContext context;
    VulkanDeviceInfo info{};
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physical_properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::uint32_t timestamp_valid_bits = 0;
    VkDevice device = VK_NULL_HANDLE;
    vulkan_detail::DeviceDispatch vk{};
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool query_pool = VK_NULL_HANDLE;
    std::array<std::array<VkPipeline, 5>, 5> pipelines{};
    BufferResource retained_source_device;
    BufferResource retained_source_staging;
    BufferResource retained_plan_device;
    BufferResource retained_plan_staging;
    BufferResource retained_workspace;
    BufferResource retained_partial;
    BufferResource retained_readback;
    std::size_t peak_workspace_elements = 0;
    std::size_t peak_working_set_bytes = 0;
    VulkanRuntimeTelemetry telemetry{};
    bool device_lost = false;
    mutable std::mutex mutex;

    void clear_retained_buffers() noexcept {
        retained_source_device.reset();
        retained_source_staging.reset();
        retained_plan_device.reset();
        retained_plan_staging.reset();
        retained_workspace.reset();
        retained_partial.reset();
        retained_readback.reset();
    }
};

VulkanBackendProbe probe_vulkan_backend(std::wstring_view loader_path,
                                        bool enable_validation) {
    VulkanBackendProbe probe;
    try {
        vulkan_detail::VulkanLoader loader(loader_path);
        probe.loader_available = true;
    } catch (const std::exception &error) {
        probe.unavailable_reason = error.what();
        return probe;
    }
    try {
        InstanceContext context(loader_path, enable_validation);
        probe.instance_available = true;
        const auto candidates = enumerate_candidates(context);
        probe.devices.reserve(candidates.size());
        bool has_production_device = false;
        for (const PhysicalCandidate &candidate : candidates) {
            has_production_device = has_production_device
                || candidate.info.production_compute_supported;
            probe.devices.push_back(candidate.info);
        }
        if (!has_production_device) {
            probe.unavailable_reason = candidates.empty()
                ? "no Vulkan physical device is available"
                : "no Vulkan device meets the production compute limits";
        }
    } catch (const std::exception &error) {
        probe.unavailable_reason = error.what();
    }
    return probe;
}

bool vulkan_backend_available() noexcept {
    try {
        const VulkanBackendProbe probe = probe_vulkan_backend();
        return std::ranges::any_of(probe.devices,
            [](const VulkanDeviceInfo &device) {
                return device.production_compute_supported;
            });
    } catch (...) {
        return false;
    }
}

VulkanAnalysisEngine::VulkanAnalysisEngine(VulkanAnalysisOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

VulkanAnalysisEngine::~VulkanAnalysisEngine() = default;
VulkanAnalysisEngine::VulkanAnalysisEngine(VulkanAnalysisEngine &&) noexcept = default;
VulkanAnalysisEngine &VulkanAnalysisEngine::operator=(
    VulkanAnalysisEngine &&) noexcept = default;

const VulkanDeviceInfo &VulkanAnalysisEngine::device_info() const noexcept {
    return impl_->info;
}

const VulkanAnalysisOptions &VulkanAnalysisEngine::options() const noexcept {
    return impl_->options;
}

std::size_t VulkanAnalysisEngine::peak_workspace_elements() const noexcept {
    return impl_->peak_workspace_elements;
}

std::size_t VulkanAnalysisEngine::peak_working_set_bytes() const noexcept {
    return impl_->peak_working_set_bytes;
}

VulkanRuntimeTelemetry VulkanAnalysisEngine::runtime_telemetry() const {
    const std::scoped_lock lock(impl_->mutex);
    VulkanRuntimeTelemetry result = impl_->telemetry;
    result.working_buffer_retained_bytes = impl_->retained_bytes();
    result.validation_error_count =
        impl_->context.validation.errors.load(std::memory_order_relaxed);
    result.validation_warning_count =
        impl_->context.validation.warnings.load(std::memory_order_relaxed);
    return result;
}

void VulkanAnalysisEngine::reset_analysis_telemetry() {
    const std::scoped_lock lock(impl_->mutex);
    VulkanRuntimeTelemetry reset;
    reset.pipeline_creation_ms = impl_->telemetry.pipeline_creation_ms;
    reset.created_pipeline_names = impl_->telemetry.created_pipeline_names;
    reset.gpu_timestamps_available = impl_->telemetry.gpu_timestamps_available;
    reset.working_buffer_retained_bytes = impl_->retained_bytes();
    reset.working_buffer_peak_retained_bytes = reset.working_buffer_retained_bytes;
    impl_->telemetry = std::move(reset);
    impl_->context.validation.errors.store(0, std::memory_order_relaxed);
    impl_->context.validation.warnings.store(0, std::memory_order_relaxed);
}

void VulkanAnalysisEngine::trim_working_buffers() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->clear_retained_buffers();
    impl_->telemetry.working_buffer_active_bytes = 0;
    impl_->telemetry.working_buffer_retained_bytes = 0;
}

std::vector<CandidateResult> VulkanAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->analyze(source, candidates, metric, stop);
}

} // namespace getnative

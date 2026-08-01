#include "vulkan_loader.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace getnative::vulkan_detail {
namespace {

template <class Function>
[[nodiscard]] Function require_instance_function(PFN_vkGetInstanceProcAddr get_proc,
                                                 VkInstance instance,
                                                 const char *name) {
    const PFN_vkVoidFunction function = get_proc(instance, name);
    if (function == nullptr) {
        throw std::runtime_error(std::string{"Vulkan loader is missing "} + name);
    }
    return reinterpret_cast<Function>(function);
}

template <class Function>
[[nodiscard]] Function optional_instance_function(PFN_vkGetInstanceProcAddr get_proc,
                                                  VkInstance instance,
                                                  const char *name) noexcept {
    return reinterpret_cast<Function>(get_proc(instance, name));
}

template <class Function>
[[nodiscard]] Function require_device_function(PFN_vkGetDeviceProcAddr get_proc,
                                               VkDevice device,
                                               const char *name) {
    const PFN_vkVoidFunction function = get_proc(device, name);
    if (function == nullptr) {
        throw std::runtime_error(std::string{"Vulkan device is missing "} + name);
    }
    return reinterpret_cast<Function>(function);
}

} // namespace

VulkanLoader::VulkanLoader(std::wstring_view path) {
#if defined(_WIN32)
    const std::wstring terminated_path{path};
    HMODULE module = LoadLibraryW(terminated_path.c_str());
    if (module == nullptr) {
        throw std::runtime_error("Vulkan loader is unavailable: LoadLibraryW failed");
    }
    module_ = module;
    get_instance_proc_addr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(module, "vkGetInstanceProcAddr"));
    if (get_instance_proc_addr_ == nullptr) {
        FreeLibrary(module);
        module_ = nullptr;
        throw std::runtime_error("Vulkan loader is missing vkGetInstanceProcAddr");
    }
#else
    (void)path;
    throw std::runtime_error("Vulkan backend requires Windows vulkan-1.dll");
#endif

    global_.create_instance = require_instance_function<PFN_vkCreateInstance>(
        get_instance_proc_addr_, VK_NULL_HANDLE, "vkCreateInstance");
    global_.enumerate_instance_version =
        optional_instance_function<PFN_vkEnumerateInstanceVersion>(
            get_instance_proc_addr_, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    global_.enumerate_instance_layer_properties =
        require_instance_function<PFN_vkEnumerateInstanceLayerProperties>(
            get_instance_proc_addr_, VK_NULL_HANDLE,
            "vkEnumerateInstanceLayerProperties");
    global_.enumerate_instance_extension_properties =
        require_instance_function<PFN_vkEnumerateInstanceExtensionProperties>(
            get_instance_proc_addr_, VK_NULL_HANDLE,
            "vkEnumerateInstanceExtensionProperties");
}

VulkanLoader::~VulkanLoader() {
#if defined(_WIN32)
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
#endif
}

PFN_vkVoidFunction VulkanLoader::instance_proc(VkInstance instance,
                                               const char *name) const noexcept {
    return get_instance_proc_addr_(instance, name);
}

InstanceDispatch VulkanLoader::load_instance(VkInstance instance) const {
    InstanceDispatch dispatch;
#define GETNATIVE_LOAD_INSTANCE(member, type, name) \
    dispatch.member = require_instance_function<type>(get_instance_proc_addr_, instance, name)
    GETNATIVE_LOAD_INSTANCE(destroy_instance, PFN_vkDestroyInstance, "vkDestroyInstance");
    GETNATIVE_LOAD_INSTANCE(enumerate_physical_devices, PFN_vkEnumeratePhysicalDevices,
                            "vkEnumeratePhysicalDevices");
    GETNATIVE_LOAD_INSTANCE(get_physical_device_properties,
                            PFN_vkGetPhysicalDeviceProperties,
                            "vkGetPhysicalDeviceProperties");
    GETNATIVE_LOAD_INSTANCE(get_physical_device_properties2,
                            PFN_vkGetPhysicalDeviceProperties2,
                            "vkGetPhysicalDeviceProperties2");
    GETNATIVE_LOAD_INSTANCE(get_queue_family_properties,
                            PFN_vkGetPhysicalDeviceQueueFamilyProperties,
                            "vkGetPhysicalDeviceQueueFamilyProperties");
    GETNATIVE_LOAD_INSTANCE(get_memory_properties,
                            PFN_vkGetPhysicalDeviceMemoryProperties,
                            "vkGetPhysicalDeviceMemoryProperties");
    GETNATIVE_LOAD_INSTANCE(enumerate_device_extension_properties,
                            PFN_vkEnumerateDeviceExtensionProperties,
                            "vkEnumerateDeviceExtensionProperties");
    GETNATIVE_LOAD_INSTANCE(create_device, PFN_vkCreateDevice, "vkCreateDevice");
    GETNATIVE_LOAD_INSTANCE(get_device_proc_addr, PFN_vkGetDeviceProcAddr,
                            "vkGetDeviceProcAddr");
#undef GETNATIVE_LOAD_INSTANCE
    dispatch.create_debug_messenger =
        optional_instance_function<PFN_vkCreateDebugUtilsMessengerEXT>(
            get_instance_proc_addr_, instance, "vkCreateDebugUtilsMessengerEXT");
    dispatch.destroy_debug_messenger =
        optional_instance_function<PFN_vkDestroyDebugUtilsMessengerEXT>(
            get_instance_proc_addr_, instance, "vkDestroyDebugUtilsMessengerEXT");
    return dispatch;
}

DeviceDispatch VulkanLoader::load_device(VkDevice device,
                                         PFN_vkGetDeviceProcAddr get_proc) {
    if (get_proc == nullptr) {
        throw std::runtime_error("Vulkan instance is missing vkGetDeviceProcAddr");
    }
    DeviceDispatch dispatch;
#define GETNATIVE_LOAD_DEVICE(member, type, name) \
    dispatch.member = require_device_function<type>(get_proc, device, name)
    GETNATIVE_LOAD_DEVICE(destroy_device, PFN_vkDestroyDevice, "vkDestroyDevice");
    GETNATIVE_LOAD_DEVICE(get_device_queue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
    GETNATIVE_LOAD_DEVICE(create_buffer, PFN_vkCreateBuffer, "vkCreateBuffer");
    GETNATIVE_LOAD_DEVICE(destroy_buffer, PFN_vkDestroyBuffer, "vkDestroyBuffer");
    GETNATIVE_LOAD_DEVICE(get_buffer_memory_requirements,
                          PFN_vkGetBufferMemoryRequirements,
                          "vkGetBufferMemoryRequirements");
    GETNATIVE_LOAD_DEVICE(allocate_memory, PFN_vkAllocateMemory, "vkAllocateMemory");
    GETNATIVE_LOAD_DEVICE(free_memory, PFN_vkFreeMemory, "vkFreeMemory");
    GETNATIVE_LOAD_DEVICE(bind_buffer_memory, PFN_vkBindBufferMemory,
                          "vkBindBufferMemory");
    GETNATIVE_LOAD_DEVICE(map_memory, PFN_vkMapMemory, "vkMapMemory");
    GETNATIVE_LOAD_DEVICE(unmap_memory, PFN_vkUnmapMemory, "vkUnmapMemory");
    GETNATIVE_LOAD_DEVICE(flush_mapped_memory_ranges,
                          PFN_vkFlushMappedMemoryRanges,
                          "vkFlushMappedMemoryRanges");
    GETNATIVE_LOAD_DEVICE(invalidate_mapped_memory_ranges,
                          PFN_vkInvalidateMappedMemoryRanges,
                          "vkInvalidateMappedMemoryRanges");
    GETNATIVE_LOAD_DEVICE(create_descriptor_set_layout,
                          PFN_vkCreateDescriptorSetLayout,
                          "vkCreateDescriptorSetLayout");
    GETNATIVE_LOAD_DEVICE(destroy_descriptor_set_layout,
                          PFN_vkDestroyDescriptorSetLayout,
                          "vkDestroyDescriptorSetLayout");
    GETNATIVE_LOAD_DEVICE(create_descriptor_pool, PFN_vkCreateDescriptorPool,
                          "vkCreateDescriptorPool");
    GETNATIVE_LOAD_DEVICE(destroy_descriptor_pool, PFN_vkDestroyDescriptorPool,
                          "vkDestroyDescriptorPool");
    GETNATIVE_LOAD_DEVICE(allocate_descriptor_sets, PFN_vkAllocateDescriptorSets,
                          "vkAllocateDescriptorSets");
    GETNATIVE_LOAD_DEVICE(update_descriptor_sets, PFN_vkUpdateDescriptorSets,
                          "vkUpdateDescriptorSets");
    GETNATIVE_LOAD_DEVICE(create_shader_module, PFN_vkCreateShaderModule,
                          "vkCreateShaderModule");
    GETNATIVE_LOAD_DEVICE(destroy_shader_module, PFN_vkDestroyShaderModule,
                          "vkDestroyShaderModule");
    GETNATIVE_LOAD_DEVICE(create_pipeline_layout, PFN_vkCreatePipelineLayout,
                          "vkCreatePipelineLayout");
    GETNATIVE_LOAD_DEVICE(destroy_pipeline_layout, PFN_vkDestroyPipelineLayout,
                          "vkDestroyPipelineLayout");
    GETNATIVE_LOAD_DEVICE(create_pipeline_cache, PFN_vkCreatePipelineCache,
                          "vkCreatePipelineCache");
    GETNATIVE_LOAD_DEVICE(destroy_pipeline_cache, PFN_vkDestroyPipelineCache,
                          "vkDestroyPipelineCache");
    GETNATIVE_LOAD_DEVICE(get_pipeline_cache_data, PFN_vkGetPipelineCacheData,
                          "vkGetPipelineCacheData");
    GETNATIVE_LOAD_DEVICE(create_compute_pipelines, PFN_vkCreateComputePipelines,
                          "vkCreateComputePipelines");
    GETNATIVE_LOAD_DEVICE(destroy_pipeline, PFN_vkDestroyPipeline, "vkDestroyPipeline");
    GETNATIVE_LOAD_DEVICE(create_command_pool, PFN_vkCreateCommandPool,
                          "vkCreateCommandPool");
    GETNATIVE_LOAD_DEVICE(destroy_command_pool, PFN_vkDestroyCommandPool,
                          "vkDestroyCommandPool");
    GETNATIVE_LOAD_DEVICE(allocate_command_buffers, PFN_vkAllocateCommandBuffers,
                          "vkAllocateCommandBuffers");
    GETNATIVE_LOAD_DEVICE(reset_command_buffer, PFN_vkResetCommandBuffer,
                          "vkResetCommandBuffer");
    GETNATIVE_LOAD_DEVICE(begin_command_buffer, PFN_vkBeginCommandBuffer,
                          "vkBeginCommandBuffer");
    GETNATIVE_LOAD_DEVICE(end_command_buffer, PFN_vkEndCommandBuffer,
                          "vkEndCommandBuffer");
    GETNATIVE_LOAD_DEVICE(create_fence, PFN_vkCreateFence, "vkCreateFence");
    GETNATIVE_LOAD_DEVICE(destroy_fence, PFN_vkDestroyFence, "vkDestroyFence");
    GETNATIVE_LOAD_DEVICE(reset_fences, PFN_vkResetFences, "vkResetFences");
    GETNATIVE_LOAD_DEVICE(wait_for_fences, PFN_vkWaitForFences, "vkWaitForFences");
    GETNATIVE_LOAD_DEVICE(queue_submit, PFN_vkQueueSubmit, "vkQueueSubmit");
    GETNATIVE_LOAD_DEVICE(queue_wait_idle, PFN_vkQueueWaitIdle, "vkQueueWaitIdle");
    GETNATIVE_LOAD_DEVICE(device_wait_idle, PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle");
    GETNATIVE_LOAD_DEVICE(cmd_copy_buffer, PFN_vkCmdCopyBuffer, "vkCmdCopyBuffer");
    GETNATIVE_LOAD_DEVICE(cmd_pipeline_barrier, PFN_vkCmdPipelineBarrier,
                          "vkCmdPipelineBarrier");
    GETNATIVE_LOAD_DEVICE(cmd_bind_pipeline, PFN_vkCmdBindPipeline,
                          "vkCmdBindPipeline");
    GETNATIVE_LOAD_DEVICE(cmd_bind_descriptor_sets, PFN_vkCmdBindDescriptorSets,
                          "vkCmdBindDescriptorSets");
    GETNATIVE_LOAD_DEVICE(cmd_push_constants, PFN_vkCmdPushConstants,
                          "vkCmdPushConstants");
    GETNATIVE_LOAD_DEVICE(cmd_dispatch, PFN_vkCmdDispatch, "vkCmdDispatch");
    GETNATIVE_LOAD_DEVICE(create_query_pool, PFN_vkCreateQueryPool,
                          "vkCreateQueryPool");
    GETNATIVE_LOAD_DEVICE(destroy_query_pool, PFN_vkDestroyQueryPool,
                          "vkDestroyQueryPool");
    GETNATIVE_LOAD_DEVICE(cmd_reset_query_pool, PFN_vkCmdResetQueryPool,
                          "vkCmdResetQueryPool");
    GETNATIVE_LOAD_DEVICE(cmd_write_timestamp, PFN_vkCmdWriteTimestamp,
                          "vkCmdWriteTimestamp");
    GETNATIVE_LOAD_DEVICE(get_query_pool_results, PFN_vkGetQueryPoolResults,
                          "vkGetQueryPoolResults");
#undef GETNATIVE_LOAD_DEVICE
    return dispatch;
}

NonCoherentRange aligned_noncoherent_range(VkDeviceSize offset, VkDeviceSize size,
                                           VkDeviceSize atom_size,
                                           VkDeviceSize allocation_size) {
    if (size == 0 || atom_size == 0 || offset > allocation_size
        || size > allocation_size - offset) {
        throw std::invalid_argument("invalid Vulkan non-coherent memory range");
    }
    const VkDeviceSize aligned_offset = offset - offset % atom_size;
    const VkDeviceSize end = offset + size;
    VkDeviceSize aligned_end = allocation_size;
    if (end < allocation_size) {
        const VkDeviceSize remainder = end % atom_size;
        if (remainder == 0) {
            aligned_end = end;
        } else {
            const VkDeviceSize padding = atom_size - remainder;
            if (padding > std::numeric_limits<VkDeviceSize>::max() - end) {
                throw std::length_error("Vulkan non-coherent range alignment overflow");
            }
            aligned_end = std::min(end + padding, allocation_size);
        }
    }
    return {aligned_offset, aligned_end - aligned_offset};
}

const char *vk_result_name(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VK_ERROR_UNRECOGNIZED";
    }
}

void check_vk(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string{operation} + " failed with "
                                 + vk_result_name(result));
    }
}

} // namespace getnative::vulkan_detail

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <string>
#include <string_view>

namespace getnative::vulkan_detail {

struct GlobalDispatch {
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;
    PFN_vkEnumerateInstanceLayerProperties enumerate_instance_layer_properties = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extension_properties = nullptr;
};

struct InstanceDispatch {
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger = nullptr;
};

struct DeviceDispatch {
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = nullptr;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout = nullptr;
    PFN_vkCreateDescriptorPool create_descriptor_pool = nullptr;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool = nullptr;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets = nullptr;
    PFN_vkUpdateDescriptorSets update_descriptor_sets = nullptr;
    PFN_vkCreateShaderModule create_shader_module = nullptr;
    PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkCreatePipelineCache create_pipeline_cache = nullptr;
    PFN_vkDestroyPipelineCache destroy_pipeline_cache = nullptr;
    PFN_vkGetPipelineCacheData get_pipeline_cache_data = nullptr;
    PFN_vkCreateComputePipelines create_compute_pipelines = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkResetCommandBuffer reset_command_buffer = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkCmdCopyBuffer cmd_copy_buffer = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = nullptr;
    PFN_vkCmdPushConstants cmd_push_constants = nullptr;
    PFN_vkCmdDispatch cmd_dispatch = nullptr;
    PFN_vkCreateQueryPool create_query_pool = nullptr;
    PFN_vkDestroyQueryPool destroy_query_pool = nullptr;
    PFN_vkCmdResetQueryPool cmd_reset_query_pool = nullptr;
    PFN_vkCmdWriteTimestamp cmd_write_timestamp = nullptr;
    PFN_vkGetQueryPoolResults get_query_pool_results = nullptr;
};

class VulkanLoader {
public:
    explicit VulkanLoader(std::wstring_view path);
    ~VulkanLoader();

    VulkanLoader(const VulkanLoader &) = delete;
    VulkanLoader &operator=(const VulkanLoader &) = delete;
    VulkanLoader(VulkanLoader &&) = delete;
    VulkanLoader &operator=(VulkanLoader &&) = delete;

    [[nodiscard]] const GlobalDispatch &global() const noexcept { return global_; }
    [[nodiscard]] PFN_vkVoidFunction instance_proc(VkInstance instance,
                                                   const char *name) const noexcept;
    [[nodiscard]] InstanceDispatch load_instance(VkInstance instance) const;
    [[nodiscard]] static DeviceDispatch load_device(
        VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr);

private:
    void *module_ = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
    GlobalDispatch global_{};
};

struct NonCoherentRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

[[nodiscard]] NonCoherentRange aligned_noncoherent_range(
    VkDeviceSize offset, VkDeviceSize size, VkDeviceSize atom_size,
    VkDeviceSize allocation_size);
[[nodiscard]] const char *vk_result_name(VkResult result) noexcept;
void check_vk(VkResult result, std::string_view operation);

} // namespace getnative::vulkan_detail

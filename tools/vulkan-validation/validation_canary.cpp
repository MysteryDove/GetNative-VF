// Positive control: intentionally trigger one core and one synchronization error.
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
std::atomic<int> core_errors{0}, sync_errors{0};
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (!(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) return VK_FALSE;
    const char *id = data->pMessageIdName ? data->pMessageIdName : "";
    std::fprintf(stderr, "%s\n", id);
    if (std::strstr(id, "SYNC-HAZARD-WRITE-AFTER-WRITE")) ++sync_errors;
    if (std::strstr(id, "VUID-VkFenceCreateInfo-flags-parameter")) ++core_errors;
    return VK_TRUE;
}
void check(VkResult result) {
    if (result != VK_SUCCESS) { std::fprintf(stderr, "Vulkan error %d\n", result); std::exit(2); }
}
}

int main() {
    const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = &extension;
    VkInstance instance;
    check(vkCreateInstance(&instance_info, nullptr, &instance));
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debug.pfnUserCallback = message;
    VkDebugUtilsMessengerEXT messenger;
    check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT"))(instance, &debug, nullptr, &messenger));
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count) return 77;
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    auto physical = devices.front();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device;
    check(vkCreateDevice(physical, &device_info, nullptr, &device));
    VkFenceCreateInfo invalid_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    invalid_fence.flags = 0x40000000U;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &invalid_fence, nullptr, &fence) == VK_SUCCESS) {
        vkDestroyFence(device, fence, nullptr);
    }
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = 32;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer;
    check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    while (!(requirements.memoryTypeBits & (1U << allocation.memoryTypeIndex))) ++allocation.memoryTypeIndex;
    VkDeviceMemory memory;
    check(vkAllocateMemory(device, &allocation, nullptr, &memory));
    check(vkBindBufferMemory(device, buffer, memory, 0));
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool;
    check(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.commandBufferCount = 1;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer command;
    check(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin));
    vkCmdFillBuffer(command, buffer, 0, 32, 0);
    vkCmdFillBuffer(command, buffer, 0, 32, 1);
    check(vkEndCommandBuffer(command));
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf("core_errors=%d sync_errors=%d\n", core_errors.load(), sync_errors.load());
    return core_errors > 0 && sync_errors > 0 ? 0 : 1;
}

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#include <vulkan/vulkan.h>

#define GETNATIVE_VK_GLOBAL(name) extern PFN_##name name;
#define GETNATIVE_VK_GLOBAL_OPTIONAL(name) extern PFN_##name name;
#define GETNATIVE_VK_INSTANCE(name) extern PFN_##name name;
#define GETNATIVE_VK_DEVICE(name) extern PFN_##name name;
#include "vulkan_function_list.hpp"
#undef GETNATIVE_VK_DEVICE
#undef GETNATIVE_VK_INSTANCE
#undef GETNATIVE_VK_GLOBAL_OPTIONAL
#undef GETNATIVE_VK_GLOBAL

namespace getnative::vulkan_detail {

void ensure_vulkan_loader();
void load_vulkan_instance_functions(VkInstance instance);
void load_vulkan_device_functions(VkInstance instance, VkDevice device);

} // namespace getnative::vulkan_detail

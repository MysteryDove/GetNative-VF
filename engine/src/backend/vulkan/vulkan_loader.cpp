#include "vulkan_loader.hpp"

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define GETNATIVE_VK_GLOBAL(name) PFN_##name name = nullptr;
#define GETNATIVE_VK_GLOBAL_OPTIONAL(name) PFN_##name name = nullptr;
#define GETNATIVE_VK_INSTANCE(name) PFN_##name name = nullptr;
#define GETNATIVE_VK_DEVICE(name) PFN_##name name = nullptr;
#include "vulkan_function_list.hpp"
#undef GETNATIVE_VK_DEVICE
#undef GETNATIVE_VK_INSTANCE
#undef GETNATIVE_VK_GLOBAL_OPTIONAL
#undef GETNATIVE_VK_GLOBAL

namespace getnative::vulkan_detail {
namespace {

class LoaderLibrary {
public:
    LoaderLibrary() {
#ifdef _WIN32
        handle_ = LoadLibraryW(L"vulkan-1.dll");
        if (handle_ == nullptr) {
            throw std::runtime_error(
                "Vulkan loader is unavailable: LoadLibraryW(vulkan-1.dll) failed ("
                + std::to_string(GetLastError()) + ")");
        }
#else
        handle_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            const char *error = dlerror();
            throw std::runtime_error(
                std::string{"Vulkan loader is unavailable: "}
                + (error != nullptr ? error : "dlopen(libvulkan.so.1) failed"));
        }
#endif
    }

    template <class Function>
    [[nodiscard]] Function symbol(const char *name) const noexcept {
#ifdef _WIN32
        const auto raw = GetProcAddress(handle_, name);
#else
        void *raw = dlsym(handle_, name);
#endif
        Function result = nullptr;
        static_assert(sizeof(result) == sizeof(raw));
        std::memcpy(&result, &raw, sizeof(result));
        return result;
    }

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void *handle_ = nullptr;
#endif
};

[[nodiscard]] LoaderLibrary &library() {
    // Keep the loader resident for process lifetime so Vulkan function pointers
    // remain valid through static teardown in embedding applications.
    static LoaderLibrary *value = new LoaderLibrary();
    return *value;
}

template <class Function>
void require_function(Function &destination, Function resolved, const char *name) {
    if (resolved == nullptr) {
        throw std::runtime_error(
            std::string{"Vulkan loader is missing required function "} + name);
    }
    destination = resolved;
}

std::once_flag global_once;
std::once_flag instance_once;
std::once_flag device_once;

} // namespace

void ensure_vulkan_loader() {
    std::call_once(global_once, [] {
        require_function(
            vkGetInstanceProcAddr,
            library().symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"),
            "vkGetInstanceProcAddr");
#define GETNATIVE_VK_GLOBAL(name)                                             \
        if constexpr (!std::is_same_v<PFN_##name, PFN_vkGetInstanceProcAddr>) { \
            require_function(                                                \
                name, reinterpret_cast<PFN_##name>(                          \
                    vkGetInstanceProcAddr(nullptr, #name)), #name);          \
        }
#define GETNATIVE_VK_GLOBAL_OPTIONAL(name)                                    \
        name = reinterpret_cast<PFN_##name>(                                  \
            vkGetInstanceProcAddr(nullptr, #name));
#define GETNATIVE_VK_INSTANCE(name)
#define GETNATIVE_VK_DEVICE(name)
#include "vulkan_function_list.hpp"
#undef GETNATIVE_VK_DEVICE
#undef GETNATIVE_VK_INSTANCE
#undef GETNATIVE_VK_GLOBAL_OPTIONAL
#undef GETNATIVE_VK_GLOBAL
    });
}

void load_vulkan_instance_functions(VkInstance instance) {
    ensure_vulkan_loader();
    std::call_once(instance_once, [instance] {
#define GETNATIVE_VK_GLOBAL(name)
#define GETNATIVE_VK_GLOBAL_OPTIONAL(name)
#define GETNATIVE_VK_INSTANCE(name)                                           \
    require_function(                                                         \
        name, reinterpret_cast<PFN_##name>(                                   \
            vkGetInstanceProcAddr(instance, #name)), #name);
#define GETNATIVE_VK_DEVICE(name)
#include "vulkan_function_list.hpp"
#undef GETNATIVE_VK_DEVICE
#undef GETNATIVE_VK_INSTANCE
#undef GETNATIVE_VK_GLOBAL_OPTIONAL
#undef GETNATIVE_VK_GLOBAL
    });
}

void load_vulkan_device_functions(VkInstance instance, VkDevice device) {
    if (vkGetDeviceProcAddr == nullptr) {
        throw std::logic_error(
            "Vulkan instance functions must be loaded before device functions");
    }
    std::call_once(device_once, [instance, device] {
        const auto resolve = [instance, device](const char *name) {
            const PFN_vkVoidFunction device_function =
                vkGetDeviceProcAddr(device, name);
            if (device_function == nullptr) return PFN_vkVoidFunction{};
            return vkGetInstanceProcAddr(instance, name);
        };
        require_function(
            vkDestroyDevice,
            reinterpret_cast<PFN_vkDestroyDevice>(resolve("vkDestroyDevice")),
            "vkDestroyDevice");
#define GETNATIVE_VK_GLOBAL(name)
#define GETNATIVE_VK_GLOBAL_OPTIONAL(name)
#define GETNATIVE_VK_INSTANCE(name)
#define GETNATIVE_VK_DEVICE(name)                                             \
    require_function(                                                         \
        name, reinterpret_cast<PFN_##name>(                                   \
            resolve(#name)), #name);
#include "vulkan_function_list.hpp"
#undef GETNATIVE_VK_DEVICE
#undef GETNATIVE_VK_INSTANCE
#undef GETNATIVE_VK_GLOBAL_OPTIONAL
#undef GETNATIVE_VK_GLOBAL
    });
}

} // namespace getnative::vulkan_detail

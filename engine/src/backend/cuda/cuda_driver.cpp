#include "cuda_driver.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#else
#error "The CUDA driver loader requires Windows or Linux"
#endif

#include <sstream>
#include <stdexcept>
#include <string>

namespace getnative::cuda_detail {
namespace {

template <class Function>
[[nodiscard]] Function required_symbol(void *library, const char *name) {
#if defined(_WIN32)
    const HMODULE module = static_cast<HMODULE>(library);
    const FARPROC address = GetProcAddress(module, name);
    if (address == nullptr) {
        throw std::runtime_error("CUDA Driver API is missing required symbol "
                                 + std::string{name});
    }
    return reinterpret_cast<Function>(address);
#else
    (void)dlerror();
    void *address = dlsym(library, name);
    const char *error = dlerror();
    if (error != nullptr || address == nullptr) {
        throw std::runtime_error("CUDA Driver API is missing required symbol "
                                 + std::string{name} + ": "
                                 + (error == nullptr ? "unknown error" : error));
    }
    return reinterpret_cast<Function>(address);
#endif
}

} // namespace

DriverApi::~DriverApi() {
#if defined(_WIN32)
    if (library != nullptr) FreeLibrary(static_cast<HMODULE>(library));
#else
    if (library != nullptr) (void)dlclose(library);
#endif
}

std::shared_ptr<DriverApi> load_cuda_driver() {
#if defined(_WIN32)
    HMODULE library = LoadLibraryW(L"nvcuda.dll");
    if (library == nullptr) {
        throw std::runtime_error(
            "CUDA driver DLL nvcuda.dll is unavailable (Windows error "
            + std::to_string(GetLastError()) + ")");
    }
#else
    void *library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char *error = dlerror();
        throw std::runtime_error(
            "CUDA driver library libcuda.so.1 is unavailable: "
            + std::string{error == nullptr ? "unknown error" : error});
    }
#endif

    auto api = std::shared_ptr<DriverApi>(new DriverApi{});
    api->library = static_cast<void *>(library);
    api->init = required_symbol<DriverApi::Init>(library, "cuInit");
    api->driver_get_version = required_symbol<DriverApi::DriverGetVersion>(
        library, "cuDriverGetVersion");
    api->device_get_count = required_symbol<DriverApi::DeviceGetCount>(
        library, "cuDeviceGetCount");
    api->device_get = required_symbol<DriverApi::DeviceGet>(library, "cuDeviceGet");
    api->device_get_name = required_symbol<DriverApi::DeviceGetName>(
        library, "cuDeviceGetName");
    api->device_get_uuid = required_symbol<DriverApi::DeviceGetUuid>(
        library, "cuDeviceGetUuid_v2");
    api->device_get_attribute = required_symbol<DriverApi::DeviceGetAttribute>(
        library, "cuDeviceGetAttribute");
    api->device_total_mem = required_symbol<DriverApi::DeviceTotalMem>(
        library, "cuDeviceTotalMem_v2");
    api->ctx_create = required_symbol<DriverApi::CtxCreate>(library, "cuCtxCreate_v2");
    api->ctx_destroy = required_symbol<DriverApi::CtxDestroy>(library, "cuCtxDestroy_v2");
    api->ctx_get_current = required_symbol<DriverApi::CtxGetCurrent>(
        library, "cuCtxGetCurrent");
    api->ctx_set_current = required_symbol<DriverApi::CtxSetCurrent>(
        library, "cuCtxSetCurrent");
    api->ctx_synchronize = required_symbol<DriverApi::CtxSynchronize>(
        library, "cuCtxSynchronize");
    api->module_load_data = required_symbol<DriverApi::ModuleLoadData>(
        library, "cuModuleLoadData");
    api->module_unload = required_symbol<DriverApi::ModuleUnload>(
        library, "cuModuleUnload");
    api->module_get_function = required_symbol<DriverApi::ModuleGetFunction>(
        library, "cuModuleGetFunction");
    api->func_get_attribute = required_symbol<DriverApi::FuncGetAttribute>(
        library, "cuFuncGetAttribute");
    api->mem_get_info = required_symbol<DriverApi::MemGetInfo>(
        library, "cuMemGetInfo_v2");
    api->pointer_get_attribute = required_symbol<DriverApi::PointerGetAttribute>(
        library, "cuPointerGetAttribute");
    api->mem_alloc = required_symbol<DriverApi::MemAlloc>(library, "cuMemAlloc_v2");
    api->mem_free = required_symbol<DriverApi::MemFree>(library, "cuMemFree_v2");
    api->memcpy_htod = required_symbol<DriverApi::MemcpyHtoD>(
        library, "cuMemcpyHtoD_v2");
    api->memcpy_dtoh = required_symbol<DriverApi::MemcpyDtoH>(
        library, "cuMemcpyDtoH_v2");
    api->memcpy_htod_async = required_symbol<DriverApi::MemcpyHtoDAsync>(
        library, "cuMemcpyHtoDAsync_v2");
    api->memcpy_dtoh_async = required_symbol<DriverApi::MemcpyDtoHAsync>(
        library, "cuMemcpyDtoHAsync_v2");
    api->mem_host_alloc = required_symbol<DriverApi::MemHostAlloc>(
        library, "cuMemHostAlloc");
    api->mem_free_host = required_symbol<DriverApi::MemFreeHost>(
        library, "cuMemFreeHost");
    api->stream_create = required_symbol<DriverApi::StreamCreate>(
        library, "cuStreamCreate");
    api->stream_destroy = required_symbol<DriverApi::StreamDestroy>(
        library, "cuStreamDestroy_v2");
    api->stream_synchronize = required_symbol<DriverApi::StreamSynchronize>(
        library, "cuStreamSynchronize");
    api->stream_wait_event = required_symbol<DriverApi::StreamWaitEvent>(
        library, "cuStreamWaitEvent");
    api->event_create = required_symbol<DriverApi::EventCreate>(
        library, "cuEventCreate");
    api->event_destroy = required_symbol<DriverApi::EventDestroy>(
        library, "cuEventDestroy_v2");
    api->event_record = required_symbol<DriverApi::EventRecord>(
        library, "cuEventRecord");
    api->event_elapsed_time = required_symbol<DriverApi::EventElapsedTime>(
        library, "cuEventElapsedTime");
    api->launch_kernel = required_symbol<DriverApi::LaunchKernel>(
        library, "cuLaunchKernel");
    api->get_error_name = required_symbol<DriverApi::GetErrorName>(
        library, "cuGetErrorName");
    api->get_error_string = required_symbol<DriverApi::GetErrorString>(
        library, "cuGetErrorString");
    return api;
}

std::string cuda_error(const DriverApi &api, CUresult result) {
    const char *name = nullptr;
    const char *description = nullptr;
    (void)api.get_error_name(result, &name);
    (void)api.get_error_string(result, &description);
    std::ostringstream stream;
    stream << "CUDA error " << static_cast<int>(result);
    if (name != nullptr) stream << " (" << name << ')';
    if (description != nullptr) stream << ": " << description;
    return stream.str();
}

void cuda_check(const DriverApi &api, CUresult result, const char *operation) {
    if (result != CUDA_SUCCESS) {
        throw std::runtime_error(std::string{operation} + " failed: "
                                 + cuda_error(api, result));
    }
}

} // namespace getnative::cuda_detail

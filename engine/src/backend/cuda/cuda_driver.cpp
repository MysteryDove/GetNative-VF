#include "cuda_driver.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <sstream>
#include <stdexcept>
#include <string>

namespace getnative::cuda_detail {
namespace {

template <class Function>
[[nodiscard]] Function resolve_required(HMODULE library, const char *name) {
    const FARPROC address = GetProcAddress(library, name);
    if (address == nullptr) {
        throw std::runtime_error("CUDA Driver API is missing required symbol "
                                 + std::string{name});
    }
    return reinterpret_cast<Function>(address);
}

} // namespace

DriverApi::~DriverApi() {
    if (library != nullptr) {
        FreeLibrary(static_cast<HMODULE>(library));
    }
}

std::shared_ptr<DriverApi> load_cuda_driver() {
    HMODULE library = LoadLibraryW(L"nvcuda.dll");
    if (library == nullptr) {
        const DWORD code = GetLastError();
        throw std::runtime_error(
            "CUDA driver DLL nvcuda.dll is unavailable (Windows error "
            + std::to_string(code) + ")");
    }

    auto api = std::shared_ptr<DriverApi>(new DriverApi{});
    api->library = library;
    api->init = resolve_required<DriverApi::Init>(library, "cuInit");
    api->driver_get_version = resolve_required<DriverApi::DriverGetVersion>(
        library, "cuDriverGetVersion");
    api->device_get_count = resolve_required<DriverApi::DeviceGetCount>(
        library, "cuDeviceGetCount");
    api->device_get = resolve_required<DriverApi::DeviceGet>(library, "cuDeviceGet");
    api->device_get_name = resolve_required<DriverApi::DeviceGetName>(
        library, "cuDeviceGetName");
    api->device_get_uuid = resolve_required<DriverApi::DeviceGetUuid>(
        library, "cuDeviceGetUuid_v2");
    api->device_get_attribute = resolve_required<DriverApi::DeviceGetAttribute>(
        library, "cuDeviceGetAttribute");
    api->device_total_mem = resolve_required<DriverApi::DeviceTotalMem>(
        library, "cuDeviceTotalMem_v2");
    api->ctx_create = resolve_required<DriverApi::CtxCreate>(
        library, "cuCtxCreate_v2");
    api->ctx_destroy = resolve_required<DriverApi::CtxDestroy>(
        library, "cuCtxDestroy_v2");
    api->ctx_get_current = resolve_required<DriverApi::CtxGetCurrent>(
        library, "cuCtxGetCurrent");
    api->ctx_set_current = resolve_required<DriverApi::CtxSetCurrent>(
        library, "cuCtxSetCurrent");
    api->module_load_data_ex = resolve_required<DriverApi::ModuleLoadDataEx>(
        library, "cuModuleLoadDataEx");
    api->module_unload = resolve_required<DriverApi::ModuleUnload>(
        library, "cuModuleUnload");
    api->module_get_function = resolve_required<DriverApi::ModuleGetFunction>(
        library, "cuModuleGetFunction");
    api->func_get_attribute = resolve_required<DriverApi::FuncGetAttribute>(
        library, "cuFuncGetAttribute");
    api->stream_create = resolve_required<DriverApi::StreamCreate>(
        library, "cuStreamCreate");
    api->stream_destroy = resolve_required<DriverApi::StreamDestroy>(
        library, "cuStreamDestroy_v2");
    api->stream_synchronize = resolve_required<DriverApi::StreamSynchronize>(
        library, "cuStreamSynchronize");
    api->event_create = resolve_required<DriverApi::EventCreate>(
        library, "cuEventCreate");
    api->event_record = resolve_required<DriverApi::EventRecord>(
        library, "cuEventRecord");
    api->event_elapsed_time = resolve_required<DriverApi::EventElapsedTime>(
        library, "cuEventElapsedTime_v2");
    api->event_destroy = resolve_required<DriverApi::EventDestroy>(
        library, "cuEventDestroy_v2");
    api->mem_alloc = resolve_required<DriverApi::MemAlloc>(
        library, "cuMemAlloc_v2");
    api->mem_free = resolve_required<DriverApi::MemFree>(
        library, "cuMemFree_v2");
    api->mem_host_alloc = resolve_required<DriverApi::MemHostAlloc>(
        library, "cuMemHostAlloc");
    api->mem_free_host = resolve_required<DriverApi::MemFreeHost>(
        library, "cuMemFreeHost");
    api->memcpy_htod_async = resolve_required<DriverApi::MemcpyHtoDAsync>(
        library, "cuMemcpyHtoDAsync_v2");
    api->memcpy_dtoh_async = resolve_required<DriverApi::MemcpyDtoHAsync>(
        library, "cuMemcpyDtoHAsync_v2");
    api->launch_kernel = resolve_required<DriverApi::LaunchKernel>(
        library, "cuLaunchKernel");
    api->get_error_name = resolve_required<DriverApi::GetErrorName>(
        library, "cuGetErrorName");
    api->get_error_string = resolve_required<DriverApi::GetErrorString>(
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

#pragma once

#include <cuda.h>

#include <memory>
#include <string>

namespace getnative::cuda_detail {

struct DriverApi {
    using Init = CUresult(CUDAAPI *)(unsigned int);
    using DriverGetVersion = CUresult(CUDAAPI *)(int *);
    using DeviceGetCount = CUresult(CUDAAPI *)(int *);
    using DeviceGet = CUresult(CUDAAPI *)(CUdevice *, int);
    using DeviceGetName = CUresult(CUDAAPI *)(char *, int, CUdevice);
    using DeviceGetUuid = CUresult(CUDAAPI *)(CUuuid *, CUdevice);
    using DeviceGetAttribute = CUresult(CUDAAPI *)(int *, CUdevice_attribute, CUdevice);
    using DeviceTotalMem = CUresult(CUDAAPI *)(std::size_t *, CUdevice);
    using CtxCreate = CUresult(CUDAAPI *)(CUcontext *, unsigned int, CUdevice);
    using CtxDestroy = CUresult(CUDAAPI *)(CUcontext);
    using CtxGetCurrent = CUresult(CUDAAPI *)(CUcontext *);
    using CtxSetCurrent = CUresult(CUDAAPI *)(CUcontext);
    using CtxSynchronize = CUresult(CUDAAPI *)();
    using ModuleLoadData = CUresult(CUDAAPI *)(CUmodule *, const void *);
    using ModuleUnload = CUresult(CUDAAPI *)(CUmodule);
    using ModuleGetFunction = CUresult(CUDAAPI *)(CUfunction *, CUmodule, const char *);
    using FuncGetAttribute = CUresult(CUDAAPI *)(
        int *, CUfunction_attribute, CUfunction);
    using MemGetInfo = CUresult(CUDAAPI *)(std::size_t *, std::size_t *);
    using PointerGetAttribute = CUresult(CUDAAPI *)(
        void *, CUpointer_attribute, CUdeviceptr);
    using MemAlloc = CUresult(CUDAAPI *)(CUdeviceptr *, std::size_t);
    using MemFree = CUresult(CUDAAPI *)(CUdeviceptr);
    using MemcpyHtoD = CUresult(CUDAAPI *)(CUdeviceptr, const void *, std::size_t);
    using MemcpyDtoH = CUresult(CUDAAPI *)(void *, CUdeviceptr, std::size_t);
    using MemcpyHtoDAsync = CUresult(CUDAAPI *)(
        CUdeviceptr, const void *, std::size_t, CUstream);
    using MemcpyDtoHAsync = CUresult(CUDAAPI *)(
        void *, CUdeviceptr, std::size_t, CUstream);
    using MemHostAlloc = CUresult(CUDAAPI *)(void **, std::size_t, unsigned int);
    using MemFreeHost = CUresult(CUDAAPI *)(void *);
    using StreamCreate = CUresult(CUDAAPI *)(CUstream *, unsigned int);
    using StreamDestroy = CUresult(CUDAAPI *)(CUstream);
    using StreamSynchronize = CUresult(CUDAAPI *)(CUstream);
    using StreamWaitEvent = CUresult(CUDAAPI *)(CUstream, CUevent, unsigned int);
    using EventCreate = CUresult(CUDAAPI *)(CUevent *, unsigned int);
    using EventDestroy = CUresult(CUDAAPI *)(CUevent);
    using EventRecord = CUresult(CUDAAPI *)(CUevent, CUstream);
    using EventElapsedTime = CUresult(CUDAAPI *)(float *, CUevent, CUevent);
    using LaunchKernel = CUresult(CUDAAPI *)(
        CUfunction, unsigned int, unsigned int, unsigned int,
        unsigned int, unsigned int, unsigned int, unsigned int,
        CUstream, void **, void **);
    using GetErrorName = CUresult(CUDAAPI *)(CUresult, const char **);
    using GetErrorString = CUresult(CUDAAPI *)(CUresult, const char **);

    ~DriverApi();
    DriverApi(const DriverApi &) = delete;
    DriverApi &operator=(const DriverApi &) = delete;

    void *library = nullptr;
    Init init = nullptr;
    DriverGetVersion driver_get_version = nullptr;
    DeviceGetCount device_get_count = nullptr;
    DeviceGet device_get = nullptr;
    DeviceGetName device_get_name = nullptr;
    DeviceGetUuid device_get_uuid = nullptr;
    DeviceGetAttribute device_get_attribute = nullptr;
    DeviceTotalMem device_total_mem = nullptr;
    CtxCreate ctx_create = nullptr;
    CtxDestroy ctx_destroy = nullptr;
    CtxGetCurrent ctx_get_current = nullptr;
    CtxSetCurrent ctx_set_current = nullptr;
    CtxSynchronize ctx_synchronize = nullptr;
    ModuleLoadData module_load_data = nullptr;
    ModuleUnload module_unload = nullptr;
    ModuleGetFunction module_get_function = nullptr;
    FuncGetAttribute func_get_attribute = nullptr;
    MemGetInfo mem_get_info = nullptr;
    PointerGetAttribute pointer_get_attribute = nullptr;
    MemAlloc mem_alloc = nullptr;
    MemFree mem_free = nullptr;
    MemcpyHtoD memcpy_htod = nullptr;
    MemcpyDtoH memcpy_dtoh = nullptr;
    MemcpyHtoDAsync memcpy_htod_async = nullptr;
    MemcpyDtoHAsync memcpy_dtoh_async = nullptr;
    MemHostAlloc mem_host_alloc = nullptr;
    MemFreeHost mem_free_host = nullptr;
    StreamCreate stream_create = nullptr;
    StreamDestroy stream_destroy = nullptr;
    StreamSynchronize stream_synchronize = nullptr;
    StreamWaitEvent stream_wait_event = nullptr;
    EventCreate event_create = nullptr;
    EventDestroy event_destroy = nullptr;
    EventRecord event_record = nullptr;
    EventElapsedTime event_elapsed_time = nullptr;
    LaunchKernel launch_kernel = nullptr;
    GetErrorName get_error_name = nullptr;
    GetErrorString get_error_string = nullptr;

private:
    DriverApi() = default;
    friend std::shared_ptr<DriverApi> load_cuda_driver();
};

[[nodiscard]] std::shared_ptr<DriverApi> load_cuda_driver();
[[nodiscard]] std::string cuda_error(const DriverApi &api, CUresult result);
void cuda_check(const DriverApi &api, CUresult result, const char *operation);

} // namespace getnative::cuda_detail

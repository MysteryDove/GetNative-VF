#include "getnative/cuda_analysis.hpp"

#include "cuda_driver.hpp"
#include "getnative_cuda_fatbin.hpp"
#include "../gpu/gpu_batch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace getnative {
namespace {

using cuda_detail::DriverApi;
namespace gpu = detail::gpu;
using gpu::AnalysisJob;
using gpu::AxisPlanDescriptor;
using gpu::KernelShape;
using gpu::PackedTile;
using gpu::TileRange;
using gpu::TileSignature;

constexpr std::size_t shape_count = 5U;
constexpr std::size_t jit_log_capacity = 16U * 1024U;
constexpr std::uint32_t reduction_width = 256U;

struct EmbeddedCudaArtifact {
    const unsigned char *data = nullptr;
    std::size_t size = 0U;
    std::string_view name;
    std::string_view compile_flags;
};

[[nodiscard]] EmbeddedCudaArtifact production_cuda_artifact() {
    return {
        getnative_cuda_fatbin,
        getnative_cuda_fatbin_size,
        "getnative_cuda.fatbin",
        "-O3 -lineinfo -Xptxas=-v (production path; FP32 FMA allowed)",
    };
}

[[nodiscard]] std::string fnv1a64_hex(const unsigned char *data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ data[index]) * 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

[[nodiscard]] bool environment_flag_enabled(const char *name) noexcept {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return false;
    const std::string_view text{value};
    return text != "0" && text != "false" && text != "FALSE"
        && text != "off" && text != "OFF";
}

[[nodiscard]] std::size_t checked_product(std::size_t left, std::size_t right,
                                          std::string_view name) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::length_error(std::string{name} + " size overflow");
    }
    return left * right;
}

[[nodiscard]] std::size_t checked_add(std::size_t left, std::size_t right,
                                      std::string_view name) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::length_error(std::string{name} + " size overflow");
    }
    return left + right;
}

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, std::string_view name) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string{name} + " exceeds CUDA's 32-bit range");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::size_t shape_index(KernelShape shape) noexcept {
    switch (shape) {
    case KernelShape::bandwidth3: return 0U;
    case KernelShape::bandwidth7: return 1U;
    case KernelShape::bandwidth11: return 2U;
    case KernelShape::bandwidth15: return 3U;
    case KernelShape::generic: return 4U;
    }
    return 4U;
}

[[nodiscard]] gpu::KernelDispatchPolicy dispatch_policy(CudaKernelVariant variant) {
    switch (variant) {
    case CudaKernelVariant::automatic:
        return gpu::KernelDispatchPolicy::automatic;
    case CudaKernelVariant::cpp_generic:
        return gpu::KernelDispatchPolicy::generic_only;
    case CudaKernelVariant::cpp_specialized:
        return gpu::KernelDispatchPolicy::required_specialized;
    case CudaKernelVariant::architecture_specific:
        throw std::runtime_error(
            "CUDA architecture-specific variant is not compiled and benchmark-approved");
    case CudaKernelVariant::inline_ptx:
        throw std::runtime_error("CUDA inline-ptx variant is unavailable: NO_PTX_CANDIDATE");
    }
    throw std::invalid_argument("unknown CUDA kernel variant");
}

[[nodiscard]] const char *shape_name(KernelShape shape) noexcept {
    switch (shape) {
    case KernelShape::bandwidth3: return "b3";
    case KernelShape::bandwidth7: return "b7";
    case KernelShape::bandwidth11: return "b11";
    case KernelShape::bandwidth15: return "b15";
    case KernelShape::generic: return "generic";
    }
    return "generic";
}

[[nodiscard]] std::string format_uuid(const CUuuid &uuid) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(uuid.bytes);
    std::ostringstream stream;
    stream << "GPU-" << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < sizeof(uuid.bytes); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) stream << '-';
        stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return stream.str();
}

[[nodiscard]] std::vector<CudaDeviceInfo> enumerate_devices(const DriverApi &api) {
    cuda_detail::cuda_check(api, api.init(0U), "cuInit");
    int driver_version = 0;
    cuda_detail::cuda_check(
        api, api.driver_get_version(&driver_version), "cuDriverGetVersion");
    int count = 0;
    cuda_detail::cuda_check(api, api.device_get_count(&count), "cuDeviceGetCount");
    if (count < 0) throw std::runtime_error("cuDeviceGetCount returned a negative count");

    std::vector<CudaDeviceInfo> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (int ordinal = 0; ordinal < count; ++ordinal) {
        CUdevice device = 0;
        cuda_detail::cuda_check(api, api.device_get(&device, ordinal), "cuDeviceGet");
        std::array<char, 256> name{};
        cuda_detail::cuda_check(
            api, api.device_get_name(name.data(), static_cast<int>(name.size()), device),
            "cuDeviceGetName");
        CUuuid uuid{};
        cuda_detail::cuda_check(
            api, api.device_get_uuid(&uuid, device), "cuDeviceGetUuid_v2");
        int major = 0;
        int minor = 0;
        int maximum_threads = 0;
        cuda_detail::cuda_check(
            api,
            api.device_get_attribute(
                &major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device),
            "cuDeviceGetAttribute(COMPUTE_CAPABILITY_MAJOR)");
        cuda_detail::cuda_check(
            api,
            api.device_get_attribute(
                &minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device),
            "cuDeviceGetAttribute(COMPUTE_CAPABILITY_MINOR)");
        cuda_detail::cuda_check(
            api,
            api.device_get_attribute(
                &maximum_threads, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, device),
            "cuDeviceGetAttribute(MAX_THREADS_PER_BLOCK)");
        std::size_t total_memory = 0U;
        cuda_detail::cuda_check(
            api, api.device_total_mem(&total_memory, device), "cuDeviceTotalMem_v2");
        devices.push_back({
            ordinal,
            std::string{name.data()},
            format_uuid(uuid),
            major,
            minor,
            driver_version,
            total_memory,
            maximum_threads,
        });
    }
    return devices;
}

class CurrentContextGuard {
public:
    CurrentContextGuard(const DriverApi &api, CUcontext desired) : api_(&api) {
        cuda_detail::cuda_check(api, api.ctx_get_current(&previous_), "cuCtxGetCurrent");
        if (previous_ != desired) {
            cuda_detail::cuda_check(api, api.ctx_set_current(desired), "cuCtxSetCurrent");
            changed_ = true;
        }
    }

    ~CurrentContextGuard() {
        if (changed_) (void)api_->ctx_set_current(previous_);
    }

    CurrentContextGuard(const CurrentContextGuard &) = delete;
    CurrentContextGuard &operator=(const CurrentContextGuard &) = delete;

private:
    const DriverApi *api_ = nullptr;
    CUcontext previous_ = nullptr;
    bool changed_ = false;
};

struct DeviceBuffer {
    const DriverApi *api = nullptr;
    CUdeviceptr pointer = 0U;
    std::size_t bytes = 0U;

    DeviceBuffer() = default;
    DeviceBuffer(const DriverApi &requested_api, CUdeviceptr requested_pointer,
                 std::size_t requested_bytes)
        : api(&requested_api), pointer(requested_pointer), bytes(requested_bytes) {}
    ~DeviceBuffer() { reset(); }
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;
    DeviceBuffer(DeviceBuffer &&other) noexcept { swap(other); }
    DeviceBuffer &operator=(DeviceBuffer &&other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }

    void reset() noexcept {
        if (pointer != 0U && api != nullptr) (void)api->mem_free(pointer);
        api = nullptr;
        pointer = 0U;
        bytes = 0U;
    }

    void swap(DeviceBuffer &other) noexcept {
        std::swap(api, other.api);
        std::swap(pointer, other.pointer);
        std::swap(bytes, other.bytes);
    }
};

class EventPair {
public:
    explicit EventPair(const DriverApi &api) : api_(&api) {
        cuda_detail::cuda_check(api, api.event_create(&begin_, CU_EVENT_DEFAULT),
                                "cuEventCreate");
        try {
            cuda_detail::cuda_check(api, api.event_create(&end_, CU_EVENT_DEFAULT),
                                    "cuEventCreate");
        } catch (...) {
            (void)api.event_destroy(begin_);
            begin_ = nullptr;
            throw;
        }
    }

    ~EventPair() {
        if (end_ != nullptr) (void)api_->event_destroy(end_);
        if (begin_ != nullptr) (void)api_->event_destroy(begin_);
    }
    EventPair(const EventPair &) = delete;
    EventPair &operator=(const EventPair &) = delete;

    void record_begin(CUstream stream) {
        cuda_detail::cuda_check(*api_, api_->event_record(begin_, stream), "cuEventRecord");
    }
    void record_end(CUstream stream) {
        cuda_detail::cuda_check(*api_, api_->event_record(end_, stream), "cuEventRecord");
        complete_ = true;
    }
    [[nodiscard]] double elapsed_ms() const {
        if (!complete_) return 0.0;
        float milliseconds = 0.0F;
        cuda_detail::cuda_check(
            *api_, api_->event_elapsed_time(&milliseconds, begin_, end_),
            "cuEventElapsedTime_v2");
        return static_cast<double>(milliseconds);
    }

private:
    const DriverApi *api_ = nullptr;
    CUevent begin_ = nullptr;
    CUevent end_ = nullptr;
    bool complete_ = false;
};

struct ShapeFunctions {
    CUfunction image_inverse = nullptr;
    CUfunction metric = nullptr;
    CUfunction matrix_inverse = nullptr;
    CUfunction matrix_forward = nullptr;
    CUfunction horizontal_first_metric = nullptr;
};

struct WorkingBuffers {
    DeviceBuffer transient_source;
    DeviceBuffer transient_workspace;
    DeviceBuffer transient_partials;
    CUdeviceptr source = 0U;
    CUdeviceptr workspace = 0U;
    CUdeviceptr partials = 0U;
    std::size_t resident_bytes = 0U;
};

struct PlanBuffers {
    DeviceBuffer descriptors;
    DeviceBuffer transpose_offsets;
    DeviceBuffer transpose_indices;
    DeviceBuffer transpose_weights;
    DeviceBuffer lower_ld;
    DeviceBuffer upper_l;
    DeviceBuffer inverse_diagonal;
    DeviceBuffer forward_left;
    DeviceBuffer forward_weights;
    std::size_t total_bytes = 0U;
};

template <class Value>
[[nodiscard]] std::size_t vector_bytes(const std::vector<Value> &values,
                                       std::string_view name) {
    if (values.empty()) {
        throw std::invalid_argument(std::string{name} + " must not be empty");
    }
    return checked_product(values.size(), sizeof(Value), name);
}

} // namespace

struct CudaAnalysisEngine::Impl {
    explicit Impl(CudaAnalysisOptions requested_options)
        : options(std::move(requested_options)), api(cuda_detail::load_cuda_driver()) {
        if (options.tile_size == 0U || options.reduction_groups_per_candidate == 0U
            || options.inverse_threads_per_block == 0U) {
            throw std::invalid_argument("CUDA execution configuration counts must be positive");
        }
        if (options.reduction_groups_per_candidate
            > (std::numeric_limits<std::uint32_t>::max()
               / reduction_width)) {
            throw std::invalid_argument("CUDA reduction group count exceeds the 32-bit schedule");
        }
        if (options.maximum_total_working_set_bytes == 0U
            || options.maximum_total_working_set_bytes
                >= 2ULL * 1024ULL * 1024ULL * 1024ULL) {
            throw std::invalid_argument("CUDA total explicit working-set limit must be below 2 GiB");
        }
        if (options.reuse_working_buffers
            && options.retained_working_buffer_limit_bytes == 0U) {
            throw std::invalid_argument(
                "CUDA retained-buffer limit must be positive when reuse is enabled");
        }
        if (options.kernel_variant == CudaKernelVariant::architecture_specific) {
            throw std::runtime_error(
                "CUDA architecture-specific variant is not compiled and benchmark-approved");
        }
        if (options.kernel_variant == CudaKernelVariant::inline_ptx) {
            throw std::runtime_error("CUDA inline-ptx variant is unavailable: NO_PTX_CANDIDATE");
        }

        artifact = production_cuda_artifact();
        telemetry.module_artifact_name = artifact.name;
        telemetry.module_artifact_bytes = artifact.size;
        telemetry.module_artifact_hash_fnv1a64 = fnv1a64_hex(
            artifact.data, artifact.size);
        telemetry.module_compile_flags = artifact.compile_flags;
        telemetry.ptx_jit_forced = environment_flag_enabled("CUDA_FORCE_PTX_JIT")
            || environment_flag_enabled("CUDA_FORCE_JIT");
        if (telemetry.ptx_jit_forced) {
            telemetry.module_path_provenance = "forced-generic-ptx-jit";
        } else if (environment_flag_enabled("CUDA_DISABLE_PTX_JIT")) {
            telemetry.module_path_provenance = "native-cubin-only";
        } else {
            telemetry.module_path_provenance = "native-cubin-preferred-with-ptx-fallback";
        }

        const auto devices = enumerate_devices(*api);
        if (devices.empty()) throw std::runtime_error("no CUDA device is available");
        const auto selected = std::find_if(devices.begin(), devices.end(), [&](const auto &item) {
            return options.device_uuid.empty()
                ? item.ordinal == options.device_ordinal
                : item.uuid == options.device_uuid;
        });
        if (selected == devices.end()) {
            throw std::runtime_error(options.device_uuid.empty()
                ? "CUDA device ordinal was not found"
                : "CUDA device UUID was not found");
        }
        info = *selected;
        device = static_cast<CUdevice>(info.ordinal);
        if (options.inverse_threads_per_block
            > static_cast<std::size_t>(info.maximum_threads_per_block)
            || reduction_width
                > static_cast<std::uint32_t>(info.maximum_threads_per_block)) {
            throw std::runtime_error("CUDA device cannot run the configured block sizes");
        }
        cuda_detail::cuda_check(
            *api,
            api->device_get_attribute(
                &maximum_grid_x, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X, device),
            "cuDeviceGetAttribute(MAX_GRID_DIM_X)");
        cuda_detail::cuda_check(
            *api,
            api->device_get_attribute(
                &maximum_grid_y, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y, device),
            "cuDeviceGetAttribute(MAX_GRID_DIM_Y)");

        CUcontext previous = nullptr;
        cuda_detail::cuda_check(*api, api->ctx_get_current(&previous), "cuCtxGetCurrent");
        try {
            cuda_detail::cuda_check(
                *api, api->ctx_create(&context, CU_CTX_SCHED_AUTO, device), "cuCtxCreate_v2");
            const auto module_start = std::chrono::steady_clock::now();
            load_module();
            telemetry.module_load_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - module_start).count();
            cuda_detail::cuda_check(
                *api, api->stream_create(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
            load_functions();
            cuda_detail::cuda_check(*api, api->ctx_set_current(previous), "cuCtxSetCurrent");
        } catch (...) {
            if (stream != nullptr) (void)api->stream_destroy(stream);
            if (module != nullptr) (void)api->module_unload(module);
            if (context != nullptr) (void)api->ctx_destroy(context);
            context = nullptr;
            (void)api->ctx_set_current(previous);
            throw;
        }
    }

    ~Impl() {
        if (context == nullptr) return;
        CUcontext previous = nullptr;
        (void)api->ctx_get_current(&previous);
        (void)api->ctx_set_current(context);
        if (stream != nullptr) (void)api->stream_synchronize(stream);
        clear_retained_buffers();
        if (stream != nullptr) (void)api->stream_destroy(stream);
        if (module != nullptr) (void)api->module_unload(module);
        (void)api->ctx_destroy(context);
        context = nullptr;
        (void)api->ctx_set_current(previous);
    }

    CudaAnalysisOptions options;
    std::shared_ptr<DriverApi> api;
    EmbeddedCudaArtifact artifact;
    CudaDeviceInfo info;
    CUdevice device = 0;
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUstream stream = nullptr;
    int maximum_grid_x = 0;
    int maximum_grid_y = 0;
    std::array<ShapeFunctions, shape_count> functions{};
    DeviceBuffer retained_source;
    DeviceBuffer retained_workspace;
    DeviceBuffer retained_partials;
    CudaRuntimeTelemetry telemetry;
    std::size_t peak_workspace_elements = 0U;
    mutable std::mutex mutex;

    void load_module() {
        std::array<char, jit_log_capacity> info_log{};
        std::array<char, jit_log_capacity> error_log{};
        const auto info_size = static_cast<unsigned int>(info_log.size());
        const auto error_size = static_cast<unsigned int>(error_log.size());
        std::array options_array{
            CU_JIT_INFO_LOG_BUFFER,
            CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
            CU_JIT_ERROR_LOG_BUFFER,
            CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
        };
        std::array<void *, 4> values{
            info_log.data(),
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(info_size)),
            error_log.data(),
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(error_size)),
        };
        const CUresult result = api->module_load_data_ex(
            &module, artifact.data,
            static_cast<unsigned int>(options_array.size()),
            options_array.data(), values.data());
        telemetry.module_jit_info_log.assign(info_log.data());
        telemetry.module_jit_error_log.assign(error_log.data());
        if (result != CUDA_SUCCESS) {
            std::string message = "cuModuleLoadDataEx failed: "
                + cuda_detail::cuda_error(*api, result);
            if (!telemetry.module_jit_error_log.empty()) {
                message += "; JIT log: " + telemetry.module_jit_error_log;
            }
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] CUfunction load_function(std::string name) {
        CUfunction function = nullptr;
        cuda_detail::cuda_check(
            *api, api->module_get_function(&function, module, name.c_str()),
            ("cuModuleGetFunction(" + name + ")").c_str());
        telemetry.created_kernel_names.push_back(std::move(name));
        const auto attribute = [&](CUfunction_attribute requested,
                                   const char *operation) {
            int value = 0;
            cuda_detail::cuda_check(
                *api, api->func_get_attribute(&value, requested, function), operation);
            return value;
        };
        telemetry.module_binary_version = std::max(
            telemetry.module_binary_version,
            attribute(CU_FUNC_ATTRIBUTE_BINARY_VERSION,
                      "cuFuncGetAttribute(BINARY_VERSION)"));
        telemetry.module_ptx_version = std::max(
            telemetry.module_ptx_version,
            attribute(CU_FUNC_ATTRIBUTE_PTX_VERSION,
                      "cuFuncGetAttribute(PTX_VERSION)"));
        telemetry.maximum_kernel_register_count = std::max(
            telemetry.maximum_kernel_register_count,
            attribute(CU_FUNC_ATTRIBUTE_NUM_REGS,
                      "cuFuncGetAttribute(NUM_REGS)"));
        telemetry.maximum_kernel_static_shared_bytes = std::max(
            telemetry.maximum_kernel_static_shared_bytes,
            attribute(CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
                      "cuFuncGetAttribute(SHARED_SIZE_BYTES)"));
        return function;
    }

    void load_functions() {
        for (const KernelShape shape : {
                 KernelShape::bandwidth3,
                 KernelShape::bandwidth7,
                 KernelShape::bandwidth11,
                 KernelShape::bandwidth15,
                 KernelShape::generic,
             }) {
            const std::string suffix = shape_name(shape);
            ShapeFunctions &entry = functions[shape_index(shape)];
            entry.image_inverse = load_function("inverse_axis_" + suffix);
            entry.metric = load_function("metric_axis_p1_" + suffix);
            entry.matrix_inverse = load_function("inverse_axis_matrix_" + suffix);
            entry.matrix_forward = load_function("forward_axis_matrix_" + suffix);
            entry.horizontal_first_metric = load_function(
                "metric_axis_p1_horizontal_first_" + suffix);
        }
    }

    [[nodiscard]] DeviceBuffer allocate_buffer(std::size_t bytes, bool working) {
        if (bytes == 0U) throw std::invalid_argument("CUDA buffer must not be empty");
        const auto start = std::chrono::steady_clock::now();
        CUdeviceptr pointer = 0U;
        cuda_detail::cuda_check(*api, api->mem_alloc(&pointer, bytes), "cuMemAlloc_v2");
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        ++telemetry.buffer_allocation_count;
        telemetry.buffer_allocation_bytes = checked_add(
            telemetry.buffer_allocation_bytes, bytes, "allocation telemetry");
        telemetry.buffer_allocation_ms += elapsed;
        if (working) {
            ++telemetry.working_buffer_allocation_count;
            telemetry.working_buffer_allocation_bytes = checked_add(
                telemetry.working_buffer_allocation_bytes, bytes,
                "working allocation telemetry");
            telemetry.working_buffer_allocation_ms += elapsed;
        }
        return DeviceBuffer{*api, pointer, bytes};
    }

    [[nodiscard]] std::size_t retained_bytes() const {
        return checked_add(
            checked_add(retained_source.bytes, retained_workspace.bytes,
                        "retained CUDA buffers"),
            retained_partials.bytes, "retained CUDA buffers");
    }

    void clear_retained_buffers() noexcept {
        retained_partials.reset();
        retained_workspace.reset();
        retained_source.reset();
    }

    CUdeviceptr acquire_retained(DeviceBuffer &buffer, std::size_t bytes) {
        if (buffer.pointer != 0U && buffer.bytes >= bytes) {
            ++telemetry.working_buffer_reuse_count;
            return buffer.pointer;
        }
        buffer.reset();
        buffer = allocate_buffer(bytes, true);
        return buffer.pointer;
    }

    [[nodiscard]] WorkingBuffers prepare_working_buffers(
        std::size_t source_bytes, std::size_t workspace_bytes,
        std::size_t partial_bytes) {
        const std::size_t active = checked_add(
            checked_add(source_bytes, workspace_bytes, "CUDA working buffers"),
            partial_bytes, "CUDA working buffers");
        if (active > options.maximum_total_working_set_bytes) {
            throw std::length_error("CUDA working buffers exceed the total explicit limit");
        }
        telemetry.working_buffer_active_bytes = active;
        telemetry.working_buffer_peak_active_bytes = std::max(
            telemetry.working_buffer_peak_active_bytes, active);

        bool retain = options.reuse_working_buffers;
        if (retain) {
            const std::size_t desired = checked_add(
                checked_add(std::max(source_bytes, retained_source.bytes),
                            std::max(workspace_bytes, retained_workspace.bytes),
                            "CUDA retained buffers"),
                std::max(partial_bytes, retained_partials.bytes),
                "CUDA retained buffers");
            retain = desired <= options.retained_working_buffer_limit_bytes
                && desired <= options.maximum_total_working_set_bytes;
        }

        WorkingBuffers result;
        if (retain) {
            result.source = acquire_retained(retained_source, source_bytes);
            result.workspace = acquire_retained(retained_workspace, workspace_bytes);
            result.partials = acquire_retained(retained_partials, partial_bytes);
            result.resident_bytes = retained_bytes();
            telemetry.working_buffer_peak_retained_bytes = std::max(
                telemetry.working_buffer_peak_retained_bytes, result.resident_bytes);
        } else {
            clear_retained_buffers();
            result.transient_source = allocate_buffer(source_bytes, true);
            result.transient_workspace = allocate_buffer(workspace_bytes, true);
            result.transient_partials = allocate_buffer(partial_bytes, true);
            result.source = result.transient_source.pointer;
            result.workspace = result.transient_workspace.pointer;
            result.partials = result.transient_partials.pointer;
            result.resident_bytes = active;
        }
        telemetry.working_buffer_retained_bytes = retained_bytes();
        telemetry.total_peak_explicit_bytes = std::max(
            telemetry.total_peak_explicit_bytes, result.resident_bytes);
        return result;
    }

    template <class Value>
    [[nodiscard]] DeviceBuffer upload_plan_vector(const std::vector<Value> &values,
                                                   std::string_view name) {
        const std::size_t bytes = vector_bytes(values, name);
        DeviceBuffer buffer = allocate_buffer(bytes, false);
        try {
            cuda_detail::cuda_check(
                *api,
                api->memcpy_htod_async(buffer.pointer, values.data(), bytes, stream),
                "cuMemcpyHtoDAsync_v2(plan)");
            telemetry.plan_upload_bytes = checked_add(
                telemetry.plan_upload_bytes, bytes, "plan upload telemetry");
        } catch (...) {
            const std::exception_ptr original = std::current_exception();
            try {
                drain();
            } catch (...) {
                // Preserve the operation that initiated failure after mandatory drain.
            }
            std::rethrow_exception(original);
        }
        return buffer;
    }

    [[nodiscard]] PlanBuffers upload_plan(const PackedTile &packed,
                                          std::size_t persistent_bytes,
                                          EventPair &timer) {
        PlanBuffers result;
        result.total_bytes = gpu::packed_plan_bytes(packed);
        if (checked_add(persistent_bytes, result.total_bytes,
                        "CUDA total explicit working set")
            > options.maximum_total_working_set_bytes) {
            throw std::length_error("CUDA tile plan exceeds the total explicit working-set limit");
        }
        telemetry.queued_plan_peak_bytes = std::max(
            telemetry.queued_plan_peak_bytes, result.total_bytes);
        telemetry.total_peak_explicit_bytes = std::max(
            telemetry.total_peak_explicit_bytes,
            persistent_bytes + result.total_bytes);
        try {
            timer.record_begin(stream);
            result.descriptors = upload_plan_vector(packed.descriptors, "plan descriptors");
            result.transpose_offsets = upload_plan_vector(
                packed.transpose_offsets, "transpose offsets");
            result.transpose_indices = upload_plan_vector(
                packed.transpose_indices, "transpose indices");
            result.transpose_weights = upload_plan_vector(
                packed.transpose_weights, "transpose weights");
            result.lower_ld = upload_plan_vector(packed.lower_ld, "lower factors");
            result.upper_l = upload_plan_vector(packed.upper_l, "upper factors");
            result.inverse_diagonal = upload_plan_vector(
                packed.inverse_diagonal, "inverse diagonal");
            result.forward_left = upload_plan_vector(packed.forward_left, "forward left");
            result.forward_weights = upload_plan_vector(
                packed.forward_weights, "forward weights");
            timer.record_end(stream);
        } catch (...) {
            const std::exception_ptr original = std::current_exception();
            try {
                drain();
            } catch (...) {
                // Preserve the operation that initiated failure after mandatory drain.
            }
            std::rethrow_exception(original);
        }
        return result;
    }

    [[nodiscard]] CUfunction image_inverse(KernelShape shape) const noexcept {
        return functions[shape_index(shape)].image_inverse;
    }
    [[nodiscard]] CUfunction metric(KernelShape shape) const noexcept {
        return functions[shape_index(shape)].metric;
    }
    [[nodiscard]] CUfunction matrix_inverse(KernelShape shape) const noexcept {
        return functions[shape_index(shape)].matrix_inverse;
    }
    [[nodiscard]] CUfunction matrix_forward(KernelShape shape) const noexcept {
        return functions[shape_index(shape)].matrix_forward;
    }
    [[nodiscard]] CUfunction horizontal_first_metric(KernelShape shape) const noexcept {
        return functions[shape_index(shape)].horizontal_first_metric;
    }

    void launch_blocks(CUfunction function, std::size_t grid_x,
                       std::size_t grid_y, std::uint32_t block_size,
                       void **parameters, const char *operation) {
        if (grid_x == 0U || grid_y == 0U || block_size == 0U) {
            throw std::logic_error("CUDA kernel launch dimensions must be positive");
        }
        if (grid_x > static_cast<std::size_t>(maximum_grid_x)
            || grid_y > static_cast<std::size_t>(maximum_grid_y)) {
            throw std::length_error("CUDA kernel grid exceeds the device limits");
        }
        const auto start = std::chrono::steady_clock::now();
        cuda_detail::cuda_check(
            *api,
            api->launch_kernel(
                function, static_cast<unsigned int>(grid_x),
                static_cast<unsigned int>(grid_y), 1U,
                block_size, 1U, 1U, 0U, stream, parameters, nullptr),
            operation);
        telemetry.buffer_wiring_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    void launch(CUfunction function, std::size_t work_items_per_candidate,
                std::size_t candidate_count, std::uint32_t block_size,
                void **parameters, const char *operation) {
        if (work_items_per_candidate == 0U || block_size == 0U) {
            throw std::logic_error("CUDA kernel work item and block counts must be positive");
        }
        const std::size_t grid_x =
            (work_items_per_candidate + block_size - 1U) / block_size;
        launch_blocks(function, grid_x, candidate_count, block_size,
                      parameters, operation);
    }

    void launch_image_inverse(
        CUfunction function, AnalysisJob job, const PlanBuffers &plan,
        CUdeviceptr source, CUdeviceptr workspace,
        std::size_t work_items_per_candidate) {
        CUdeviceptr descriptors = plan.descriptors.pointer;
        CUdeviceptr transpose_offsets = plan.transpose_offsets.pointer;
        CUdeviceptr transpose_indices = plan.transpose_indices.pointer;
        CUdeviceptr transpose_weights = plan.transpose_weights.pointer;
        CUdeviceptr lower_ld = plan.lower_ld.pointer;
        CUdeviceptr upper_l = plan.upper_l.pointer;
        CUdeviceptr inverse_diagonal = plan.inverse_diagonal.pointer;
        std::array<void *, 10> parameters{
            &source, &job, &descriptors, &transpose_offsets, &transpose_indices,
            &transpose_weights, &lower_ld, &upper_l, &inverse_diagonal, &workspace,
        };
        launch(function, work_items_per_candidate, job.candidate_count,
               checked_u32(options.inverse_threads_per_block, "inverse block size"),
               parameters.data(), "cuLaunchKernel(image inverse)");
    }

    void launch_matrix_inverse(
        CUfunction function, AnalysisJob job, const PlanBuffers &plan,
        CUdeviceptr descriptor_pointer, CUdeviceptr workspace,
        std::size_t work_items_per_candidate) {
        CUdeviceptr transpose_offsets = plan.transpose_offsets.pointer;
        CUdeviceptr transpose_indices = plan.transpose_indices.pointer;
        CUdeviceptr transpose_weights = plan.transpose_weights.pointer;
        CUdeviceptr lower_ld = plan.lower_ld.pointer;
        CUdeviceptr upper_l = plan.upper_l.pointer;
        CUdeviceptr inverse_diagonal = plan.inverse_diagonal.pointer;
        std::array<void *, 9> parameters{
            &job, &descriptor_pointer, &transpose_offsets, &transpose_indices,
            &transpose_weights, &lower_ld, &upper_l, &inverse_diagonal, &workspace,
        };
        launch(function, work_items_per_candidate, job.candidate_count,
               checked_u32(options.inverse_threads_per_block, "inverse block size"),
               parameters.data(), "cuLaunchKernel(matrix inverse)");
    }

    void launch_matrix_forward(
        CUfunction function, AnalysisJob job, const PlanBuffers &plan,
        CUdeviceptr descriptor_pointer, CUdeviceptr workspace,
        std::size_t work_items_per_candidate) {
        CUdeviceptr forward_left = plan.forward_left.pointer;
        CUdeviceptr forward_weights = plan.forward_weights.pointer;
        std::array<void *, 5> parameters{
            &job, &descriptor_pointer, &forward_left, &forward_weights, &workspace,
        };
        launch(function, work_items_per_candidate, job.candidate_count,
               checked_u32(options.inverse_threads_per_block, "forward block size"),
               parameters.data(), "cuLaunchKernel(matrix forward)");
    }

    void launch_metric(
        CUfunction function, AnalysisJob job, const PlanBuffers &plan,
        CUdeviceptr descriptor_pointer, CUdeviceptr source, CUdeviceptr workspace,
        CUdeviceptr partials, std::size_t partial_count) {
        CUdeviceptr forward_left = plan.forward_left.pointer;
        CUdeviceptr forward_weights = plan.forward_weights.pointer;
        std::array<void *, 7> parameters{
            &source, &job, &descriptor_pointer, &forward_left, &forward_weights,
            &workspace, &partials,
        };
        const std::size_t expected_partials = checked_product(
            static_cast<std::size_t>(job.candidate_count),
            static_cast<std::size_t>(job.groups_per_candidate),
            "CUDA metric dispatch");
        if (partial_count != expected_partials) {
            throw std::logic_error("CUDA metric partial count mismatch");
        }
        launch_blocks(function, job.groups_per_candidate, job.candidate_count,
                      reduction_width, parameters.data(), "cuLaunchKernel(metric)");
    }

    void drain() {
        cuda_detail::cuda_check(*api, api->stream_synchronize(stream),
                                "cuStreamSynchronize");
    }
};

CudaRuntimeProbe cuda_runtime_probe() noexcept {
    CudaRuntimeProbe probe;
    try {
        const auto api = cuda_detail::load_cuda_driver();
        probe.driver_loaded = true;
        cuda_detail::cuda_check(*api, api->init(0U), "cuInit");
        probe.initialized = true;
        probe.devices = enumerate_devices(*api);
        probe.device_available = !probe.devices.empty();
        if (!probe.device_available) probe.reason = "CUDA driver reported no devices";
    } catch (const std::exception &error) {
        probe.reason = error.what();
    } catch (...) {
        probe.reason = "unknown CUDA probe failure";
    }
    return probe;
}

bool cuda_backend_available() noexcept {
    return cuda_runtime_probe().device_available;
}

std::vector<CudaDeviceInfo> enumerate_cuda_devices() {
    const auto api = cuda_detail::load_cuda_driver();
    return enumerate_devices(*api);
}

CudaAnalysisEngine::CudaAnalysisEngine(CudaAnalysisOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CudaAnalysisEngine::~CudaAnalysisEngine() = default;
CudaAnalysisEngine::CudaAnalysisEngine(CudaAnalysisEngine &&) noexcept = default;
CudaAnalysisEngine &CudaAnalysisEngine::operator=(CudaAnalysisEngine &&) noexcept = default;

const CudaDeviceInfo &CudaAnalysisEngine::device_info() const noexcept { return impl_->info; }
const CudaAnalysisOptions &CudaAnalysisEngine::options() const noexcept { return impl_->options; }
std::size_t CudaAnalysisEngine::peak_workspace_elements() const noexcept {
    return impl_->peak_workspace_elements;
}
std::size_t CudaAnalysisEngine::peak_working_set_bytes() const noexcept {
    return impl_->telemetry.total_peak_explicit_bytes;
}

CudaRuntimeTelemetry CudaAnalysisEngine::runtime_telemetry() const {
    const std::scoped_lock lock(impl_->mutex);
    CudaRuntimeTelemetry result = impl_->telemetry;
    result.working_buffer_retained_bytes = impl_->retained_bytes();
    return result;
}

void CudaAnalysisEngine::reset_analysis_telemetry() {
    const std::scoped_lock lock(impl_->mutex);
    const double module_load_ms = impl_->telemetry.module_load_ms;
    const std::size_t artifact_bytes = impl_->telemetry.module_artifact_bytes;
    const std::int32_t binary_version = impl_->telemetry.module_binary_version;
    const std::int32_t ptx_version = impl_->telemetry.module_ptx_version;
    const std::int32_t maximum_registers =
        impl_->telemetry.maximum_kernel_register_count;
    const std::int32_t maximum_static_shared =
        impl_->telemetry.maximum_kernel_static_shared_bytes;
    const bool ptx_jit_forced = impl_->telemetry.ptx_jit_forced;
    const std::vector<std::string> names = impl_->telemetry.created_kernel_names;
    const std::string artifact_name = impl_->telemetry.module_artifact_name;
    const std::string artifact_hash =
        impl_->telemetry.module_artifact_hash_fnv1a64;
    const std::string compile_flags = impl_->telemetry.module_compile_flags;
    const std::string path_provenance = impl_->telemetry.module_path_provenance;
    const std::string info_log = impl_->telemetry.module_jit_info_log;
    const std::string error_log = impl_->telemetry.module_jit_error_log;
    impl_->telemetry = {};
    impl_->telemetry.module_load_ms = module_load_ms;
    impl_->telemetry.module_artifact_bytes = artifact_bytes;
    impl_->telemetry.module_binary_version = binary_version;
    impl_->telemetry.module_ptx_version = ptx_version;
    impl_->telemetry.maximum_kernel_register_count = maximum_registers;
    impl_->telemetry.maximum_kernel_static_shared_bytes = maximum_static_shared;
    impl_->telemetry.ptx_jit_forced = ptx_jit_forced;
    impl_->telemetry.created_kernel_names = names;
    impl_->telemetry.module_artifact_name = artifact_name;
    impl_->telemetry.module_artifact_hash_fnv1a64 = artifact_hash;
    impl_->telemetry.module_compile_flags = compile_flags;
    impl_->telemetry.module_path_provenance = path_provenance;
    impl_->telemetry.module_jit_info_log = info_log;
    impl_->telemetry.module_jit_error_log = error_log;
    impl_->telemetry.working_buffer_retained_bytes = impl_->retained_bytes();
    impl_->telemetry.working_buffer_peak_retained_bytes = impl_->retained_bytes();
}

void CudaAnalysisEngine::trim_working_buffers() {
    const std::scoped_lock lock(impl_->mutex);
    CurrentContextGuard context_guard(*impl_->api, impl_->context);
    impl_->drain();
    impl_->clear_retained_buffers();
    impl_->telemetry.working_buffer_retained_bytes = 0U;
}

std::vector<CandidateResult> CudaAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    const std::scoped_lock call_lock(impl_->mutex);
    const gpu::MetricCropBounds crop = gpu::validate_source_and_metric(source, metric);
    if (candidates.empty()) return {};
    if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");
    CurrentContextGuard context_guard(*impl_->api, impl_->context);

    const std::size_t image_elements = checked_product(
        static_cast<std::size_t>(source.width), static_cast<std::size_t>(source.height),
        "CUDA source image");
    (void)checked_u32(image_elements, "CUDA source image element count");
    std::vector<float> contiguous_source;
    const float *source_data = source.data;
    if (source.stride != source.width) {
        const std::size_t last_row = static_cast<std::size_t>(source.height - 1);
        const std::size_t stride = static_cast<std::size_t>(source.stride);
        if (last_row != 0U
            && stride > (static_cast<std::size_t>(
                              std::numeric_limits<std::ptrdiff_t>::max())
                          - static_cast<std::size_t>(source.width)) / last_row) {
            throw std::length_error("CUDA source stride exceeds pointer arithmetic range");
        }
        contiguous_source.resize(image_elements);
        for (std::int32_t y = 0; y < source.height; ++y) {
            std::copy_n(
                source.data + static_cast<std::ptrdiff_t>(y) * source.stride,
                source.width,
                contiguous_source.data()
                    + static_cast<std::ptrdiff_t>(y) * source.width);
        }
        source_data = contiguous_source.data();
    }

    const std::size_t device_workspace_limit =
        impl_->options.maximum_total_working_set_bytes / sizeof(float);
    const std::size_t configured_workspace_limit =
        impl_->options.workspace_limit_elements == 0U
        ? std::min(device_workspace_limit,
                   static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        : std::min({
            device_workspace_limit,
            impl_->options.workspace_limit_elements,
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
        });
    const gpu::KernelDispatchPolicy policy = dispatch_policy(
        impl_->options.kernel_variant);
    const gpu::TiledBatch tiled_batch = gpu::plan_tiles(
        source, candidates, impl_->options.tile_size, configured_workspace_limit,
        policy, stop);
    if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");
    const auto &tiles = tiled_batch.tiles;
    const std::size_t maximum_workspace = tiled_batch.maximum_workspace_elements;
    impl_->peak_workspace_elements = std::max(
        impl_->peak_workspace_elements, maximum_workspace);

    const std::size_t groups = impl_->options.reduction_groups_per_candidate;
    const std::size_t source_bytes = checked_product(
        image_elements, sizeof(float), "CUDA source buffer");
    const std::size_t workspace_bytes = checked_product(
        maximum_workspace, sizeof(float), "CUDA workspace buffer");
    const std::size_t partial_count = checked_product(
        candidates.size(), groups, "CUDA partial results");
    const std::size_t partial_bytes = checked_product(
        partial_count, sizeof(float), "CUDA partial result buffer");
    WorkingBuffers working = impl_->prepare_working_buffers(
        source_bytes, workspace_bytes, partial_bytes);
    std::vector<float> host_partials(partial_count);

    try {
        EventPair source_upload(*impl_->api);
        source_upload.record_begin(impl_->stream);
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->memcpy_htod_async(
                working.source, source_data, source_bytes, impl_->stream),
            "cuMemcpyHtoDAsync_v2(source)");
        source_upload.record_end(impl_->stream);
        impl_->telemetry.source_upload_bytes = checked_add(
            impl_->telemetry.source_upload_bytes, source_bytes,
            "source upload telemetry");
        bool source_timing_recorded = false;

        for (const TileRange &tile : tiles) {
        if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");
        const auto pack_start = std::chrono::steady_clock::now();
        const PackedTile packed = gpu::pack_tile(
            source, candidates.subspan(tile.begin, tile.end - tile.begin), policy);
        impl_->telemetry.plan_pack_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pack_start).count();
        if (packed.workspace_elements != tile.workspace_elements
            || packed.workspace_elements > maximum_workspace) {
            throw std::logic_error("CUDA tile workspace preflight mismatch");
        }

        ++impl_->telemetry.analyzed_tile_count;
        if (gpu::uses_specialized_pipeline(tile.signature)) {
            ++impl_->telemetry.specialized_tile_count;
        } else {
            ++impl_->telemetry.generic_tile_count;
        }

        EventPair plan_upload(*impl_->api);
        EventPair inverse_h(*impl_->api);
        EventPair inverse_v(*impl_->api);
        EventPair first_forward(*impl_->api);
        EventPair metric_timer(*impl_->api);
        EventPair readback(*impl_->api);
        PlanBuffers plan;
        bool submitted = false;
        bool synchronized = false;
        const auto record_submission = [&] {
            if (!submitted) {
                submitted = true;
                ++impl_->telemetry.stream_submission_count;
            }
        };
        try {
            plan = impl_->upload_plan(packed, working.resident_bytes, plan_upload);
            if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");

            const std::size_t tile_candidate_count = tile.end - tile.begin;
            AnalysisJob job{
                static_cast<std::uint32_t>(source.width),
                static_cast<std::uint32_t>(source.height),
                crop.left,
                crop.right,
                crop.top,
                crop.bottom,
                metric.threshold,
                checked_u32(groups, "CUDA reduction groups"),
                checked_u32(tile_candidate_count, "CUDA tile candidate count"),
                packed.maximum_vector_count,
            };
            const std::size_t tile_partial_count = checked_product(
                tile_candidate_count, groups, "CUDA tile partials");
            (void)checked_u32(tile_partial_count, "CUDA tile partial count");
            const std::size_t tile_partial_offset = checked_product(
                tile.begin, groups, "CUDA partial candidate offset");
            CUdeviceptr tile_partials = working.partials
                + checked_product(tile_partial_offset, sizeof(float),
                                  "CUDA partial offset");

            if (tile.signature.axes != AnalysisAxes::both) {
                const bool horizontal = tile.signature.axes == AnalysisAxes::horizontal;
                const KernelShape inverse_shape = horizontal
                    ? tile.signature.horizontal_inverse_shape
                    : tile.signature.vertical_inverse_shape;
                const KernelShape forward_shape = horizontal
                    ? tile.signature.horizontal_forward_shape
                    : tile.signature.vertical_forward_shape;
                job.maximum_vector_count = packed.maximum_vector_count;
                inverse_h.record_begin(impl_->stream);
                impl_->launch_image_inverse(
                    impl_->image_inverse(inverse_shape), job, plan,
                    working.source, working.workspace, packed.maximum_vector_count);
                record_submission();
                inverse_h.record_end(impl_->stream);
                if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");
                metric_timer.record_begin(impl_->stream);
                impl_->launch_metric(
                    impl_->metric(forward_shape), job, plan, plan.descriptors.pointer,
                    working.source, working.workspace, tile_partials,
                    tile_partial_count);
                metric_timer.record_end(impl_->stream);
            } else {
                const CUdeviceptr vertical_descriptors = plan.descriptors.pointer
                    + checked_product(tile_candidate_count, sizeof(AxisPlanDescriptor),
                                      "CUDA vertical descriptor offset");
                AnalysisJob horizontal_job = job;
                horizontal_job.maximum_vector_count = static_cast<std::uint32_t>(source.height);
                AnalysisJob vertical_job = job;
                vertical_job.maximum_vector_count = packed.maximum_native_width;
                const bool vertical_first =
                    tile.signature.forward_order == ForwardOrder::vertical_first;
                AnalysisJob forward_job = job;
                forward_job.maximum_vector_count = vertical_first
                    ? packed.maximum_native_width : packed.maximum_native_height;
                inverse_h.record_begin(impl_->stream);
                impl_->launch_image_inverse(
                    impl_->image_inverse(tile.signature.horizontal_inverse_shape),
                    horizontal_job, plan, working.source, working.workspace,
                    static_cast<std::size_t>(source.height));
                record_submission();
                inverse_h.record_end(impl_->stream);
                if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");
                inverse_v.record_begin(impl_->stream);
                impl_->launch_matrix_inverse(
                    impl_->matrix_inverse(tile.signature.vertical_inverse_shape),
                    vertical_job, plan, vertical_descriptors, working.workspace,
                    packed.maximum_native_width);
                inverse_v.record_end(impl_->stream);
                if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");

                const KernelShape first_shape = vertical_first
                    ? tile.signature.vertical_forward_shape
                    : tile.signature.horizontal_forward_shape;
                const CUdeviceptr first_descriptors = vertical_first
                    ? vertical_descriptors : plan.descriptors.pointer;
                first_forward.record_begin(impl_->stream);
                impl_->launch_matrix_forward(
                    impl_->matrix_forward(first_shape), forward_job, plan,
                    first_descriptors, working.workspace,
                    forward_job.maximum_vector_count);
                first_forward.record_end(impl_->stream);
                if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");

                const CUfunction final_function = vertical_first
                    ? impl_->metric(tile.signature.horizontal_forward_shape)
                    : impl_->horizontal_first_metric(
                        tile.signature.vertical_forward_shape);
                const CUdeviceptr final_descriptors = vertical_first
                    ? plan.descriptors.pointer : vertical_descriptors;
                metric_timer.record_begin(impl_->stream);
                impl_->launch_metric(
                    final_function, job, plan, final_descriptors, working.source,
                    working.workspace, tile_partials, tile_partial_count);
                metric_timer.record_end(impl_->stream);
            }

            readback.record_begin(impl_->stream);
            const std::size_t tile_partial_bytes = checked_product(
                tile_partial_count, sizeof(float), "CUDA tile partial readback");
            cuda_detail::cuda_check(
                *impl_->api,
                impl_->api->memcpy_dtoh_async(
                    host_partials.data() + tile_partial_offset,
                    tile_partials, tile_partial_bytes, impl_->stream),
                "cuMemcpyDtoHAsync_v2(partials)");
            readback.record_end(impl_->stream);
            impl_->telemetry.partial_readback_bytes = checked_add(
                impl_->telemetry.partial_readback_bytes, tile_partial_bytes,
                "partial readback telemetry");

            impl_->drain();
            synchronized = true;
            ++impl_->telemetry.stream_completion_count;
        } catch (...) {
            const std::exception_ptr original = std::current_exception();
            if (!synchronized) {
                try {
                    impl_->drain();
                    if (submitted) ++impl_->telemetry.stream_completion_count;
                } catch (...) {
                    // The initiating failure owns the API result after mandatory drain.
                }
            }
            std::rethrow_exception(original);
        }

        if (!source_timing_recorded) {
            impl_->telemetry.source_upload_ms += source_upload.elapsed_ms();
            source_timing_recorded = true;
        }
        const double plan_upload_ms = plan_upload.elapsed_ms();
        const double inverse_h_ms = inverse_h.elapsed_ms();
        const double inverse_v_ms = inverse_v.elapsed_ms();
        const double first_forward_ms = first_forward.elapsed_ms();
        const double metric_ms = metric_timer.elapsed_ms();
        const double readback_ms = readback.elapsed_ms();
        impl_->telemetry.plan_upload_ms += plan_upload_ms;
        if (tile.signature.axes == AnalysisAxes::vertical) {
            impl_->telemetry.inverse_v_ms += inverse_h_ms;
        } else {
            impl_->telemetry.inverse_h_ms += inverse_h_ms;
        }
        impl_->telemetry.inverse_v_ms += inverse_v_ms;
        impl_->telemetry.first_forward_ms += first_forward_ms;
        impl_->telemetry.metric_ms += metric_ms;
        impl_->telemetry.partial_readback_ms += readback_ms;
        impl_->telemetry.gpu_execution_ms +=
            inverse_h_ms + inverse_v_ms + first_forward_ms + metric_ms;
        if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");
        }
    } catch (...) {
        const std::exception_ptr original = std::current_exception();
        try {
            impl_->drain();
        } catch (...) {
            // Preserve the operation that initiated failure after mandatory drain.
        }
        std::rethrow_exception(original);
    }

    const auto merge_start = std::chrono::steady_clock::now();
    std::vector<CandidateResult> results = gpu::merge_metric_partials(
        candidates, host_partials, groups, crop.pixel_count);
    impl_->telemetry.cpu_merge_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - merge_start).count();
    impl_->telemetry.working_buffer_retained_bytes = impl_->retained_bytes();
    return results;
}

} // namespace getnative

#include "getnative/cuda_analysis.hpp"

#include "cuda_baseline.hpp"
#include "cuda_driver.hpp"
#include "cuda_memory_policy.hpp"
#include "getnative_cuda_staged_fatbin.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace getnative {
namespace {

using cuda_baseline::AxisPlanDescriptor;
using cuda_baseline::CandidateDescriptor;
using cuda_detail::DriverApi;

constexpr std::size_t maximum_explicit_bytes =
    cuda_detail::cuda_maximum_explicit_bytes;
constexpr std::size_t maximum_tile_candidates = 65535U;
constexpr unsigned int inverse_vertical_columns_per_thread = 2U;
constexpr unsigned int default_pixel_threads = 256U;
constexpr std::size_t maximum_fused_horizontal_span = 4096U;
// Benchmark-gated: the shared input cache is opt-in until warm throughput is
// consistently within the 3% guard on every formal kernel matrix.
constexpr std::size_t default_input_cache_bytes = 0U;
// Benchmark-gated: set GETNATIVE_CUDA_HOST_PLAN_CACHE_BYTES to opt in. The
// 2026-08-10 1080p matrix improved bicubic/lanczos8 but regressed bilinear
// beyond the 3% guard, so the experiment is intentionally not the default.
constexpr std::size_t default_host_plan_cache_bytes = 0U;
#if !defined(GETNATIVE_CUDA_MIN_ARCHITECTURE)
#error "GETNATIVE_CUDA_MIN_ARCHITECTURE must describe the compatibility floor"
#endif
#if !defined(GETNATIVE_CUDA_ARTIFACT_TARGET)
#error "GETNATIVE_CUDA_ARTIFACT_TARGET must describe the embedded fatbin"
#endif
constexpr std::int32_t minimum_architecture = GETNATIVE_CUDA_MIN_ARCHITECTURE;

[[nodiscard]] constexpr std::int32_t architecture_code(
    std::int32_t major, std::int32_t minor) noexcept {
    return major * 10 + minor;
}

[[nodiscard]] std::string cuda_device_incompatibility(
    std::int32_t major, std::int32_t minor,
    std::int32_t maximum_threads_per_block) {
    const std::int32_t architecture = architecture_code(major, minor);
    if (architecture < minimum_architecture) {
        return "compute capability sm_" + std::to_string(architecture)
            + " is below the minimum sm_"
            + std::to_string(minimum_architecture);
    }
    if (maximum_threads_per_block
        < static_cast<std::int32_t>(default_pixel_threads)) {
        return "maximum CUDA block size is below "
            + std::to_string(default_pixel_threads) + " threads";
    }
    return {};
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left, std::size_t right, std::string_view label) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::length_error(std::string{label} + " size overflow");
    }
    return left * right;
}

[[nodiscard]] std::size_t checked_add(
    std::size_t left, std::size_t right, std::string_view label) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::length_error(std::string{label} + " size overflow");
    }
    return left + right;
}

[[nodiscard]] std::size_t environment_byte_budget(
    const char *name, std::size_t fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    try {
        std::size_t parsed = 0U;
        const unsigned long long result = std::stoull(value, &parsed, 10);
        if (value[parsed] != '\0'
            || result > static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::out_of_range("byte budget");
        }
        return static_cast<std::size_t>(result);
    } catch (const std::exception &) {
        throw std::invalid_argument(
            std::string{name} + " must be an unsigned byte count");
    }
}

[[nodiscard]] std::uint32_t checked_u32(
    std::size_t value, std::string_view label) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string{label} + " exceeds the CUDA ABI");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] constexpr bool is_power_of_two(std::uint32_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
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
    int driver_version = 0;
    cuda_detail::cuda_check(
        api, api.driver_get_version(&driver_version), "cuDriverGetVersion");
    int count = 0;
    cuda_detail::cuda_check(api, api.device_get_count(&count), "cuDeviceGetCount");
    if (count < 0) throw std::runtime_error("cuDeviceGetCount returned a negative count");

    std::vector<CudaDeviceInfo> result;
    result.reserve(static_cast<std::size_t>(count));
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
        const auto attribute = [&](CUdevice_attribute requested,
                                   const char *operation) {
            int value = 0;
            cuda_detail::cuda_check(
                api, api.device_get_attribute(&value, requested, device), operation);
            return value;
        };
        const int major = attribute(
            CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
            "cuDeviceGetAttribute(COMPUTE_CAPABILITY_MAJOR)");
        const int minor = attribute(
            CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
            "cuDeviceGetAttribute(COMPUTE_CAPABILITY_MINOR)");
        const int maximum_threads = attribute(
            CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK,
            "cuDeviceGetAttribute(MAX_THREADS_PER_BLOCK)");
        std::size_t total_memory = 0U;
        cuda_detail::cuda_check(
            api, api.device_total_mem(&total_memory, device), "cuDeviceTotalMem_v2");
        const std::string incompatibility = cuda_device_incompatibility(
            major, minor, maximum_threads);
        CudaDeviceInfo info;
        info.ordinal = ordinal;
        info.name = std::string{name.data()};
        info.uuid = format_uuid(uuid);
        info.compute_capability_major = major;
        info.compute_capability_minor = minor;
        info.driver_version = driver_version;
        info.total_memory_bytes = total_memory;
        info.maximum_threads_per_block = maximum_threads;
        info.backend_compatible = incompatibility.empty();
        info.incompatibility_reason = incompatibility;
        info.multiprocessor_count = attribute(
            CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
            "cuDeviceGetAttribute(MULTIPROCESSOR_COUNT)");
        info.maximum_threads_per_multiprocessor = attribute(
            CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_MULTIPROCESSOR,
            "cuDeviceGetAttribute(MAX_THREADS_PER_MULTIPROCESSOR)");
        info.registers_per_multiprocessor = attribute(
            CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_MULTIPROCESSOR,
            "cuDeviceGetAttribute(MAX_REGISTERS_PER_MULTIPROCESSOR)");
        info.shared_memory_per_block_bytes = attribute(
            CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK,
            "cuDeviceGetAttribute(MAX_SHARED_MEMORY_PER_BLOCK)");
        info.shared_memory_per_multiprocessor_bytes = attribute(
            CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR,
            "cuDeviceGetAttribute(MAX_SHARED_MEMORY_PER_MULTIPROCESSOR)");
        info.warp_size = attribute(
            CU_DEVICE_ATTRIBUTE_WARP_SIZE,
            "cuDeviceGetAttribute(WARP_SIZE)");
        info.clock_rate_khz = attribute(
            CU_DEVICE_ATTRIBUTE_CLOCK_RATE,
            "cuDeviceGetAttribute(CLOCK_RATE)");
        info.memory_clock_rate_khz = attribute(
            CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE,
            "cuDeviceGetAttribute(MEMORY_CLOCK_RATE)");
        info.memory_bus_width_bits = attribute(
            CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH,
            "cuDeviceGetAttribute(GLOBAL_MEMORY_BUS_WIDTH)");
        info.l2_cache_bytes = attribute(
            CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE,
            "cuDeviceGetAttribute(L2_CACHE_SIZE)");
        result.push_back(std::move(info));
    }
    return result;
}

[[nodiscard]] std::string fnv1a64_hex(
    const unsigned char *data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ data[index]) * 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

// Worker/analyze threads stay on the engine context. Skip per-call
// cuCtxGetCurrent/SetCurrent once this thread has already bound it.
void bind_analysis_context(const DriverApi &api, CUcontext desired) {
    thread_local CUcontext bound = nullptr;
    if (bound == desired) return;
    cuda_detail::cuda_check(api, api.ctx_set_current(desired), "cuCtxSetCurrent");
    bound = desired;
}

class DeviceBuffer {
public:
    DeviceBuffer() = default;
    DeviceBuffer(const DriverApi &api, std::size_t bytes) : api_(&api), bytes_(bytes) {
        if (bytes != 0U) {
            cuda_detail::cuda_check(api, api.mem_alloc(&pointer_, bytes), "cuMemAlloc_v2");
        }
    }
    ~DeviceBuffer() {
        if (pointer_ != 0U && api_ != nullptr) (void)api_->mem_free(pointer_);
    }
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;
    DeviceBuffer(DeviceBuffer &&other) noexcept { swap(other); }
    DeviceBuffer &operator=(DeviceBuffer &&other) noexcept {
        if (this != &other) {
            DeviceBuffer temporary(std::move(other));
            swap(temporary);
        }
        return *this;
    }

    [[nodiscard]] CUdeviceptr pointer() const noexcept { return pointer_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

    void reserve(const DriverApi &api, std::size_t bytes,
                 std::size_t &allocation_count) {
        if (bytes <= bytes_) return;
        reset();
        DeviceBuffer replacement(api, bytes);
        swap(replacement);
        ++allocation_count;
    }

    void reset() noexcept {
        DeviceBuffer released;
        swap(released);
    }

    void swap(DeviceBuffer &other) noexcept {
        std::swap(api_, other.api_);
        std::swap(pointer_, other.pointer_);
        std::swap(bytes_, other.bytes_);
    }

private:
    const DriverApi *api_ = nullptr;
    CUdeviceptr pointer_ = 0U;
    std::size_t bytes_ = 0U;
};

class PinnedBuffer {
public:
    PinnedBuffer() = default;
    ~PinnedBuffer() {
        if (pointer_ != nullptr && api_ != nullptr) (void)api_->mem_free_host(pointer_);
    }
    PinnedBuffer(const PinnedBuffer &) = delete;
    PinnedBuffer &operator=(const PinnedBuffer &) = delete;
    PinnedBuffer(PinnedBuffer &&other) noexcept { swap(other); }
    PinnedBuffer &operator=(PinnedBuffer &&other) noexcept {
        if (this != &other) {
            PinnedBuffer temporary(std::move(other));
            swap(temporary);
        }
        return *this;
    }

    void reserve(const DriverApi &api, std::size_t bytes,
                 std::size_t &allocation_count) {
        if (bytes <= bytes_) return;
        PinnedBuffer replacement;
        replacement.api_ = &api;
        replacement.bytes_ = bytes;
        cuda_detail::cuda_check(
            api,
            api.mem_host_alloc(&replacement.pointer_, bytes, CU_MEMHOSTALLOC_PORTABLE),
            "cuMemHostAlloc");
        swap(replacement);
        ++allocation_count;
    }

    [[nodiscard]] void *data() noexcept { return pointer_; }
    [[nodiscard]] const void *data() const noexcept { return pointer_; }

    void swap(PinnedBuffer &other) noexcept {
        std::swap(api_, other.api_);
        std::swap(pointer_, other.pointer_);
        std::swap(bytes_, other.bytes_);
    }

private:
    const DriverApi *api_ = nullptr;
    void *pointer_ = nullptr;
    std::size_t bytes_ = 0U;
};

class CudaEvent {
public:
    CudaEvent() = default;
    ~CudaEvent() {
        if (event_ != nullptr && api_ != nullptr) (void)api_->event_destroy(event_);
    }
    CudaEvent(const CudaEvent &) = delete;
    CudaEvent &operator=(const CudaEvent &) = delete;
    CudaEvent(CudaEvent &&other) noexcept { swap(other); }
    CudaEvent &operator=(CudaEvent &&other) noexcept {
        if (this != &other) {
            CudaEvent temporary(std::move(other));
            swap(temporary);
        }
        return *this;
    }

    void create(const DriverApi &api) {
        api_ = &api;
        cuda_detail::cuda_check(
            api, api.event_create(&event_, CU_EVENT_DEFAULT), "cuEventCreate");
    }
    void record(CUstream stream) const {
        cuda_detail::cuda_check(
            *api_, api_->event_record(event_, stream), "cuEventRecord");
    }
    [[nodiscard]] CUevent get() const noexcept { return event_; }

    void swap(CudaEvent &other) noexcept {
        std::swap(api_, other.api_);
        std::swap(event_, other.event_);
    }

private:
    const DriverApi *api_ = nullptr;
    CUevent event_ = nullptr;
};

template <class Function>
class ScopeExit {
public:
    explicit ScopeExit(Function function) : function_(std::move(function)) {}
    ~ScopeExit() { function_(); }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    Function function_;
};

void validate_axis_plan(const AxisPlan &plan) {
    if (!plan.valid() || plan.half_bandwidth < 0 || plan.half_bandwidth > 15
        || plan.forward_width > 16) {
        throw std::invalid_argument(
            "CUDA baseline requires a valid plan with half-bandwidth <= 15 and forward width <= 16");
    }
    for (std::size_t row = 0; row < static_cast<std::size_t>(plan.source_size); ++row) {
        const std::size_t begin = row * static_cast<std::size_t>(plan.forward_width);
        const std::int32_t left = plan.forward_indices[begin];
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            const std::int64_t expected = static_cast<std::int64_t>(left) + tap;
            if (expected < 0 || expected >= plan.destination_size
                || plan.forward_indices[begin + static_cast<std::size_t>(tap)]
                    != expected) {
                throw std::invalid_argument("CUDA baseline received an invalid forward plan row");
            }
        }
    }
    if (plan.transpose_offsets.front() != 0U
        || plan.transpose_offsets.back() != plan.transpose_indices.size()) {
        throw std::invalid_argument("CUDA baseline received invalid transpose offsets");
    }
    for (std::size_t index = 1; index < plan.transpose_offsets.size(); ++index) {
        if (plan.transpose_offsets[index] < plan.transpose_offsets[index - 1U]) {
            throw std::invalid_argument("CUDA baseline received non-monotonic transpose offsets");
        }
    }
    for (const std::int32_t index : plan.transpose_indices) {
        if (index < 0 || index >= plan.source_size) {
            throw std::invalid_argument("CUDA baseline received an invalid transpose index");
        }
    }
    const auto finite = [](const auto &values) {
        return std::all_of(values.begin(), values.end(), [](float value) {
            return std::isfinite(value);
        });
    };
    if (!finite(plan.forward_weights) || !finite(plan.transpose_weights)
        || !finite(plan.lower_ld) || !finite(plan.upper_l)
        || !finite(plan.inverse_diagonal)) {
        throw std::invalid_argument("CUDA baseline received non-finite plan coefficients");
    }
}

[[nodiscard]] std::size_t fused_horizontal_span(
    const AxisPlan &plan, std::size_t output_columns_per_block) {
    const std::size_t source_size = static_cast<std::size_t>(plan.source_size);
    const std::size_t width = static_cast<std::size_t>(plan.forward_width);
    std::size_t maximum_span = 0U;
    std::int32_t previous_left = -1;
    for (std::size_t row = 0U; row < source_size; ++row) {
        const std::int32_t left = plan.forward_indices[row * width];
        if (left < previous_left) return 0U;
        previous_left = left;
        const std::size_t last_row = std::min(
            source_size - 1U, row + output_columns_per_block - 1U);
        const std::int32_t last_left = plan.forward_indices[last_row * width];
        const std::size_t span = static_cast<std::size_t>(last_left - left)
            + width;
        maximum_span = std::max(maximum_span, span);
    }
    return maximum_span <= maximum_fused_horizontal_span ? maximum_span : 0U;
}

struct PackedBatch {
    struct Tile {
        std::size_t first_candidate = 0U;
        std::size_t candidate_count = 0U;
        std::size_t workspace_elements = 0U;
        std::size_t maximum_vertical_columns = 0U;
        std::size_t maximum_forward_elements = 0U;
        std::size_t maximum_fused_horizontal_span = 0U;
        bool has_horizontal = false;
        bool has_vertical = false;
        bool has_both = false;
        bool fused_both = true;
    };

    // Byte sizes of the pinned staging image, in upload order. Every packed
    // range is a whole plan array, so the exact sizes are known from a cheap
    // pre-pass over the candidate plans before anything is written.
    struct Layout {
        std::size_t candidates_bytes = 0U;
        std::size_t forward_left_bytes = 0U;
        std::size_t forward_weights_bytes = 0U;
        std::size_t transpose_offsets_bytes = 0U;
        std::size_t transpose_indices_bytes = 0U;
        std::size_t transpose_weights_bytes = 0U;
        std::size_t lower_ld_bytes = 0U;
        std::size_t upper_l_bytes = 0U;
        std::size_t inverse_diagonal_bytes = 0U;
        std::size_t total_bytes = 0U;

        // {offset, bytes} per array, in the fixed upload order used for the
        // H2D copies (candidates first).
        [[nodiscard]] std::array<std::pair<std::size_t, std::size_t>, 9U>
        regions() const {
            const std::array<std::size_t, 9U> bytes = {
                candidates_bytes, forward_left_bytes, forward_weights_bytes,
                transpose_offsets_bytes, transpose_indices_bytes,
                transpose_weights_bytes, lower_ld_bytes, upper_l_bytes,
                inverse_diagonal_bytes};
            std::array<std::pair<std::size_t, std::size_t>, 9U> result{};
            std::size_t offset = 0U;
            for (std::size_t index = 0U; index < bytes.size(); ++index) {
                result[index] = {offset, bytes[index]};
                offset += bytes[index];
            }
            return result;
        }
    };

    std::vector<Tile> tiles;
    Layout layout;
    std::size_t workspace_elements = 0U;
    std::size_t maximum_tile_candidate_count = 0U;
    bool has_horizontal = false;

    void clear_for_reuse() {
        tiles.clear();
        layout = {};
        workspace_elements = 0U;
        maximum_tile_candidate_count = 0U;
        has_horizontal = false;
    }

    [[nodiscard]] static Layout compute_layout(
        std::span<const CandidateAnalysis> batch) {
        Layout layout;
        std::size_t forward_rows = 0U;
        std::size_t forward_weight_elements = 0U;
        std::size_t transpose_offset_elements = 0U;
        std::size_t transpose_elements = 0U;
        std::size_t factor_elements = 0U;
        std::size_t diagonal_elements = 0U;
        for (const CandidateAnalysis &candidate : batch) {
            const AxisPlan *axes[2] = {nullptr, nullptr};
            std::size_t axis_count = 0U;
            if (candidate.axes != AnalysisAxes::vertical && candidate.horizontal) {
                axes[axis_count++] = candidate.horizontal.get();
            }
            if (candidate.axes != AnalysisAxes::horizontal && candidate.vertical) {
                axes[axis_count++] = candidate.vertical.get();
            }
            for (std::size_t index = 0U; index < axis_count; ++index) {
                const AxisPlan &plan = *axes[index];
                forward_rows = checked_add(
                    forward_rows, static_cast<std::size_t>(plan.source_size),
                    "CUDA packed plan");
                forward_weight_elements = checked_add(
                    forward_weight_elements, plan.forward_weights.size(),
                    "CUDA packed plan");
                transpose_offset_elements = checked_add(
                    transpose_offset_elements, plan.transpose_offsets.size(),
                    "CUDA packed plan");
                transpose_elements = checked_add(
                    transpose_elements, plan.transpose_indices.size(),
                    "CUDA packed plan");
                factor_elements = checked_add(
                    factor_elements, plan.lower_ld.size(), "CUDA packed plan");
                diagonal_elements = checked_add(
                    diagonal_elements, plan.inverse_diagonal.size(),
                    "CUDA packed plan");
            }
        }
        layout.candidates_bytes = checked_product(
            batch.size(), sizeof(CandidateDescriptor), "CUDA candidate upload");
        layout.forward_left_bytes = checked_product(
            forward_rows, sizeof(std::int32_t), "CUDA plan upload");
        layout.forward_weights_bytes = checked_product(
            forward_weight_elements, sizeof(float), "CUDA plan upload");
        layout.transpose_offsets_bytes = checked_product(
            transpose_offset_elements, sizeof(std::uint32_t), "CUDA plan upload");
        layout.transpose_indices_bytes = checked_product(
            transpose_elements, sizeof(std::int32_t), "CUDA plan upload");
        layout.transpose_weights_bytes = checked_product(
            transpose_elements, sizeof(float), "CUDA plan upload");
        layout.lower_ld_bytes = checked_product(
            factor_elements, sizeof(float), "CUDA plan upload");
        layout.upper_l_bytes = checked_product(
            factor_elements, sizeof(float), "CUDA plan upload");
        layout.inverse_diagonal_bytes = checked_product(
            diagonal_elements, sizeof(float), "CUDA plan upload");
        std::size_t total = 0U;
        for (const auto &[offset, bytes] : layout.regions()) {
            total = checked_add(total, bytes, "CUDA plan upload");
        }
        if (total > maximum_explicit_bytes) {
            throw std::length_error("CUDA plan upload exceeds 2 GiB");
        }
        layout.total_bytes = total;
        return layout;
    }
};

// Element cursors into the pinned staging arrays; descriptor bases are these
// per-array element offsets, exactly as the old append-to-vector pack
// produced.
struct PackCursors {
    std::size_t forward_left = 0U;
    std::size_t forward_weights = 0U;
    std::size_t transpose_offsets = 0U;
    std::size_t transpose_indices = 0U;
    std::size_t transpose_weights = 0U;
    std::size_t lower_ld = 0U;
    std::size_t upper_l = 0U;
    std::size_t inverse_diagonal = 0U;
};

// Writes `elements` values from `source` into the pinned region at the
// cursor and advances it; returns the base element offset.
template <class Value>
[[nodiscard]] std::uint32_t pack_array_into(
    std::byte *pinned, std::pair<std::size_t, std::size_t> region,
    std::size_t &cursor, const std::vector<Value> &source) {
    const std::size_t bytes = checked_product(
        source.size(), sizeof(Value), "CUDA plan upload");
    if (cursor > region.second / sizeof(Value)
        || source.size() > region.second / sizeof(Value) - cursor) {
        throw std::length_error("CUDA packed plan exceeds the staged region");
    }
    const std::uint32_t base = checked_u32(cursor, "CUDA packed plan offset");
    std::memcpy(
        pinned + region.first + cursor * sizeof(Value), source.data(), bytes);
    cursor += source.size();
    return base;
}

[[nodiscard]] AxisPlanDescriptor pack_axis(
    const AxisPlan &plan, std::byte *pinned,
    const std::array<std::pair<std::size_t, std::size_t>, 9U> &regions,
    PackCursors &cursors) {
    validate_axis_plan(plan);
    AxisPlanDescriptor result;
    result.source_size = static_cast<std::uint32_t>(plan.source_size);
    result.destination_size = static_cast<std::uint32_t>(plan.destination_size);
    result.half_bandwidth = static_cast<std::uint32_t>(plan.half_bandwidth);
    result.forward_width = static_cast<std::uint32_t>(plan.forward_width);
    const std::size_t rows = static_cast<std::size_t>(plan.source_size);
    const std::size_t width = static_cast<std::size_t>(plan.forward_width);
    if (rows > regions[1].second / sizeof(std::int32_t) - cursors.forward_left
        || plan.forward_weights.size()
            > regions[2].second / sizeof(float) - cursors.forward_weights) {
        throw std::length_error("CUDA packed plan exceeds the staged region");
    }
    result.forward_left_base = checked_u32(
        cursors.forward_left, "CUDA packed plan offset");
    result.forward_weights_base = checked_u32(
        cursors.forward_weights, "CUDA packed plan offset");
    // One pass produces both the per-row left edge and the tap-major weight
    // layout: row-outer iteration reads the row-major source once (the old
    // tap-outer loop re-read every cache line forward_width times) while the
    // scattered writes land in forward_width <= 16 concurrently-hot output
    // lines, which fit L1 (16 x 64 B = 1 KiB).
    auto *const left_out = reinterpret_cast<std::int32_t *>(
        pinned + regions[1].first) + cursors.forward_left;
    auto *const weights_out = reinterpret_cast<float *>(
        pinned + regions[2].first) + cursors.forward_weights;
    for (std::size_t row = 0U; row < rows; ++row) {
        left_out[row] = plan.forward_indices[row * width];
        const float *const row_weights =
            plan.forward_weights.data() + row * width;
        for (std::size_t tap = 0U; tap < width; ++tap) {
            weights_out[tap * rows + row] = row_weights[tap];
        }
    }
    cursors.forward_left += rows;
    cursors.forward_weights += plan.forward_weights.size();

    result.transpose_offsets_base = pack_array_into(
        pinned, regions[3], cursors.transpose_offsets, plan.transpose_offsets);
    result.transpose_indices_base = pack_array_into(
        pinned, regions[4], cursors.transpose_indices, plan.transpose_indices);
    result.transpose_weights_base = pack_array_into(
        pinned, regions[5], cursors.transpose_weights, plan.transpose_weights);
    result.lower_ld_base = pack_array_into(
        pinned, regions[6], cursors.lower_ld, plan.lower_ld);
    result.upper_l_base = pack_array_into(
        pinned, regions[7], cursors.upper_l, plan.upper_l);
    result.inverse_diagonal_base = pack_array_into(
        pinned, regions[8], cursors.inverse_diagonal, plan.inverse_diagonal);
    return result;
}

[[nodiscard]] std::uint32_t axes_code(AnalysisAxes axes) {
    switch (axes) {
    case AnalysisAxes::horizontal: return cuda_baseline::horizontal_axes;
    case AnalysisAxes::vertical: return cuda_baseline::vertical_axes;
    case AnalysisAxes::both: return cuda_baseline::both_axes;
    }
    throw std::invalid_argument("CUDA candidate has an invalid axes value");
}

// Fills the pinned staging image; `pinned` must already hold
// packed.layout.total_bytes (the caller reserves it between the layout
// pre-pass and this call).
void pack_batch(
    PackedBatch &packed, std::byte *pinned, ConstImageView source,
    std::span<const CandidateAnalysis> candidates,
    std::size_t workspace_limit_elements, std::size_t pixel_threads) {
    const auto regions = packed.layout.regions();
    PackCursors cursors;
    auto *const pinned_candidates =
        reinterpret_cast<CandidateDescriptor *>(pinned);
    const std::size_t hard_limit = maximum_explicit_bytes / sizeof(float);
    const std::size_t default_limit =
        cuda_detail::cuda_default_workspace_bytes / sizeof(float);
    const std::size_t limit = workspace_limit_elements == 0U
        ? std::min(default_limit, hard_limit)
        : std::min(workspace_limit_elements, hard_limit);
    PackedBatch::Tile tile;
    tile.first_candidate = 0U;

    for (std::size_t candidate_index = 0U;
         candidate_index < candidates.size(); ++candidate_index) {
        const CandidateAnalysis &candidate = candidates[candidate_index];
        CandidateDescriptor descriptor;
        descriptor.axes = axes_code(candidate.axes);
        std::size_t intermediate = 0U;
        std::size_t native = 0U;
        std::size_t candidate_fused_span = 0U;

        if (candidate.axes == AnalysisAxes::horizontal
            || candidate.axes == AnalysisAxes::both) {
            if (!candidate.horizontal || candidate.horizontal->source_size != source.width) {
                throw std::invalid_argument("CUDA candidate has no matching horizontal plan");
            }
            descriptor.horizontal = pack_axis(
                *candidate.horizontal, pinned, regions, cursors);
            intermediate = checked_product(
                static_cast<std::size_t>(source.height),
                static_cast<std::size_t>(candidate.horizontal->destination_size),
                "CUDA horizontal intermediate");
        }
        if (candidate.axes == AnalysisAxes::vertical
            || candidate.axes == AnalysisAxes::both) {
            if (!candidate.vertical || candidate.vertical->source_size != source.height) {
                throw std::invalid_argument("CUDA candidate has no matching vertical plan");
            }
            descriptor.vertical = pack_axis(
                *candidate.vertical, pinned, regions, cursors);
            native = checked_product(
                static_cast<std::size_t>(candidate.vertical->destination_size),
                candidate.axes == AnalysisAxes::both
                    ? static_cast<std::size_t>(candidate.horizontal->destination_size)
                    : static_cast<std::size_t>(source.width),
                "CUDA native image");
        }
        if (candidate.axes == AnalysisAxes::both) {
            const std::size_t horizontal_first = checked_product(
                static_cast<std::size_t>(source.width),
                static_cast<std::size_t>(candidate.vertical->destination_size),
                "CUDA forward intermediate");
            intermediate = std::max(intermediate, horizontal_first);
            candidate_fused_span = fused_horizontal_span(
                *candidate.horizontal, pixel_threads * 8U);
            if (candidate_fused_span != 0U) {
                descriptor.forward_order = cuda_baseline::vertical_first;
            } else {
                descriptor.forward_order =
                    select_forward_order(*candidate.horizontal, *candidate.vertical)
                        == ForwardOrder::horizontal_first
                    ? cuda_baseline::horizontal_first
                    : cuda_baseline::vertical_first;
            }
        }
        const std::size_t total = checked_add(
            intermediate, native, "CUDA candidate workspace");
        if (total > limit) {
            throw std::length_error("CUDA staged candidate exceeds the workspace limit");
        }
        const bool tile_full = tile.candidate_count != 0U
            && (tile.candidate_count >= maximum_tile_candidates
                || total > limit - tile.workspace_elements);
        if (tile_full) {
            packed.workspace_elements = std::max(
                packed.workspace_elements, tile.workspace_elements);
            packed.maximum_tile_candidate_count = std::max(
                packed.maximum_tile_candidate_count, tile.candidate_count);
            packed.tiles.push_back(tile);
            tile = {};
            tile.first_candidate = candidate_index;
        }

        descriptor.workspace_base = checked_u32(
            tile.workspace_elements, "CUDA tile workspace offset");
        descriptor.intermediate_elements = checked_u32(
            intermediate, "CUDA candidate intermediate");
        descriptor.native_elements = checked_u32(
            native, "CUDA candidate native image");
        tile.workspace_elements = checked_add(
            tile.workspace_elements, total, "CUDA tile workspace");
        ++tile.candidate_count;
        tile.has_horizontal = tile.has_horizontal
            || candidate.axes != AnalysisAxes::vertical;
        packed.has_horizontal = packed.has_horizontal
            || candidate.axes != AnalysisAxes::vertical;
        tile.has_vertical = tile.has_vertical
            || candidate.axes != AnalysisAxes::horizontal;
        tile.has_both = tile.has_both || candidate.axes == AnalysisAxes::both;
        tile.fused_both = tile.fused_both
            && candidate.axes == AnalysisAxes::both
            && candidate_fused_span != 0U;
        tile.maximum_fused_horizontal_span = std::max(
            tile.maximum_fused_horizontal_span, candidate_fused_span);
        if (candidate.axes != AnalysisAxes::horizontal) {
            tile.maximum_vertical_columns = std::max(
                tile.maximum_vertical_columns,
                candidate.axes == AnalysisAxes::vertical
                    ? static_cast<std::size_t>(source.width)
                    : static_cast<std::size_t>(candidate.horizontal->destination_size));
        }
        if (candidate.axes == AnalysisAxes::both) {
            const std::size_t forward_elements = descriptor.forward_order
                    == cuda_baseline::vertical_first
                ? checked_product(
                    static_cast<std::size_t>(source.height),
                    static_cast<std::size_t>(candidate.horizontal->destination_size),
                    "CUDA vertical-first forward intermediate")
                : checked_product(
                    static_cast<std::size_t>(source.width),
                    static_cast<std::size_t>(candidate.vertical->destination_size),
                    "CUDA horizontal-first forward intermediate");
            tile.maximum_forward_elements = std::max(
                tile.maximum_forward_elements, forward_elements);
        }
        pinned_candidates[candidate_index] = descriptor;
    }

    if (tile.candidate_count != 0U) {
        packed.workspace_elements = std::max(
            packed.workspace_elements, tile.workspace_elements);
        packed.maximum_tile_candidate_count = std::max(
            packed.maximum_tile_candidate_count, tile.candidate_count);
        packed.tiles.push_back(tile);
    }

    // The pre-pass is exact: cursors must land precisely on region ends.
    if (cursors.forward_left * sizeof(std::int32_t)
                != packed.layout.forward_left_bytes
            || cursors.forward_weights * sizeof(float)
                != packed.layout.forward_weights_bytes
            || cursors.transpose_offsets * sizeof(std::uint32_t)
                != packed.layout.transpose_offsets_bytes
            || cursors.transpose_indices * sizeof(std::int32_t)
                != packed.layout.transpose_indices_bytes
            || cursors.transpose_weights * sizeof(float)
                != packed.layout.transpose_weights_bytes
            || cursors.lower_ld * sizeof(float) != packed.layout.lower_ld_bytes
            || cursors.upper_l * sizeof(float) != packed.layout.upper_l_bytes
            || cursors.inverse_diagonal * sizeof(float)
                != packed.layout.inverse_diagonal_bytes) {
        throw std::runtime_error("CUDA staged plan byte accounting mismatch");
    }
}

void validate_source_and_metric(ConstImageView source, const MetricSpec &metric) {
    if (source.data == nullptr || source.width <= 0 || source.height <= 0
        || source.stride < source.width) {
        throw std::invalid_argument("invalid CUDA source image");
    }
    const std::int64_t horizontal_crop = static_cast<std::int64_t>(metric.crop_left)
        + static_cast<std::int64_t>(metric.crop_right);
    const std::int64_t vertical_crop = static_cast<std::int64_t>(metric.crop_top)
        + static_cast<std::int64_t>(metric.crop_bottom);
    if (metric.crop_left < 0 || metric.crop_right < 0 || metric.crop_top < 0
        || metric.crop_bottom < 0
        || horizontal_crop >= source.width || vertical_crop >= source.height
        || !std::isfinite(metric.threshold) || metric.threshold < 0.0F) {
        throw std::invalid_argument("invalid CUDA metric configuration");
    }
    if (source.height > 1
        && source.stride > std::numeric_limits<std::ptrdiff_t>::max()
            / static_cast<std::ptrdiff_t>(source.height - 1)) {
        throw std::length_error("CUDA source stride overflows host pointer arithmetic");
    }
    if (metric.norm < cuda_minimum_p_norm || metric.norm > cuda_maximum_p_norm) {
        throw std::invalid_argument("CUDA p-norm must be in [1, 4]");
    }
}

struct CudaLumaLayout {
    std::uint32_t storage_bytes = 0U;
    std::uint32_t storage_shift = 0U;
};

[[nodiscard]] CudaLumaLayout validate_cuda_luma(
    const CudaLumaFrameView &source) {
    if (source.device_pointer == 0U || source.context == 0U
        || source.width <= 0 || source.height <= 0
        || source.bit_depth < 8 || source.bit_depth > 16) {
        throw std::invalid_argument("invalid CUDA luma frame");
    }

    CudaLumaLayout layout;
    switch (source.format) {
    case CudaLumaFormat::nv12:
    case CudaLumaFormat::yuv444p8:
        if (source.bit_depth != 8) {
            throw std::invalid_argument("8-bit CUDA luma format requires bit_depth=8");
        }
        layout.storage_bytes = 1U;
        break;
    case CudaLumaFormat::p010:
        if (source.bit_depth != 10) {
            throw std::invalid_argument("P010 CUDA luma requires bit_depth=10");
        }
        layout.storage_bytes = 2U;
        layout.storage_shift = 6U;
        break;
    case CudaLumaFormat::p016:
        layout.storage_bytes = 2U;
        layout.storage_shift = static_cast<std::uint32_t>(16 - source.bit_depth);
        break;
    case CudaLumaFormat::yuv444p16:
        layout.storage_bytes = 2U;
        break;
    }

    const std::size_t row_bytes = checked_product(
        static_cast<std::size_t>(source.width), layout.storage_bytes,
        "CUDA luma row");
    if (source.pitch_bytes < row_bytes) {
        throw std::invalid_argument("CUDA luma pitch is smaller than one row");
    }
    return layout;
}

struct BatchIdentity {
    struct Candidate {
        AnalysisAxes axes = AnalysisAxes::both;
        std::shared_ptr<const AxisPlan> horizontal;
        std::shared_ptr<const AxisPlan> vertical;
    };

    std::int32_t width = 0;
    std::int32_t height = 0;
    std::size_t workspace_limit_elements = 0U;
    std::vector<Candidate> candidates;

    [[nodiscard]] bool matches(
        ConstImageView source,
        std::span<const CandidateAnalysis> requested_candidates,
        std::size_t requested_workspace_limit) const noexcept {
        if (width != source.width || height != source.height
            || workspace_limit_elements != requested_workspace_limit
            || candidates.size() != requested_candidates.size()) {
            return false;
        }
        for (std::size_t index = 0U; index < candidates.size(); ++index) {
            if (candidates[index].axes != requested_candidates[index].axes
                || candidates[index].horizontal.get()
                    != requested_candidates[index].horizontal.get()
                || candidates[index].vertical.get()
                    != requested_candidates[index].vertical.get()) {
                return false;
            }
        }
        return true;
    }

    void assign(ConstImageView source,
                std::span<const CandidateAnalysis> requested_candidates,
                std::size_t requested_workspace_limit) {
        width = source.width;
        height = source.height;
        workspace_limit_elements = requested_workspace_limit;
        candidates.clear();
        candidates.reserve(requested_candidates.size());
        for (const CandidateAnalysis &candidate : requested_candidates) {
            candidates.push_back({
                candidate.axes, candidate.horizontal, candidate.vertical,
            });
        }
    }
};

// Identity of a resident device source frame. The pointer and geometry are
// checked along with a 16-sample content probe so a recycled host address
// with different content cannot produce a stale hit. Callers that pin a
// frame for the session (the worker's frame cache) get cross-call uploads
// and transposes for free.
struct SourceIdentity {
    const float *data = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::ptrdiff_t stride = 0;
    std::uint64_t probe = 0;

    [[nodiscard]] bool matches(ConstImageView source, std::uint64_t value) const noexcept {
        return data == source.data && width == source.width
            && height == source.height && stride == source.stride
            && probe == value;
    }

    void assign(ConstImageView source, std::uint64_t value) noexcept {
        data = source.data;
        width = source.width;
        height = source.height;
        stride = source.stride;
        probe = value;
    }
};

struct SharedSourceEntry {
    SourceIdentity identity;
    PinnedBuffer staging;
    DeviceBuffer source;
    DeviceBuffer transposed;
    std::size_t retained_bytes = 0U;
    std::uint64_t last_use = 0U;
    bool transposed_ready = false;
};

[[nodiscard]] std::uint64_t source_probe(ConstImageView source) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    constexpr std::size_t sample_count = 16U;
    for (std::size_t index = 0U; index < sample_count; ++index) {
        const std::int32_t row = static_cast<std::int32_t>(
            (index * static_cast<std::size_t>(source.height)) / sample_count);
        const std::int32_t column = static_cast<std::int32_t>(
            (index * static_cast<std::size_t>(source.width)) / sample_count);
        const float value =
            source.data[static_cast<std::ptrdiff_t>(row) * source.stride + column];
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>(bits >> shift);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

struct ExecutionSlot {
    explicit ExecutionSlot(const DriverApi &api) : api(&api) {
        cuda_detail::cuda_check(
            api, api.stream_create(&stream, CU_STREAM_NON_BLOCKING),
            "cuStreamCreate");
        try {
            for (CudaEvent &event : events) event.create(api);
            producer_ready.create(api);
        } catch (...) {
            (void)api.stream_destroy(stream);
            stream = nullptr;
            throw;
        }
    }

    ~ExecutionSlot() {
        if (stream != nullptr && api != nullptr) {
            (void)api->stream_synchronize(stream);
            (void)api->stream_destroy(stream);
        }
    }

    ExecutionSlot(const ExecutionSlot &) = delete;
    ExecutionSlot &operator=(const ExecutionSlot &) = delete;

    // Drops the large per-batch buffers (plan arrays, workspace, partials)
    // while keeping source residency: the source and its transpose are small
    // (2 x source bytes), generic across batches, and expensive to rebuild
    // (H2D + transpose kernel) when a budget reset evicts them.
    void reset_plan_buffers() noexcept {
        device_candidates.reset();
        device_forward_left.reset();
        device_forward_weights.reset();
        device_transpose_offsets.reset();
        device_transpose_indices.reset();
        device_transpose_weights.reset();
        device_lower_ld.reset();
        device_upper_l.reset();
        device_inverse_diagonal.reset();
        device_workspace.reset();
        device_partials.reset();
        device_results.reset();
        plan_ready = false;
    }

    const DriverApi *api = nullptr;
    CUstream stream = nullptr;
    std::array<CudaEvent, 10U> events;
    CudaEvent producer_ready;
    PinnedBuffer pinned_source;
    PinnedBuffer pinned_results;
    PinnedBuffer pinned_plan;
    DeviceBuffer device_source;
    DeviceBuffer device_transposed_source;
    DeviceBuffer device_candidates;
    DeviceBuffer device_forward_left;
    DeviceBuffer device_forward_weights;
    DeviceBuffer device_transpose_offsets;
    DeviceBuffer device_transpose_indices;
    DeviceBuffer device_transpose_weights;
    DeviceBuffer device_lower_ld;
    DeviceBuffer device_upper_l;
    DeviceBuffer device_inverse_diagonal;
    DeviceBuffer device_workspace;
    DeviceBuffer device_partials;
    DeviceBuffer device_results;
    BatchIdentity identity;
    PackedBatch packed;
    bool plan_ready = false;
    SourceIdentity source_identity;
    bool source_ready = false;
    bool transposed_source_ready = false;
};

struct HostPackedPlanEntry {
    BatchIdentity identity;
    PackedBatch packed;
    std::vector<std::byte> image;
    std::uint64_t last_use = 0U;
};

[[nodiscard]] double elapsed_ms(
    const DriverApi &api, const CudaEvent &begin, const CudaEvent &end) {
    float milliseconds = 0.0F;
    cuda_detail::cuda_check(
        api, api.event_elapsed_time(&milliseconds, begin.get(), end.get()),
        "cuEventElapsedTime");
    return static_cast<double>(milliseconds);
}

void update_maximum(std::atomic<std::size_t> &destination,
                    std::size_t candidate) noexcept {
    std::size_t current = destination.load(std::memory_order_relaxed);
    while (current < candidate
           && !destination.compare_exchange_weak(
               current, candidate, std::memory_order_relaxed)) {}
}

void merge_telemetry(CudaRuntimeTelemetry &destination,
                     const CudaRuntimeTelemetry &source) {
    destination.kernel_launch_count += source.kernel_launch_count;
    destination.analyzed_candidate_count += source.analyzed_candidate_count;
    destination.tile_count += source.tile_count;
    destination.buffer_allocation_count += source.buffer_allocation_count;
    destination.plan_cache_hits += source.plan_cache_hits;
    destination.plan_cache_misses += source.plan_cache_misses;
    destination.host_plan_cache_hits += source.host_plan_cache_hits;
    destination.host_plan_cache_misses += source.host_plan_cache_misses;
    destination.host_plan_cache_bytes = source.host_plan_cache_bytes;
    destination.source_cache_hits += source.source_cache_hits;
    destination.source_cache_misses += source.source_cache_misses;
    destination.source_upload_bytes += source.source_upload_bytes;
    destination.source_upload_count += source.source_upload_count;
    destination.source_conversion_bytes += source.source_conversion_bytes;
    destination.source_conversion_count += source.source_conversion_count;
    destination.source_transpose_count += source.source_transpose_count;
    destination.plan_upload_bytes += source.plan_upload_bytes;
    destination.result_readback_bytes += source.result_readback_bytes;
    destination.workspace_bytes = std::max(
        destination.workspace_bytes, source.workspace_bytes);
    destination.pinned_staging_bytes = std::max(
        destination.pinned_staging_bytes, source.pinned_staging_bytes);
    destination.peak_workspace_elements = std::max(
        destination.peak_workspace_elements, source.peak_workspace_elements);
    destination.execution_slot_wait_ms += source.execution_slot_wait_ms;
    destination.host_pack_ms += source.host_pack_ms;
    destination.source_staging_ms += source.source_staging_ms;
    destination.source_upload_ms += source.source_upload_ms;
    destination.source_conversion_ms += source.source_conversion_ms;
    destination.plan_upload_ms += source.plan_upload_ms;
    destination.source_transpose_ms += source.source_transpose_ms;
    destination.horizontal_fused_ms += source.horizontal_fused_ms;
    destination.inverse_horizontal_ms += source.inverse_horizontal_ms;
    destination.inverse_vertical_ms += source.inverse_vertical_ms;
    destination.forward_intermediate_ms += source.forward_intermediate_ms;
    destination.metric_ms += source.metric_ms;
    destination.kernel_ms += source.kernel_ms;
    destination.result_readback_ms += source.result_readback_ms;
    destination.gpu_total_ms += source.gpu_total_ms;
}

} // namespace

struct CudaAnalysisEngine::Impl {
    explicit Impl(CudaAnalysisOptions requested_options)
        : options(std::move(requested_options)) {
        if (options.device_uuid.empty() && options.device_ordinal < 0) {
            throw std::invalid_argument("CUDA device ordinal must not be negative");
        }
        if (options.kernel_variant != CudaKernelVariant::automatic
            && options.kernel_variant != CudaKernelVariant::cpp_generic) {
            throw std::runtime_error(
                "requested CUDA kernel variant has not passed the cpp-generic baseline gate");
        }
        if (options.execution_slots == 0U || options.execution_slots > 8U) {
            throw std::invalid_argument("CUDA execution_slots must be in [1, 8]");
        }
        const CudaLaunchPolicy &policy = options.launch_policy;
        if (!is_power_of_two(policy.inverse_threads)
            || policy.inverse_threads < 32U || policy.inverse_threads > 256U) {
            throw std::invalid_argument(
                "CUDA inverse_threads must be a power of two in [32, 256]");
        }
        if (!is_power_of_two(policy.pixel_threads)
            || policy.pixel_threads < 32U || policy.pixel_threads > 256U) {
            throw std::invalid_argument(
                "CUDA pixel_threads must be a power of two in [32, 256]");
        }
        if (policy.maximum_metric_blocks == 0U
            || policy.maximum_metric_blocks > 1024U) {
            throw std::invalid_argument(
                "CUDA maximum_metric_blocks must be in [1, 1024]");
        }

        api = cuda_detail::load_cuda_driver();
        cuda_detail::cuda_check(*api, api->init(0U), "cuInit");
        const auto devices = enumerate_devices(*api);
        const auto selected = std::find_if(
            devices.begin(), devices.end(), [&](const CudaDeviceInfo &candidate) {
                return options.device_uuid.empty()
                    ? candidate.ordinal == options.device_ordinal
                    : candidate.uuid == options.device_uuid;
            });
        if (selected == devices.end()) {
            throw std::runtime_error(options.device_uuid.empty()
                ? "CUDA device ordinal was not found"
                : "CUDA device UUID was not found");
        }
        info = *selected;
        if (!info.backend_compatible) {
            throw std::runtime_error(
                "CUDA device is incompatible with this backend: "
                + info.incompatibility_reason);
        }
        if (policy.inverse_threads
                > static_cast<std::uint32_t>(info.maximum_threads_per_block)
            || policy.pixel_threads
                > static_cast<std::uint32_t>(info.maximum_threads_per_block)) {
            throw std::runtime_error(
                "CUDA launch policy exceeds the device thread-block limit");
        }
        const std::size_t inverse_shared_bytes = checked_product(
            static_cast<std::size_t>(policy.inverse_threads),
            33U * sizeof(float), "CUDA inverse shared memory");
        if (inverse_shared_bytes
            > static_cast<std::size_t>(info.shared_memory_per_block_bytes)) {
            throw std::runtime_error(
                "CUDA launch policy exceeds the device shared-memory limit");
        }

        CUdevice device = 0;
        cuda_detail::cuda_check(
            *api, api->device_get(&device, info.ordinal), "cuDeviceGet(selected)");
        CUcontext previous = nullptr;
        cuda_detail::cuda_check(*api, api->ctx_get_current(&previous), "cuCtxGetCurrent");
        try {
            cuda_detail::cuda_check(
                *api,
                api->ctx_create(&context, CU_CTX_SCHED_AUTO, device),
                "cuCtxCreate_v2");
            cuda_detail::cuda_check(
                *api,
                api->module_load_data(&module, getnative_cuda_staged_fatbin),
                "cuModuleLoadData");
            const auto load_function = [&](CUfunction &function, const char *name) {
                cuda_detail::cuda_check(
                    *api, api->module_get_function(&function, module, name),
                    "cuModuleGetFunction(staged)");
            };
            load_function(inverse_horizontal_function,
                          "getnative_cuda_inverse_horizontal");
            load_function(horizontal_fused_function,
                          "getnative_cuda_horizontal_fused");
            load_function(transpose_source_function,
                          "getnative_cuda_transpose_source");
            load_function(luma_to_f32_function,
                          "getnative_cuda_luma_to_f32");
            load_function(inverse_vertical_function,
                          "getnative_cuda_inverse_vertical");
            load_function(inverse_vertical_pair_function,
                          "getnative_cuda_inverse_vertical_pair");
            load_function(forward_intermediate_function,
                          "getnative_cuda_forward_intermediate");
            load_function(both_fused_metric_function,
                          "getnative_cuda_both_fused_metric");
            load_function(metric_partials_function,
                          "getnative_cuda_metric_partials");
            load_function(metric_finalize_function,
                          "getnative_cuda_metric_finalize");
            const auto record_resources = [&](const char *name,
                                              CUfunction function) {
                CudaKernelResourceInfo resource;
                resource.name = name;
                const auto query = [&](CUfunction_attribute attribute,
                                       const char *operation) {
                    int value = 0;
                    cuda_detail::cuda_check(
                        *api, api->func_get_attribute(&value, attribute, function),
                        operation);
                    return value;
                };
                resource.register_count = query(
                    CU_FUNC_ATTRIBUTE_NUM_REGS,
                    "cuFuncGetAttribute(NUM_REGS)");
                resource.static_shared_bytes = query(
                    CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
                    "cuFuncGetAttribute(SHARED_SIZE_BYTES)");
                resource.local_bytes = query(
                    CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES,
                    "cuFuncGetAttribute(LOCAL_SIZE_BYTES)");
                resource.constant_bytes = query(
                    CU_FUNC_ATTRIBUTE_CONST_SIZE_BYTES,
                    "cuFuncGetAttribute(CONST_SIZE_BYTES)");
                resource.binary_version = query(
                    CU_FUNC_ATTRIBUTE_BINARY_VERSION,
                    "cuFuncGetAttribute(BINARY_VERSION)");
                resource.ptx_version = query(
                    CU_FUNC_ATTRIBUTE_PTX_VERSION,
                    "cuFuncGetAttribute(PTX_VERSION)");
                telemetry.kernel_resources.push_back(std::move(resource));
            };
            telemetry.kernel_resources.reserve(10U);
            record_resources(
                "getnative_cuda_luma_to_f32", luma_to_f32_function);
            record_resources(
                "getnative_cuda_inverse_horizontal", inverse_horizontal_function);
            record_resources(
                "getnative_cuda_horizontal_fused", horizontal_fused_function);
            record_resources(
                "getnative_cuda_transpose_source", transpose_source_function);
            record_resources(
                "getnative_cuda_inverse_vertical", inverse_vertical_function);
            record_resources(
                "getnative_cuda_inverse_vertical_pair",
                inverse_vertical_pair_function);
            record_resources(
                "getnative_cuda_forward_intermediate",
                forward_intermediate_function);
            record_resources(
                "getnative_cuda_both_fused_metric", both_fused_metric_function);
            record_resources(
                "getnative_cuda_metric_partials", metric_partials_function);
            record_resources(
                "getnative_cuda_metric_finalize", metric_finalize_function);
            std::size_t free_memory_bytes = 0U;
            std::size_t total_memory_bytes = 0U;
            cuda_detail::cuda_check(
                *api,
                api->mem_get_info(&free_memory_bytes, &total_memory_bytes),
                "cuMemGetInfo_v2");
            memory_budget = cuda_detail::make_cuda_memory_budget(
                free_memory_bytes, options.execution_slots,
                options.workspace_limit_elements);
            if (memory_budget.workspace_limit_bytes < sizeof(float)) {
                throw std::runtime_error(
                    "CUDA device memory budget cannot provide a workspace element");
            }
            effective_workspace_limit_elements =
                memory_budget.workspace_limit_bytes / sizeof(float);
            host_plan_cache_budget = environment_byte_budget(
                "GETNATIVE_CUDA_HOST_PLAN_CACHE_BYTES",
                default_host_plan_cache_bytes);
            input_cache_budget = environment_byte_budget(
                "GETNATIVE_CUDA_INPUT_CACHE_BYTES",
                default_input_cache_bytes);
            cuda_detail::cuda_check(
                *api, api->stream_create(&decode_stream, CU_STREAM_NON_BLOCKING),
                "cuStreamCreate(decode)");
            cuda_detail::cuda_check(
                *api, api->stream_create(&input_upload_stream, CU_STREAM_NON_BLOCKING),
                "cuStreamCreate(input cache)");
            slots.reserve(options.execution_slots);
            for (std::size_t index = 0U; index < options.execution_slots; ++index) {
                slots.push_back(std::make_unique<ExecutionSlot>(*api));
            }
            slot_busy.assign(options.execution_slots, false);
            cuda_detail::cuda_check(*api, api->ctx_set_current(previous), "cuCtxSetCurrent");
        } catch (...) {
            slots.clear();
            if (input_upload_stream != nullptr) {
                (void)api->stream_destroy(input_upload_stream);
                input_upload_stream = nullptr;
            }
            if (decode_stream != nullptr) {
                (void)api->stream_destroy(decode_stream);
                decode_stream = nullptr;
            }
            if (module != nullptr) (void)api->module_unload(module);
            if (context != nullptr) (void)api->ctx_destroy(context);
            context = nullptr;
            module = nullptr;
            (void)api->ctx_set_current(previous);
            throw;
        }

        telemetry.initial_device_free_bytes = memory_budget.initial_free_bytes;
        telemetry.device_memory_reserve_bytes = memory_budget.reserve_bytes;
        telemetry.device_memory_budget_bytes = memory_budget.engine_budget_bytes;
        telemetry.per_slot_memory_budget_bytes = memory_budget.per_slot_budget_bytes;
        telemetry.effective_workspace_limit_bytes =
            memory_budget.workspace_limit_bytes;
        telemetry.workspace_limit_clamped = memory_budget.workspace_limit_clamped;
        telemetry.artifact_target = GETNATIVE_CUDA_ARTIFACT_TARGET;
        telemetry.artifact_name = "getnative_cuda_staged.fatbin";
        telemetry.artifact_hash_fnv1a64 = fnv1a64_hex(
            getnative_cuda_staged_fatbin, getnative_cuda_staged_fatbin_size);
    }

    ~Impl() {
        if (context == nullptr || api == nullptr) return;
        CUcontext previous = nullptr;
        if (api->ctx_get_current(&previous) == CUDA_SUCCESS
            && api->ctx_set_current(context) == CUDA_SUCCESS) {
            slots.clear();
            if (input_upload_stream != nullptr) {
                (void)api->stream_synchronize(input_upload_stream);
                input_cache.clear();
                (void)api->stream_destroy(input_upload_stream);
                input_upload_stream = nullptr;
            }
            if (decode_stream != nullptr) {
                (void)api->stream_synchronize(decode_stream);
                (void)api->stream_destroy(decode_stream);
                decode_stream = nullptr;
            }
            if (module != nullptr) (void)api->module_unload(module);
            (void)api->ctx_set_current(previous);
        }
        (void)api->ctx_destroy(context);
    }

    [[nodiscard]] std::size_t acquire_slot(
        std::stop_token stop, ConstImageView source,
        std::span<const CandidateAnalysis> candidates,
        std::size_t workspace_limit_elements) {
        std::unique_lock lock(slot_mutex);
        for (;;) {
            for (std::size_t index = 0U; index < slots.size(); ++index) {
                if (!slot_busy[index] && slots[index]->plan_ready
                    && slots[index]->identity.matches(
                        source, candidates, workspace_limit_elements)) {
                    slot_busy[index] = true;
                    return index;
                }
            }
            const auto available = std::find(slot_busy.begin(), slot_busy.end(), false);
            if (available != slot_busy.end()) {
                *available = true;
                return static_cast<std::size_t>(available - slot_busy.begin());
            }
            if (stop.stop_requested()) {
                throw std::runtime_error("CUDA analysis cancelled while waiting for a slot");
            }
            slot_available.wait_for(lock, std::chrono::milliseconds(2));
        }
    }

    void release_slot(std::size_t index) noexcept {
        {
            const std::scoped_lock lock(slot_mutex);
            slot_busy[index] = false;
        }
        slot_available.notify_one();
    }

    CudaAnalysisOptions options;
    std::shared_ptr<DriverApi> api;
    CudaDeviceInfo info;
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUfunction inverse_horizontal_function = nullptr;
    CUfunction horizontal_fused_function = nullptr;
    CUfunction transpose_source_function = nullptr;
    CUfunction luma_to_f32_function = nullptr;
    CUfunction inverse_vertical_function = nullptr;
    CUfunction inverse_vertical_pair_function = nullptr;
    CUfunction forward_intermediate_function = nullptr;
    CUfunction both_fused_metric_function = nullptr;
    CUfunction metric_partials_function = nullptr;
    CUfunction metric_finalize_function = nullptr;
    std::vector<std::unique_ptr<ExecutionSlot>> slots;
    std::vector<bool> slot_busy;
    std::mutex slot_mutex;
    std::condition_variable slot_available;
    CUstream input_upload_stream = nullptr;
    CUstream decode_stream = nullptr;
    std::mutex input_cache_mutex;
    std::vector<std::shared_ptr<SharedSourceEntry>> input_cache;
    std::size_t input_cache_budget = 0U;
    std::size_t input_cache_bytes = 0U;
    std::uint64_t input_cache_clock = 0U;
    std::mutex host_plan_cache_mutex;
    std::vector<HostPackedPlanEntry> host_plan_cache;
    std::size_t host_plan_cache_budget = 0U;
    std::size_t host_plan_cache_bytes = 0U;
    std::uint64_t host_plan_cache_clock = 0U;
    mutable std::mutex telemetry_mutex;
    CudaRuntimeTelemetry telemetry;
    cuda_detail::CudaMemoryBudget memory_budget;
    std::size_t effective_workspace_limit_elements = 0U;
    std::atomic<std::size_t> peak_workspace_elements{0U};
    std::atomic<std::size_t> peak_working_set_bytes{0U};
};

CudaRuntimeProbe cuda_runtime_probe() noexcept {
    CudaRuntimeProbe result;
    try {
        const auto api = cuda_detail::load_cuda_driver();
        result.driver_loaded = true;
        cuda_detail::cuda_check(*api, api->init(0U), "cuInit");
        result.initialized = true;
        result.devices = enumerate_devices(*api);
        result.device_available = std::any_of(
            result.devices.begin(), result.devices.end(),
            [](const CudaDeviceInfo &device) {
                return device.backend_compatible;
            });
        if (!result.device_available) {
            result.reason = result.devices.empty()
                ? "no CUDA device is available"
                : "no CUDA device meets the minimum compute capability sm_"
                    + std::to_string(minimum_architecture);
        }
    } catch (const std::exception &error) {
        result.reason = error.what();
    } catch (...) {
        result.reason = "unknown CUDA runtime probe failure";
    }
    return result;
}

bool cuda_backend_available() noexcept { return cuda_runtime_probe().device_available; }

std::vector<CudaDeviceInfo> enumerate_cuda_devices() {
    const auto api = cuda_detail::load_cuda_driver();
    cuda_detail::cuda_check(*api, api->init(0U), "cuInit");
    return enumerate_devices(*api);
}

std::int32_t cuda_minimum_compute_capability() noexcept {
    return minimum_architecture;
}

std::string_view cuda_compiled_artifact_target() noexcept {
    return GETNATIVE_CUDA_ARTIFACT_TARGET;
}

CudaAnalysisEngine::CudaAnalysisEngine(CudaAnalysisOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CudaAnalysisEngine::~CudaAnalysisEngine() = default;
CudaAnalysisEngine::CudaAnalysisEngine(CudaAnalysisEngine &&) noexcept = default;
CudaAnalysisEngine &CudaAnalysisEngine::operator=(CudaAnalysisEngine &&) noexcept = default;

const CudaDeviceInfo &CudaAnalysisEngine::device_info() const noexcept {
    return impl_->info;
}

const CudaAnalysisOptions &CudaAnalysisEngine::options() const noexcept {
    return impl_->options;
}

std::size_t CudaAnalysisEngine::peak_workspace_elements() const noexcept {
    return impl_->peak_workspace_elements.load(std::memory_order_relaxed);
}

std::size_t CudaAnalysisEngine::peak_working_set_bytes() const noexcept {
    return impl_->peak_working_set_bytes.load(std::memory_order_relaxed);
}

CudaRuntimeTelemetry CudaAnalysisEngine::runtime_telemetry() const {
    const std::scoped_lock lock(impl_->telemetry_mutex);
    return impl_->telemetry;
}

void CudaAnalysisEngine::reset_analysis_telemetry() {
    const std::scoped_lock lock(impl_->telemetry_mutex);
    const std::string artifact_name = impl_->telemetry.artifact_name;
    const std::string artifact_hash = impl_->telemetry.artifact_hash_fnv1a64;
    const std::string artifact_target = impl_->telemetry.artifact_target;
    std::vector<CudaKernelResourceInfo> kernel_resources =
        impl_->telemetry.kernel_resources;
    impl_->telemetry = {};
    impl_->telemetry.artifact_target = artifact_target;
    impl_->telemetry.artifact_name = artifact_name;
    impl_->telemetry.artifact_hash_fnv1a64 = artifact_hash;
    impl_->telemetry.kernel_resources = std::move(kernel_resources);
    impl_->telemetry.initial_device_free_bytes =
        impl_->memory_budget.initial_free_bytes;
    impl_->telemetry.device_memory_reserve_bytes =
        impl_->memory_budget.reserve_bytes;
    impl_->telemetry.device_memory_budget_bytes =
        impl_->memory_budget.engine_budget_bytes;
    impl_->telemetry.per_slot_memory_budget_bytes =
        impl_->memory_budget.per_slot_budget_bytes;
    impl_->telemetry.effective_workspace_limit_bytes =
        impl_->memory_budget.workspace_limit_bytes;
    impl_->telemetry.workspace_limit_clamped =
        impl_->memory_budget.workspace_limit_clamped;
}

std::uintptr_t CudaAnalysisEngine::native_context() const noexcept {
    return reinterpret_cast<std::uintptr_t>(impl_->context);
}

std::uintptr_t CudaAnalysisEngine::native_decode_stream() const noexcept {
    return reinterpret_cast<std::uintptr_t>(impl_->decode_stream);
}

void CudaAnalysisEngine::preflight_axis_batch(
    ConstImageView dimensions,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::size_t concurrency) const {
    validate_source_and_metric(dimensions, metric);
    if (concurrency == 0U || concurrency > impl_->options.execution_slots) {
        throw std::length_error(
            "CUDA execution slots cannot satisfy the requested concurrency");
    }
    if (candidates.empty()) return;

    PackedBatch packed;
    packed.layout = PackedBatch::compute_layout(candidates);
    std::vector<std::byte> staged(packed.layout.total_bytes);
    pack_batch(
        packed, staged.data(), dimensions, candidates,
        impl_->effective_workspace_limit_elements,
        impl_->options.launch_policy.pixel_threads);

    const std::size_t source_bytes = checked_product(
        checked_product(
            static_cast<std::size_t>(dimensions.width),
            static_cast<std::size_t>(dimensions.height), "CUDA source image"),
        sizeof(float), "CUDA source buffer");
    const std::size_t workspace_bytes = checked_product(
        packed.workspace_elements, sizeof(float), "CUDA workspace buffer");
    const std::size_t crop_width = static_cast<std::size_t>(
        dimensions.width - metric.crop_left - metric.crop_right);
    const std::size_t crop_height = static_cast<std::size_t>(
        dimensions.height - metric.crop_top - metric.crop_bottom);
    const std::size_t pixel_threads = impl_->options.launch_policy.pixel_threads;
    const std::size_t metric_blocks = std::min<std::size_t>(
        impl_->options.launch_policy.maximum_metric_blocks,
        (checked_product(crop_width, crop_height, "CUDA metric pixels")
         + pixel_threads - 1U) / pixel_threads);
    const std::size_t fused_column_blocks =
        (crop_width + pixel_threads * 8U - 1U) / (pixel_threads * 8U);
    const std::size_t fused_partial_stride = checked_product(
        fused_column_blocks, crop_height,
        "CUDA fused metric partial stride");
    const bool fused_grid_supported =
        crop_height <= 65535U
        && fused_column_blocks <= std::numeric_limits<unsigned int>::max()
        && fused_partial_stride <= std::numeric_limits<std::uint32_t>::max();
    std::size_t partial_count = 0U;
    for (const PackedBatch::Tile &tile : packed.tiles) {
        const bool horizontal_only = tile.has_horizontal && !tile.has_vertical;
        const bool fused_both = tile.fused_both && fused_grid_supported;
        const std::size_t stride = horizontal_only
            ? static_cast<std::size_t>(dimensions.height)
            : (fused_both ? fused_partial_stride : metric_blocks);
        partial_count = std::max(
            partial_count,
            checked_product(
                tile.candidate_count, stride, "CUDA metric partials"));
    }
    const std::size_t working_set = checked_add(
        checked_add(
            packed.has_horizontal
                ? checked_add(source_bytes, source_bytes, "CUDA source buffers")
                : source_bytes,
            workspace_bytes, "CUDA working set"),
        checked_add(
            packed.layout.total_bytes,
            checked_add(
                checked_product(
                    partial_count, sizeof(double), "CUDA partial buffer"),
                checked_product(
                    packed.maximum_tile_candidate_count, sizeof(double),
                    "CUDA result buffer"),
                "CUDA working set"),
            "CUDA working set"),
        "CUDA working set");
    const std::size_t aggregate = checked_product(
        working_set, concurrency, "CUDA concurrent working set");
    if (working_set > maximum_explicit_bytes
        || working_set > impl_->memory_budget.per_slot_budget_bytes
        || aggregate > impl_->memory_budget.engine_budget_bytes) {
        throw std::length_error(
            "CUDA device memory cannot satisfy the requested concurrency");
    }
}

std::vector<CandidateResult> CudaAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop, GpuStageProfile profile) {
    return analyze_axis_batch_impl(
        source, nullptr, candidates, metric, stop, profile);
}

std::vector<CandidateResult> CudaAnalysisEngine::analyze_axis_batch_cuda_luma(
    const CudaLumaFrameView &source,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop, GpuStageProfile profile) {
    (void)validate_cuda_luma(source);
    const ConstImageView dimensions{
        reinterpret_cast<const float *>(std::uintptr_t{1}),
        source.width,
        source.height,
        source.width,
    };
    return analyze_axis_batch_impl(
        dimensions, &source, candidates, metric, stop, profile);
}

std::vector<CandidateResult> CudaAnalysisEngine::analyze_axis_batch_impl(
    ConstImageView source, const CudaLumaFrameView *cuda_luma,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop, GpuStageProfile profile) {
    validate_source_and_metric(source, metric);
    const bool cuda_device_source = cuda_luma != nullptr;
    const CudaLumaLayout luma_layout = cuda_device_source
        ? validate_cuda_luma(*cuda_luma) : CudaLumaLayout{};
    if (candidates.empty()) return {};
    if (stop.stop_requested()) throw std::runtime_error("CUDA analysis cancelled");

    const CudaLaunchPolicy &launch_policy = impl_->options.launch_policy;
    const unsigned int inverse_threads = launch_policy.inverse_threads;
    const unsigned int pixel_threads = launch_policy.pixel_threads;
    const unsigned int maximum_metric_blocks =
        launch_policy.maximum_metric_blocks;
    const std::size_t fused_output_columns_per_block = pixel_threads * 8U;

    CudaRuntimeTelemetry delta;
    const auto wait_start = std::chrono::steady_clock::now();
    const std::size_t slot_index = impl_->acquire_slot(
        stop, source, candidates, impl_->effective_workspace_limit_elements);
    delta.execution_slot_wait_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wait_start).count();
    bind_analysis_context(*impl_->api, impl_->context);
    ExecutionSlot &slot = *impl_->slots[slot_index];
    std::shared_ptr<SharedSourceEntry> shared_source;
    const bool instrument = profile == GpuStageProfile::stages;
    bool stream_synced = false;
    ScopeExit release_slot{[&] {
        if (!stream_synced) {
            (void)impl_->api->stream_synchronize(slot.stream);
        }
        shared_source.reset();
        impl_->release_slot(slot_index);
    }};
    if (cuda_device_source) {
        const CUcontext frame_context = reinterpret_cast<CUcontext>(
            cuda_luma->context);
        if (frame_context != impl_->context) {
            throw std::invalid_argument(
                "CUDA luma frame belongs to a different context");
        }
        if (instrument) {
            CUcontext pointer_context = nullptr;
            cuda_detail::cuda_check(
                *impl_->api,
                impl_->api->pointer_get_attribute(
                    &pointer_context, CU_POINTER_ATTRIBUTE_CONTEXT,
                    static_cast<CUdeviceptr>(cuda_luma->device_pointer)),
                "cuPointerGetAttribute(CONTEXT)");
            if (pointer_context != impl_->context) {
                throw std::invalid_argument(
                    "CUDA luma pointer belongs to a different context");
            }
        }
    }
    const auto pack_start = std::chrono::steady_clock::now();
    std::size_t allocation_count = 0U;
    bool plan_cache_hit = slot.plan_ready && slot.identity.matches(
        source, candidates, impl_->effective_workspace_limit_elements);
    if (!plan_cache_hit) {
        slot.plan_ready = false;
        const std::scoped_lock cache_lock(impl_->host_plan_cache_mutex);
        auto cached = std::find_if(
            impl_->host_plan_cache.begin(), impl_->host_plan_cache.end(),
            [&](const HostPackedPlanEntry &entry) {
                return entry.identity.matches(
                    source, candidates, impl_->effective_workspace_limit_elements);
            });
        if (cached != impl_->host_plan_cache.end()) {
            cached->last_use = ++impl_->host_plan_cache_clock;
            slot.packed = cached->packed;
            slot.identity = cached->identity;
            slot.pinned_plan.reserve(
                *impl_->api, cached->image.size(), allocation_count);
            std::memcpy(slot.pinned_plan.data(), cached->image.data(),
                        cached->image.size());
            delta.host_plan_cache_hits = 1U;
        } else {
            slot.packed.clear_for_reuse();
            slot.packed.layout = PackedBatch::compute_layout(candidates);
            // The pack writes straight into pinned staging, so the pinned buffer
            // must be sized before pack_batch runs.
            slot.pinned_plan.reserve(
                *impl_->api, slot.packed.layout.total_bytes, allocation_count);
            pack_batch(
                slot.packed, static_cast<std::byte *>(slot.pinned_plan.data()),
                source, candidates, impl_->effective_workspace_limit_elements,
                pixel_threads);
            slot.identity.assign(
                source, candidates, impl_->effective_workspace_limit_elements);
            delta.host_plan_cache_misses = 1U;
            const std::size_t bytes = slot.packed.layout.total_bytes;
            if (bytes <= impl_->host_plan_cache_budget) {
                while (!impl_->host_plan_cache.empty()
                       && impl_->host_plan_cache_bytes
                               > impl_->host_plan_cache_budget - bytes) {
                    const auto victim = std::min_element(
                        impl_->host_plan_cache.begin(), impl_->host_plan_cache.end(),
                        [](const HostPackedPlanEntry &left,
                           const HostPackedPlanEntry &right) {
                            return left.last_use < right.last_use;
                        });
                    impl_->host_plan_cache_bytes -= victim->image.size();
                    impl_->host_plan_cache.erase(victim);
                }
                HostPackedPlanEntry entry;
                entry.identity = slot.identity;
                entry.packed = slot.packed;
                entry.image.resize(bytes);
                std::memcpy(entry.image.data(), slot.pinned_plan.data(), bytes);
                entry.last_use = ++impl_->host_plan_cache_clock;
                impl_->host_plan_cache_bytes += bytes;
                impl_->host_plan_cache.push_back(std::move(entry));
            }
        }
        delta.host_plan_cache_bytes = impl_->host_plan_cache_bytes;
        delta.plan_cache_misses = 1U;
    } else {
        delta.plan_cache_hits = 1U;
    }
    delta.host_pack_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - pack_start).count();
    PackedBatch &packed = slot.packed;

    const std::size_t source_elements = checked_product(
        static_cast<std::size_t>(source.width),
        static_cast<std::size_t>(source.height), "CUDA source image");
    const std::size_t source_bytes = checked_product(
        source_elements, sizeof(float), "CUDA source buffer");
    if (source_bytes > maximum_explicit_bytes) {
        throw std::length_error("CUDA explicit working set exceeds 2 GiB");
    }
    const std::size_t shared_source_bytes = packed.has_horizontal
        ? checked_add(source_bytes, source_bytes, "CUDA cached transposed source")
        : source_bytes;
    const bool use_shared_source = !cuda_device_source
        && shared_source_bytes <= impl_->input_cache_budget;
    const std::size_t workspace_bytes = checked_product(
        packed.workspace_elements, sizeof(float), "CUDA workspace buffer");
    const std::size_t crop_width = static_cast<std::size_t>(
        source.width - metric.crop_left - metric.crop_right);
    const std::size_t crop_height = static_cast<std::size_t>(
        source.height - metric.crop_top - metric.crop_bottom);
    const std::size_t metric_pixel_count = checked_product(
        crop_width, crop_height, "CUDA metric pixels");
    const std::size_t metric_blocks = std::min<std::size_t>(
        maximum_metric_blocks,
        (metric_pixel_count + pixel_threads - 1U) / pixel_threads);
    const std::size_t fused_column_blocks =
        (crop_width + fused_output_columns_per_block - 1U)
        / fused_output_columns_per_block;
    const std::size_t fused_partial_stride = checked_product(
        fused_column_blocks, crop_height, "CUDA fused metric partial stride");
    const bool fused_grid_supported =
        crop_height <= 65535U
        && fused_column_blocks <= std::numeric_limits<unsigned int>::max()
        && fused_partial_stride <= std::numeric_limits<std::uint32_t>::max();
    std::size_t partial_count = 0U;
    for (const PackedBatch::Tile &tile : packed.tiles) {
        const bool horizontal_only = tile.has_horizontal && !tile.has_vertical;
        const bool fused_both = tile.fused_both && fused_grid_supported;
        const std::size_t partial_stride = horizontal_only
            ? static_cast<std::size_t>(source.height)
            : (fused_both ? fused_partial_stride : metric_blocks);
        partial_count = std::max(
            partial_count,
            checked_product(
                tile.candidate_count, partial_stride,
                "CUDA tile metric partials"));
    }
    const std::size_t partial_bytes = checked_product(
        partial_count, sizeof(double), "CUDA metric partial buffer");
    const std::size_t device_result_bytes = checked_product(
        packed.maximum_tile_candidate_count, sizeof(double),
        "CUDA tile result buffer");
    const std::size_t total_result_bytes = checked_product(
        candidates.size(), sizeof(double), "CUDA result readback");
    const std::size_t source_working_bytes = packed.has_horizontal
        ? checked_add(source_bytes, source_bytes, "CUDA transposed source")
        : source_bytes;
    const std::size_t working_set = checked_add(
        checked_add(source_working_bytes, workspace_bytes, "CUDA working set"),
        checked_add(
            packed.layout.total_bytes,
            checked_add(partial_bytes, device_result_bytes, "CUDA working set"),
            "CUDA working set"),
        "CUDA working set");
    if (working_set > maximum_explicit_bytes) {
        throw std::length_error("CUDA explicit working set exceeds 2 GiB");
    }
    if (working_set > impl_->memory_budget.per_slot_budget_bytes) {
        throw std::length_error(
            "CUDA working set exceeds the adaptive per-slot device memory budget");
    }

    std::size_t projected_device_bytes = 0U;
    const auto include_capacity = [&](const DeviceBuffer &buffer,
                                      std::size_t required_bytes) {
        projected_device_bytes = checked_add(
            projected_device_bytes, std::max(buffer.bytes(), required_bytes),
            "CUDA retained device capacity");
    };
    include_capacity(slot.device_source, use_shared_source ? 0U : source_bytes);
    include_capacity(
        slot.device_transposed_source,
        use_shared_source ? 0U : (packed.has_horizontal ? source_bytes : 0U));
    include_capacity(slot.device_workspace, workspace_bytes);
    include_capacity(slot.device_partials, partial_bytes);
    include_capacity(slot.device_results, device_result_bytes);
    include_capacity(slot.device_candidates, packed.layout.candidates_bytes);
    include_capacity(
        slot.device_forward_left, packed.layout.forward_left_bytes);
    include_capacity(
        slot.device_forward_weights, packed.layout.forward_weights_bytes);
    include_capacity(
        slot.device_transpose_offsets, packed.layout.transpose_offsets_bytes);
    include_capacity(
        slot.device_transpose_indices, packed.layout.transpose_indices_bytes);
    include_capacity(
        slot.device_transpose_weights, packed.layout.transpose_weights_bytes);
    include_capacity(slot.device_lower_ld, packed.layout.lower_ld_bytes);
    include_capacity(slot.device_upper_l, packed.layout.upper_l_bytes);
    include_capacity(
        slot.device_inverse_diagonal, packed.layout.inverse_diagonal_bytes);
    const std::size_t retained_capacity_limit = std::min(
        maximum_explicit_bytes, impl_->memory_budget.per_slot_budget_bytes);
    if (projected_device_bytes > retained_capacity_limit) {
        slot.reset_plan_buffers();
        if (plan_cache_hit) {
            plan_cache_hit = false;
            delta.plan_cache_hits = 0U;
            delta.plan_cache_misses = 1U;
        }
    }

    if (!use_shared_source && !cuda_device_source) {
        slot.pinned_source.reserve(
            *impl_->api, source_bytes, allocation_count);
    }
    slot.pinned_results.reserve(
        *impl_->api, device_result_bytes, allocation_count);
    if (!plan_cache_hit) {
        // No-op on the miss path (already reserved before pack_batch); this
        // covers the budget-reset path that flips a hit to a miss below the
        // pack phase, where the retained pinned image is re-uploaded.
        slot.pinned_plan.reserve(
            *impl_->api, packed.layout.total_bytes, allocation_count);
    }
    if (!use_shared_source) {
        slot.device_source.reserve(
            *impl_->api, source_bytes, allocation_count);
    }
    if (packed.has_horizontal && !use_shared_source) {
        slot.device_transposed_source.reserve(
            *impl_->api, source_bytes, allocation_count);
    }
    slot.device_workspace.reserve(
        *impl_->api, workspace_bytes, allocation_count);
    slot.device_partials.reserve(
        *impl_->api, partial_bytes, allocation_count);
    slot.device_results.reserve(
        *impl_->api, device_result_bytes, allocation_count);
    slot.device_candidates.reserve(
        *impl_->api, packed.layout.candidates_bytes,
        allocation_count);
    slot.device_forward_left.reserve(
        *impl_->api, packed.layout.forward_left_bytes,
        allocation_count);
    slot.device_forward_weights.reserve(
        *impl_->api, packed.layout.forward_weights_bytes,
        allocation_count);
    slot.device_transpose_offsets.reserve(
        *impl_->api, packed.layout.transpose_offsets_bytes,
        allocation_count);
    slot.device_transpose_indices.reserve(
        *impl_->api, packed.layout.transpose_indices_bytes,
        allocation_count);
    slot.device_transpose_weights.reserve(
        *impl_->api, packed.layout.transpose_weights_bytes,
        allocation_count);
    slot.device_lower_ld.reserve(
        *impl_->api, packed.layout.lower_ld_bytes, allocation_count);
    slot.device_upper_l.reserve(
        *impl_->api, packed.layout.upper_l_bytes, allocation_count);
    slot.device_inverse_diagonal.reserve(
        *impl_->api, packed.layout.inverse_diagonal_bytes,
        allocation_count);
    delta.buffer_allocation_count = allocation_count;

    const std::uint64_t probe = cuda_device_source ? 0U : source_probe(source);
    bool source_cache_hit = false;
    double shared_upload_ms = 0.0;
    double shared_transpose_ms = 0.0;
    const auto stage_source = [&](float *staged_source) {
        if (source.stride == source.width) {
            std::memcpy(staged_source, source.data, source_bytes);
        } else {
            const std::size_t row_bytes = checked_product(
                static_cast<std::size_t>(source.width), sizeof(float),
                "CUDA source row");
            for (std::int32_t row = 0; row < source.height; ++row) {
                std::memcpy(
                    staged_source + static_cast<std::ptrdiff_t>(row) * source.width,
                    source.data + static_cast<std::ptrdiff_t>(row) * source.stride,
                    row_bytes);
            }
        }
    };

    if (use_shared_source) {
        const std::scoped_lock cache_lock(impl_->input_cache_mutex);
        auto found = std::find_if(
            impl_->input_cache.begin(), impl_->input_cache.end(),
            [&](const std::shared_ptr<SharedSourceEntry> &entry) {
                return entry->identity.matches(source, probe);
            });
        if (found != impl_->input_cache.end()) {
            shared_source = *found;
            shared_source->last_use = ++impl_->input_cache_clock;
            source_cache_hit = true;
            delta.source_cache_hits = 1U;
            if (packed.has_horizontal && !shared_source->transposed_ready) {
                while (!impl_->input_cache.empty()
                       && impl_->input_cache_bytes
                               > impl_->input_cache_budget - source_bytes) {
                    const auto victim = std::min_element(
                        impl_->input_cache.begin(), impl_->input_cache.end(),
                        [&](const auto &left, const auto &right) {
                            if (left == shared_source) return false;
                            if (right == shared_source) return true;
                            if (left.use_count() != 1U) return false;
                            if (right.use_count() != 1U) return true;
                            return left->last_use < right->last_use;
                        });
                    if (victim == impl_->input_cache.end()
                        || *victim == shared_source || victim->use_count() != 1U) {
                        break;
                    }
                    impl_->input_cache_bytes -= (*victim)->retained_bytes;
                    impl_->input_cache.erase(victim);
                }
                if (impl_->input_cache_bytes
                        > impl_->input_cache_budget - source_bytes) {
                    shared_source.reset();
                    delta.source_cache_hits = 0U;
                } else {
                    shared_source->transposed.reserve(
                        *impl_->api, source_bytes, allocation_count);
                    CUdeviceptr source_pointer = shared_source->source.pointer();
                    CUdeviceptr transposed_pointer = shared_source->transposed.pointer();
                    std::uint32_t width = static_cast<std::uint32_t>(source.width);
                    std::uint32_t height = static_cast<std::uint32_t>(source.height);
                    void *parameters[] = {
                        &source_pointer, &width, &height, &transposed_pointer,
                    };
                    const auto transpose_start = std::chrono::steady_clock::now();
                    cuda_detail::cuda_check(
                        *impl_->api,
                        impl_->api->launch_kernel(
                            impl_->transpose_source_function,
                            (width + 31U) / 32U, (height + 31U) / 32U, 1U,
                            32U, 8U, 1U, 0U, impl_->input_upload_stream,
                            parameters, nullptr),
                        "cuLaunchKernel(getnative_cuda_transpose_source shared)");
                    cuda_detail::cuda_check(
                        *impl_->api,
                        impl_->api->stream_synchronize(impl_->input_upload_stream),
                        "cuStreamSynchronize(shared transpose)");
                    shared_transpose_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - transpose_start).count();
                    shared_source->transposed_ready = true;
                    shared_source->retained_bytes += source_bytes;
                    impl_->input_cache_bytes += source_bytes;
                    ++delta.source_transpose_count;
                }
            }
        } else {
            while (!impl_->input_cache.empty()
                   && impl_->input_cache_bytes
                           > impl_->input_cache_budget - shared_source_bytes) {
                const auto victim = std::min_element(
                    impl_->input_cache.begin(), impl_->input_cache.end(),
                    [](const auto &left, const auto &right) {
                        if (left.use_count() != 1U) return false;
                        if (right.use_count() != 1U) return true;
                        return left->last_use < right->last_use;
                    });
                if (victim == impl_->input_cache.end() || victim->use_count() != 1U) {
                    break;
                }
                impl_->input_cache_bytes -= (*victim)->retained_bytes;
                impl_->input_cache.erase(victim);
            }
            if (impl_->input_cache_bytes
                    <= impl_->input_cache_budget - shared_source_bytes) {
                shared_source = std::make_shared<SharedSourceEntry>();
                shared_source->staging.reserve(
                    *impl_->api, source_bytes, allocation_count);
                shared_source->source.reserve(
                    *impl_->api, source_bytes, allocation_count);
                if (packed.has_horizontal) {
                    shared_source->transposed.reserve(
                        *impl_->api, source_bytes, allocation_count);
                }
                const auto staging_start = std::chrono::steady_clock::now();
                stage_source(static_cast<float *>(shared_source->staging.data()));
                delta.source_staging_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - staging_start).count();
                const auto upload_start = std::chrono::steady_clock::now();
                cuda_detail::cuda_check(
                    *impl_->api,
                    impl_->api->memcpy_htod_async(
                        shared_source->source.pointer(), shared_source->staging.data(),
                        source_bytes, impl_->input_upload_stream),
                    "cuMemcpyHtoDAsync_v2(shared source)");
                if (packed.has_horizontal) {
                    CUdeviceptr source_pointer = shared_source->source.pointer();
                    CUdeviceptr transposed_pointer = shared_source->transposed.pointer();
                    std::uint32_t width = static_cast<std::uint32_t>(source.width);
                    std::uint32_t height = static_cast<std::uint32_t>(source.height);
                    void *parameters[] = {
                        &source_pointer, &width, &height, &transposed_pointer,
                    };
                    const auto transpose_start = std::chrono::steady_clock::now();
                    cuda_detail::cuda_check(
                        *impl_->api,
                        impl_->api->launch_kernel(
                            impl_->transpose_source_function,
                            (width + 31U) / 32U, (height + 31U) / 32U, 1U,
                            32U, 8U, 1U, 0U, impl_->input_upload_stream,
                            parameters, nullptr),
                        "cuLaunchKernel(getnative_cuda_transpose_source shared)");
                    shared_transpose_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - transpose_start).count();
                    shared_source->transposed_ready = true;
                    ++delta.source_transpose_count;
                }
                cuda_detail::cuda_check(
                    *impl_->api,
                    impl_->api->stream_synchronize(impl_->input_upload_stream),
                    "cuStreamSynchronize(shared source)");
                shared_upload_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - upload_start).count();
                shared_source->identity.assign(source, probe);
                shared_source->retained_bytes = shared_source_bytes;
                shared_source->last_use = ++impl_->input_cache_clock;
                impl_->input_cache_bytes += shared_source_bytes;
                impl_->input_cache.push_back(shared_source);
                delta.source_cache_misses = 1U;
                delta.source_upload_bytes = source_bytes;
                delta.source_upload_count = 1U;
            }
        }
    }

    if (!shared_source && !cuda_device_source) {
        source_cache_hit = slot.source_ready && slot.source_identity.matches(source, probe);
        if (source_cache_hit) {
            delta.source_cache_hits = 1U;
        } else {
            delta.source_cache_misses = 1U;
        }
        const auto staging_start = std::chrono::steady_clock::now();
        if (!source_cache_hit) {
            stage_source(static_cast<float *>(slot.pinned_source.data()));
        }
        delta.source_staging_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - staging_start).count();
    }

    if (cuda_device_source && cuda_luma->producer_stream != 0U) {
        slot.producer_ready.record(reinterpret_cast<CUstream>(
            cuda_luma->producer_stream));
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->stream_wait_event(
                slot.stream, slot.producer_ready.get(), 0U),
            "cuStreamWaitEvent(decoder frame)");
    }
    const auto record_stage = [&](std::size_t index) {
        if (instrument) slot.events[index].record(slot.stream);
    };
    record_stage(0);
    if (cuda_device_source) {
        CUdeviceptr luma_pointer = static_cast<CUdeviceptr>(
            cuda_luma->device_pointer);
        auto pitch = static_cast<unsigned long long>(
            cuda_luma->pitch_bytes);
        std::uint32_t conversion_width = static_cast<std::uint32_t>(
            source.width);
        std::uint32_t conversion_height = static_cast<std::uint32_t>(
            source.height);
        std::uint32_t bit_depth = static_cast<std::uint32_t>(
            cuda_luma->bit_depth);
        std::uint32_t storage_bytes = luma_layout.storage_bytes;
        std::uint32_t storage_shift = luma_layout.storage_shift;
        std::uint32_t limited_range =
            cuda_luma->range == CudaColorRange::limited ? 1U : 0U;
        CUdeviceptr output_pointer = slot.device_source.pointer();
        void *parameters[] = {
            &luma_pointer, &pitch, &conversion_width, &conversion_height,
            &storage_bytes, &bit_depth, &storage_shift,
            &limited_range, &output_pointer,
        };
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->launch_kernel(
                impl_->luma_to_f32_function,
                (conversion_width + 31U) / 32U,
                (conversion_height + 7U) / 8U, 1U,
                32U, 8U, 1U, 0U, slot.stream, parameters, nullptr),
            "cuLaunchKernel(getnative_cuda_luma_to_f32)");
        ++delta.kernel_launch_count;
        delta.source_conversion_bytes = source_bytes;
        delta.source_conversion_count = 1U;
        slot.source_ready = false;
        slot.transposed_source_ready = false;
    } else if (!shared_source && !source_cache_hit) {
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->memcpy_htod_async(
                slot.device_source.pointer(), slot.pinned_source.data(),
                source_bytes, slot.stream),
            "cuMemcpyHtoDAsync_v2(source)");
        slot.source_identity.assign(source, probe);
        slot.source_ready = true;
        slot.transposed_source_ready = false;
    }
    record_stage(1);
    if (!plan_cache_hit) {
        // The pack wrote the staging image directly into pinned memory, so
        // the upload is nine straight H2D copies out of the layout regions —
        // no host-side staging copy.
        const auto *plan_staging =
            static_cast<const std::byte *>(slot.pinned_plan.data());
        const auto regions = packed.layout.regions();
        DeviceBuffer *const targets[9] = {
            &slot.device_candidates, &slot.device_forward_left,
            &slot.device_forward_weights, &slot.device_transpose_offsets,
            &slot.device_transpose_indices, &slot.device_transpose_weights,
            &slot.device_lower_ld, &slot.device_upper_l,
            &slot.device_inverse_diagonal};
        for (std::size_t index = 0U; index < 9U; ++index) {
            const auto [offset, bytes] = regions[index];
            if (bytes == 0U) continue;
            cuda_detail::cuda_check(
                *impl_->api,
                impl_->api->memcpy_htod_async(
                    targets[index]->pointer(), plan_staging + offset,
                    bytes, slot.stream),
                "cuMemcpyHtoDAsync_v2(plan)");
        }
        delta.plan_upload_bytes = packed.layout.total_bytes;
    }
    record_stage(2);

    update_maximum(impl_->peak_workspace_elements, packed.workspace_elements);
    update_maximum(impl_->peak_working_set_bytes, working_set);
    delta.workspace_bytes = workspace_bytes;
    delta.pinned_staging_bytes = checked_add(
        checked_add(
            shared_source || cuda_device_source ? 0U : source_bytes,
            device_result_bytes,
                    "CUDA pinned staging"),
        packed.layout.total_bytes, "CUDA pinned staging");
    delta.peak_workspace_elements = packed.workspace_elements;
    if (!shared_source && !cuda_device_source) {
        delta.source_upload_bytes = source_cache_hit ? 0U : source_bytes;
        delta.source_upload_count = source_cache_hit ? 0U : 1U;
    }
    delta.result_readback_bytes = total_result_bytes;

    CUdeviceptr source_pointer = shared_source
        ? shared_source->source.pointer() : slot.device_source.pointer();
    CUdeviceptr transposed_source_pointer = shared_source
        ? shared_source->transposed.pointer() : slot.device_transposed_source.pointer();
    std::uint32_t width = static_cast<std::uint32_t>(source.width);
    std::uint32_t height = static_cast<std::uint32_t>(source.height);
    CUdeviceptr forward_left_pointer = slot.device_forward_left.pointer();
    CUdeviceptr forward_weights_pointer = slot.device_forward_weights.pointer();
    CUdeviceptr transpose_offsets_pointer = slot.device_transpose_offsets.pointer();
    CUdeviceptr transpose_indices_pointer = slot.device_transpose_indices.pointer();
    CUdeviceptr transpose_weights_pointer = slot.device_transpose_weights.pointer();
    CUdeviceptr lower_ld_pointer = slot.device_lower_ld.pointer();
    CUdeviceptr upper_l_pointer = slot.device_upper_l.pointer();
    CUdeviceptr inverse_diagonal_pointer = slot.device_inverse_diagonal.pointer();
    CUdeviceptr workspace_pointer = slot.device_workspace.pointer();
    CUdeviceptr partials_pointer = slot.device_partials.pointer();
    CUdeviceptr results_pointer = slot.device_results.pointer();
    std::uint32_t crop_left = static_cast<std::uint32_t>(metric.crop_left);
    std::uint32_t crop_right = static_cast<std::uint32_t>(metric.crop_right);
    std::uint32_t crop_top = static_cast<std::uint32_t>(metric.crop_top);
    std::uint32_t crop_bottom = static_cast<std::uint32_t>(metric.crop_bottom);
    std::uint32_t norm = metric.norm;
    float threshold = metric.threshold;
    std::uint32_t metric_block_count = checked_u32(
        metric_blocks, "CUDA metric block count");
    std::uint32_t fused_metric_block_count = checked_u32(
        fused_partial_stride, "CUDA fused metric block count");
    const unsigned int fused_grid_x = static_cast<unsigned int>(
        fused_column_blocks);
    const unsigned int fused_grid_z = static_cast<unsigned int>(crop_height);
    std::uint32_t metric_pixels = checked_u32(
        metric_pixel_count, "CUDA metric pixel count");

    std::vector<double> errors(candidates.size());
    const auto launch = [&](CUfunction function,
                            unsigned int grid_x, unsigned int grid_y,
                            unsigned int grid_z,
                            unsigned int block_x, unsigned int block_y,
                            unsigned int shared_bytes,
                            void **parameters, const char *operation) {
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->launch_kernel(
                function, grid_x, grid_y, grid_z, block_x, block_y, 1U,
                shared_bytes, slot.stream, parameters, nullptr),
            operation);
        ++delta.kernel_launch_count;
    };

    if (!shared_source && packed.has_horizontal && !slot.transposed_source_ready) {
        void *parameters[] = {
            &source_pointer, &width, &height, &transposed_source_pointer,
        };
        launch(
            impl_->transpose_source_function,
            (width + 31U) / 32U, (height + 31U) / 32U,
            1U, 32U, 8U, 0U, parameters,
            "cuLaunchKernel(getnative_cuda_transpose_source)");
        slot.transposed_source_ready = true;
        ++delta.source_transpose_count;
    }
    record_stage(3);

    for (const PackedBatch::Tile &tile : packed.tiles) {
        std::uint32_t tile_candidate_count = checked_u32(
            tile.candidate_count, "CUDA tile candidate count");
        const bool horizontal_only = tile.has_horizontal && !tile.has_vertical;
        const bool fused_both = tile.fused_both && fused_grid_supported;
        std::uint32_t tile_metric_block_count = horizontal_only
            ? height : (fused_both ? fused_metric_block_count : metric_block_count);
        CUdeviceptr candidates_pointer = slot.device_candidates.pointer()
            + tile.first_candidate * sizeof(CandidateDescriptor);

        record_stage(4);
        if (tile.has_horizontal) {
            const unsigned int blocks = static_cast<unsigned int>(
                (static_cast<std::size_t>(height) + inverse_threads - 1U)
                / inverse_threads);
            if (horizontal_only) {
                void *parameters[] = {
                    &transposed_source_pointer, &height,
                    &candidates_pointer, &tile_candidate_count,
                    &forward_left_pointer, &forward_weights_pointer,
                    &transpose_offsets_pointer, &transpose_indices_pointer,
                    &transpose_weights_pointer, &lower_ld_pointer,
                    &upper_l_pointer, &inverse_diagonal_pointer,
                    &workspace_pointer,
                    &crop_left, &crop_right, &crop_top, &crop_bottom, &norm,
                    &threshold, &partials_pointer,
                };
                launch(
                    impl_->horizontal_fused_function,
                    blocks, tile_candidate_count, 1U,
                    inverse_threads, 1U, 0U,
                    parameters,
                    "cuLaunchKernel(getnative_cuda_horizontal_fused)");
            } else {
                void *parameters[] = {
                    &transposed_source_pointer, &height,
                    &candidates_pointer, &tile_candidate_count,
                    &transpose_offsets_pointer, &transpose_indices_pointer,
                    &transpose_weights_pointer, &lower_ld_pointer,
                    &upper_l_pointer, &inverse_diagonal_pointer,
                    &workspace_pointer,
                };
                launch(
                    impl_->inverse_horizontal_function,
                    blocks, tile_candidate_count, 1U,
                    inverse_threads, 1U,
                    inverse_threads * 33U
                        * static_cast<unsigned int>(sizeof(float)),
                    parameters,
                    "cuLaunchKernel(getnative_cuda_inverse_horizontal)");
            }
        }
        record_stage(5);
        if (tile.has_vertical) {
            const bool paired_vertical =
                !tile.has_both && launch_policy.paired_vertical;
            const unsigned int columns_per_thread = paired_vertical
                ? inverse_vertical_columns_per_thread : 1U;
            void *parameters[] = {
                &source_pointer, &width,
                &candidates_pointer, &tile_candidate_count,
                &transpose_offsets_pointer, &transpose_indices_pointer,
                &transpose_weights_pointer, &lower_ld_pointer,
                &upper_l_pointer, &inverse_diagonal_pointer,
                &workspace_pointer,
            };
            const unsigned int blocks = static_cast<unsigned int>(
                (tile.maximum_vertical_columns
                 + inverse_threads * columns_per_thread - 1U)
                / (inverse_threads * columns_per_thread));
            launch(
                paired_vertical
                    ? impl_->inverse_vertical_pair_function
                    : impl_->inverse_vertical_function,
                blocks, tile_candidate_count, 1U,
                inverse_threads, 1U, 0U, parameters,
                paired_vertical
                    ? "cuLaunchKernel(getnative_cuda_inverse_vertical_pair)"
                    : "cuLaunchKernel(getnative_cuda_inverse_vertical)");
        }
        record_stage(6);
        if (tile.has_both && !fused_both) {
            void *parameters[] = {
                &width, &height,
                &candidates_pointer, &tile_candidate_count,
                &forward_left_pointer, &forward_weights_pointer,
                &workspace_pointer,
            };
            const unsigned int blocks = static_cast<unsigned int>(
                (tile.maximum_forward_elements + pixel_threads - 1U)
                / pixel_threads);
            launch(
                impl_->forward_intermediate_function,
                blocks, tile_candidate_count, 1U,
                pixel_threads, 1U, 0U, parameters,
                "cuLaunchKernel(getnative_cuda_forward_intermediate)");
        }
        record_stage(7);
        if (fused_both) {
            void *parameters[] = {
                &source_pointer, &width,
                &candidates_pointer, &tile_candidate_count,
                &forward_left_pointer, &forward_weights_pointer,
                &workspace_pointer,
                &crop_left, &crop_right, &crop_top, &norm,
                &threshold, &partials_pointer,
            };
            launch(
                impl_->both_fused_metric_function,
                fused_grid_x, tile_candidate_count, fused_grid_z,
                pixel_threads, 1U,
                static_cast<unsigned int>(
                    tile.maximum_fused_horizontal_span * sizeof(float)),
                parameters, "cuLaunchKernel(getnative_cuda_both_fused_metric)");
        } else if (!horizontal_only) {
            void *parameters[] = {
                &source_pointer, &transposed_source_pointer, &width, &height,
                &candidates_pointer, &tile_candidate_count,
                &forward_left_pointer, &forward_weights_pointer,
                &workspace_pointer,
                &crop_left, &crop_right, &crop_top, &crop_bottom, &norm,
                &threshold, &partials_pointer,
            };
            launch(
                impl_->metric_partials_function,
                tile_metric_block_count, tile_candidate_count, 1U,
                pixel_threads, 1U,
                pixel_threads * static_cast<unsigned int>(sizeof(double)),
                parameters, "cuLaunchKernel(getnative_cuda_metric_partials)");
        }
        {
            void *parameters[] = {
                &partials_pointer, &tile_metric_block_count, &tile_candidate_count,
                &metric_pixels, &results_pointer,
            };
            launch(
                impl_->metric_finalize_function,
                tile_candidate_count, 1U, 1U, pixel_threads, 1U,
                pixel_threads * static_cast<unsigned int>(sizeof(double)),
                parameters, "cuLaunchKernel(getnative_cuda_metric_finalize)");
        }
        record_stage(8);

        const std::size_t tile_result_bytes = checked_product(
            tile.candidate_count, sizeof(double), "CUDA tile result readback");
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->memcpy_dtoh_async(
                slot.pinned_results.data(), slot.device_results.pointer(),
                tile_result_bytes, slot.stream),
            "cuMemcpyDtoHAsync_v2(results)");
        record_stage(9);
        cuda_detail::cuda_check(
            *impl_->api,
            impl_->api->stream_synchronize(slot.stream),
            "cuStreamSynchronize(staged)");
        stream_synced = true;

        if (metric.norm > 1U) {
            auto *tile_results = static_cast<double *>(slot.pinned_results.data());
            const double reciprocal_norm = 1.0 / static_cast<double>(metric.norm);
            for (std::size_t index = 0; index < tile.candidate_count; ++index) {
                tile_results[index] = std::pow(
                    tile_results[index], reciprocal_norm);
            }
        }

        if (instrument) {
            if (tile.has_horizontal) {
                if (horizontal_only) {
                    delta.horizontal_fused_ms += elapsed_ms(
                        *impl_->api, slot.events[4], slot.events[5]);
                } else {
                    delta.inverse_horizontal_ms += elapsed_ms(
                        *impl_->api, slot.events[4], slot.events[5]);
                }
            }
            if (tile.has_vertical) {
                delta.inverse_vertical_ms += elapsed_ms(
                    *impl_->api, slot.events[5], slot.events[6]);
            }
            if (tile.has_both && !fused_both) {
                delta.forward_intermediate_ms += elapsed_ms(
                    *impl_->api, slot.events[6], slot.events[7]);
            }
            delta.metric_ms += elapsed_ms(
                *impl_->api, slot.events[7], slot.events[8]);
            delta.result_readback_ms += elapsed_ms(
                *impl_->api, slot.events[8], slot.events[9]);
        }
        std::memcpy(
            errors.data() + tile.first_candidate,
            slot.pinned_results.data(), tile_result_bytes);
        ++delta.tile_count;
        if (stop.stop_requested()) {
            throw std::runtime_error("CUDA analysis cancelled");
        }
    }
    slot.plan_ready = true;
    if (instrument) {
        if (cuda_device_source) {
            delta.source_conversion_ms = elapsed_ms(
                *impl_->api, slot.events[0], slot.events[1]);
        } else {
            delta.source_upload_ms = shared_source
                ? shared_upload_ms
                : elapsed_ms(*impl_->api, slot.events[0], slot.events[1]);
        }
        if (!plan_cache_hit) {
            delta.plan_upload_ms = elapsed_ms(
                *impl_->api, slot.events[1], slot.events[2]);
        }
        if (shared_source) {
            delta.source_transpose_ms = shared_transpose_ms;
        } else if (packed.has_horizontal) {
            delta.source_transpose_ms = elapsed_ms(
                *impl_->api, slot.events[2], slot.events[3]);
        }
        delta.gpu_total_ms = elapsed_ms(
            *impl_->api, slot.events[0], slot.events[9]);
    } else if (shared_source) {
        delta.source_transpose_ms = shared_transpose_ms;
        delta.source_upload_ms = shared_upload_ms;
    }
    delta.kernel_ms = delta.source_conversion_ms + delta.source_transpose_ms
        + delta.horizontal_fused_ms + delta.inverse_horizontal_ms
        + delta.inverse_vertical_ms
        + delta.forward_intermediate_ms + delta.metric_ms;
    delta.analyzed_candidate_count = candidates.size();

    std::vector<CandidateResult> result;
    result.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!std::isfinite(errors[index])) {
            throw std::runtime_error(
                "CUDA cpp-generic staged path produced a non-finite metric");
        }
        result.push_back({candidates[index].id, errors[index]});
    }
    if (instrument) {
        const std::scoped_lock telemetry_lock(impl_->telemetry_mutex);
        merge_telemetry(impl_->telemetry, delta);
        impl_->telemetry.peak_workspace_elements =
            impl_->peak_workspace_elements.load(std::memory_order_relaxed);
    }
    return result;
}

} // namespace getnative

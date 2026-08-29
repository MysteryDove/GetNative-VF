#include "getnative/metal_analysis.hpp"

#include "getnative_metallib.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace getnative {
namespace {

constexpr std::uint32_t horizontal_axis = 0;
constexpr std::uint32_t vertical_axis = 1;
constexpr NSUInteger reduction_width = 256;
constexpr std::size_t maximum_queued_tiles = 32;
constexpr std::int32_t maximum_half_bandwidth = 15;
constexpr std::int32_t maximum_forward_width = 16;
constexpr std::size_t maximum_reduction_groups =
    (static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
     - static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    / static_cast<std::size_t>(reduction_width);

enum class KernelShape : std::uint8_t {
    bandwidth3,
    bandwidth7,
    bandwidth11,
    bandwidth15,
    generic,
};

enum class PipelineStage : std::uint8_t {
    image_inverse,
    metric,
    matrix_inverse,
    matrix_forward,
    horizontal_first_metric,
};

struct LumaNormalizeJob {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bit_depth;
    std::uint32_t full_range;
};

constexpr std::array all_pipeline_stages{
    PipelineStage::image_inverse,
    PipelineStage::metric,
    PipelineStage::matrix_inverse,
    PipelineStage::matrix_forward,
    PipelineStage::horizontal_first_metric,
};

struct alignas(16) AxisPlanDescriptor {
    std::uint32_t source_size;
    std::uint32_t destination_size;
    std::uint32_t half_bandwidth;
    std::uint32_t forward_width;
    std::uint32_t transpose_offsets_base;
    std::uint32_t transpose_entries_base;
    std::uint32_t lower_ld_base;
    std::uint32_t upper_l_base;
    std::uint32_t inverse_diagonal_base;
    std::uint32_t forward_left_base;
    std::uint32_t forward_weights_base;
    std::uint32_t workspace_base;
    std::uint32_t direction;
    std::uint32_t vector_count;
    std::uint32_t reserved_0 = 0;
    std::uint32_t reserved_1 = 0;
};

struct AnalysisJob {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t crop_left;
    std::uint32_t crop_right;
    std::uint32_t crop_top;
    std::uint32_t crop_bottom;
    float threshold;
    std::uint32_t groups_per_candidate;
    std::uint32_t candidate_count;
    std::uint32_t maximum_vector_count;
    std::uint32_t norm;
};

struct MetricCropBounds {
    std::uint32_t left;
    std::uint32_t right;
    std::uint32_t top;
    std::uint32_t bottom;
    double pixel_count;
};

static_assert(sizeof(AxisPlanDescriptor) == 64);
static_assert(sizeof(AnalysisJob) == 44);

[[nodiscard]] std::string ns_error(NSError *error, std::string_view fallback) {
    if (error == nil) {
        return std::string{fallback};
    }
    const char *text = error.localizedDescription.UTF8String;
    return text == nullptr ? std::string{fallback} : std::string{text};
}

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, std::string_view name) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string{name} + " exceeds Metal's 32-bit index range");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::size_t checked_product(std::size_t a, std::size_t b,
                                          std::string_view name) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::length_error(std::string{name} + " size overflow");
    }
    return a * b;
}

[[nodiscard]] MetricCropBounds metric_crop_bounds(ConstImageView source,
                                                   const MetricSpec &metric) {
    if (metric.crop_left < 0 || metric.crop_right < 0 || metric.crop_top < 0
        || metric.crop_bottom < 0 || !std::isfinite(metric.threshold)
        || metric.threshold < 0.0F) {
        throw std::invalid_argument("invalid Metal metric configuration");
    }

    const auto cropped_width = static_cast<std::int64_t>(source.width)
        - static_cast<std::int64_t>(metric.crop_left)
        - static_cast<std::int64_t>(metric.crop_right);
    const auto cropped_height = static_cast<std::int64_t>(source.height)
        - static_cast<std::int64_t>(metric.crop_top)
        - static_cast<std::int64_t>(metric.crop_bottom);
    if (cropped_width <= 0 || cropped_height <= 0) {
        throw std::invalid_argument("invalid Metal metric configuration");
    }

    return {
        static_cast<std::uint32_t>(metric.crop_left),
        static_cast<std::uint32_t>(metric.crop_right),
        static_cast<std::uint32_t>(metric.crop_top),
        static_cast<std::uint32_t>(metric.crop_bottom),
        static_cast<double>(cropped_width) * static_cast<double>(cropped_height),
    };
}

template <class T>
class PlanRegion {
public:
    using value_type = T;
    PlanRegion() = default;
    explicit PlanRegion(std::span<T> storage) : mode_(Mode::direct), storage_(storage) {}

    [[nodiscard]] static PlanRegion owning() {
        PlanRegion result;
        result.mode_ = Mode::owning;
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return mode_ == Mode::owning ? values_.size() : size_;
    }
    [[nodiscard]] const T *data() const noexcept {
        return mode_ == Mode::owning ? values_.data() : storage_.data();
    }
    void reserve(std::size_t size) {
        if (mode_ == Mode::owning) {
            values_.reserve(size);
        } else if (mode_ == Mode::direct && size > storage_.size()) {
            throw std::length_error("Metal direct plan region is too small");
        }
    }
    void push_back(const T &value) {
        if (mode_ == Mode::owning) {
            values_.push_back(value);
            return;
        }
        if (size_ == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("Metal plan region size overflow");
        }
        if (mode_ == Mode::direct) {
            if (size_ == storage_.size()) {
                throw std::length_error("Metal direct plan region overflow");
            }
            storage_[size_] = value;
        }
        ++size_;
    }
    template <class Iterator>
    void append(Iterator first, Iterator last) {
        if (mode_ == Mode::owning) {
            values_.insert(values_.end(), first, last);
            return;
        }
        for (; first != last; ++first) {
            push_back(*first);
        }
    }

private:
    enum class Mode : std::uint8_t { counting, owning, direct };
    Mode mode_ = Mode::counting;
    std::vector<T> values_;
    std::span<T> storage_;
    std::size_t size_ = 0U;
};

struct PackedTile {
    PlanRegion<AxisPlanDescriptor> descriptors;
    PlanRegion<std::uint32_t> transpose_offsets;
    PlanRegion<std::uint32_t> transpose_indices;
    PlanRegion<float> transpose_weights;
    PlanRegion<float> lower_ld;
    PlanRegion<float> upper_l;
    PlanRegion<float> inverse_diagonal;
    PlanRegion<std::int32_t> forward_left;
    PlanRegion<float> forward_weights;
    std::size_t workspace_elements = 0;
    std::uint32_t maximum_vector_count = 0;
    std::uint32_t maximum_native_width = 0;
    std::uint32_t maximum_native_height = 0;
};

[[nodiscard]] PackedTile make_owning_packed_tile() {
    return {
        PlanRegion<AxisPlanDescriptor>::owning(),
        PlanRegion<std::uint32_t>::owning(),
        PlanRegion<std::uint32_t>::owning(),
        PlanRegion<float>::owning(),
        PlanRegion<float>::owning(),
        PlanRegion<float>::owning(),
        PlanRegion<float>::owning(),
        PlanRegion<std::int32_t>::owning(),
        PlanRegion<float>::owning(),
    };
}

struct TileSignature {
    AnalysisAxes axes = AnalysisAxes::vertical;
    KernelShape horizontal_inverse_shape = KernelShape::generic;
    KernelShape horizontal_forward_shape = KernelShape::generic;
    KernelShape vertical_inverse_shape = KernelShape::generic;
    KernelShape vertical_forward_shape = KernelShape::generic;
    ForwardOrder forward_order = ForwardOrder::vertical_first;

    friend bool operator==(const TileSignature &, const TileSignature &) = default;
};

struct TileRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t workspace_elements = 0;
    TileSignature signature{};
};

[[nodiscard]] bool uses_specialized_pipeline(const TileSignature &signature) noexcept {
    if (signature.axes == AnalysisAxes::horizontal) {
        return signature.horizontal_inverse_shape != KernelShape::generic
            || signature.horizontal_forward_shape != KernelShape::generic;
    }
    if (signature.axes == AnalysisAxes::vertical) {
        return signature.vertical_inverse_shape != KernelShape::generic
            || signature.vertical_forward_shape != KernelShape::generic;
    }
    return signature.horizontal_inverse_shape != KernelShape::generic
        || signature.horizontal_forward_shape != KernelShape::generic
        || signature.vertical_inverse_shape != KernelShape::generic
        || signature.vertical_forward_shape != KernelShape::generic;
}

struct AxisKernelShapes {
    KernelShape inverse = KernelShape::generic;
    KernelShape forward = KernelShape::generic;
};

[[nodiscard]] KernelShape inverse_shape(std::int32_t half_bandwidth) noexcept {
    switch (half_bandwidth) {
    case 1: return KernelShape::bandwidth3;
    case 3: return KernelShape::bandwidth7;
    case 5: return KernelShape::bandwidth11;
    case 7: return KernelShape::bandwidth15;
    default: return KernelShape::generic;
    }
}

[[nodiscard]] KernelShape forward_shape(std::int32_t forward_width) noexcept {
    switch (forward_width) {
    case 2: return KernelShape::bandwidth3;
    case 4: return KernelShape::bandwidth7;
    case 6: return KernelShape::bandwidth11;
    case 8: return KernelShape::bandwidth15;
    default: return KernelShape::generic;
    }
}

[[nodiscard]] AxisKernelShapes axis_shapes(
    const std::shared_ptr<const AxisPlan> &plan_pointer,
    std::int32_t expected_source, MetalKernelDispatchPolicy policy) {
    if (!plan_pointer || !plan_pointer->valid()) {
        throw std::invalid_argument("Metal candidate contains an invalid axis plan");
    }
    const AxisPlan &plan = *plan_pointer;
    if (plan.source_size != expected_source) {
        throw std::invalid_argument("Metal axis plan does not match the source image");
    }
    if (plan.half_bandwidth < 1 || plan.half_bandwidth > maximum_half_bandwidth
        || plan.forward_width < 1 || plan.forward_width > maximum_forward_width) {
        throw std::invalid_argument(
            "Metal supports half-bandwidth 1..15 and forward width 1..16");
    }
    if (policy == MetalKernelDispatchPolicy::generic_only) {
        return {};
    }
    const AxisKernelShapes shapes{
        inverse_shape(plan.half_bandwidth), forward_shape(plan.forward_width),
    };
    if (policy == MetalKernelDispatchPolicy::required_specialized
        && (shapes.inverse == KernelShape::generic
            || shapes.forward == KernelShape::generic)) {
        const std::string unavailable = shapes.inverse == KernelShape::generic
            ? "inverse" : "forward";
        throw std::runtime_error(
            "required Metal " + unavailable + " specialization is unavailable for B"
            + std::to_string(2 * plan.half_bandwidth + 1) + "/F"
            + std::to_string(plan.forward_width));
    }
    return shapes;
}

[[nodiscard]] TileSignature candidate_signature(ConstImageView source,
                                                const CandidateAnalysis &candidate,
                                                MetalKernelDispatchPolicy policy) {
    TileSignature signature;
    signature.axes = candidate.axes;
    if (candidate.axes == AnalysisAxes::horizontal || candidate.axes == AnalysisAxes::both) {
        const AxisKernelShapes shapes = axis_shapes(candidate.horizontal, source.width, policy);
        signature.horizontal_inverse_shape = shapes.inverse;
        signature.horizontal_forward_shape = shapes.forward;
    }
    if (candidate.axes == AnalysisAxes::vertical || candidate.axes == AnalysisAxes::both) {
        const AxisKernelShapes shapes = axis_shapes(candidate.vertical, source.height, policy);
        signature.vertical_inverse_shape = shapes.inverse;
        signature.vertical_forward_shape = shapes.forward;
    }
    if (candidate.axes == AnalysisAxes::both) {
        signature.forward_order = select_forward_order(*candidate.horizontal, *candidate.vertical);
    }
    return signature;
}

[[nodiscard]] std::size_t candidate_workspace_elements(ConstImageView source,
                                                        const CandidateAnalysis &candidate,
                                                        MetalKernelDispatchPolicy policy) {
    (void)candidate_signature(source, candidate, policy);
    if (candidate.axes != AnalysisAxes::both) {
        const auto &plan = candidate.axes == AnalysisAxes::horizontal
            ? candidate.horizontal : candidate.vertical;
        const std::size_t vectors = static_cast<std::size_t>(
            candidate.axes == AnalysisAxes::horizontal ? source.height : source.width);
        return checked_product(vectors, static_cast<std::size_t>(plan->destination_size),
                               "candidate workspace");
    }
    const std::size_t native_width = static_cast<std::size_t>(candidate.horizontal->destination_size);
    const std::size_t native_height = static_cast<std::size_t>(candidate.vertical->destination_size);
    const std::size_t native = checked_product(native_width, native_height, "two-axis native workspace");
    const std::size_t inverse_intermediate = checked_product(
        native_width, static_cast<std::size_t>(source.height), "two-axis inverse intermediate");
    const std::size_t horizontal_first_intermediate = checked_product(
        static_cast<std::size_t>(source.width), native_height, "two-axis forward intermediate");
    const std::size_t intermediate = std::max(inverse_intermediate, horizontal_first_intermediate);
    if (native > std::numeric_limits<std::size_t>::max() - intermediate) {
        throw std::length_error("two-axis candidate workspace size overflow");
    }
    return intermediate + native;
}

void append_axis(PackedTile &packed, const AxisPlan &plan, bool horizontal,
                 std::uint32_t vector_count, std::uint32_t workspace_base,
                 std::uint32_t reserved_0 = 0, std::uint32_t reserved_1 = 0) {
    const auto require_finite = [](const auto &values, std::string_view name) {
        for (const auto value : values) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument(
                    "Metal plan contains a non-finite " + std::string{name});
            }
        }
    };
    require_finite(plan.transpose_weights, "transpose weight");
    require_finite(plan.lower_ld, "lower factor");
    require_finite(plan.upper_l, "upper factor");
    require_finite(plan.inverse_diagonal, "inverse diagonal");
    require_finite(plan.forward_weights, "forward weight");
    AxisPlanDescriptor descriptor{};
    descriptor.source_size = static_cast<std::uint32_t>(plan.source_size);
    descriptor.destination_size = static_cast<std::uint32_t>(plan.destination_size);
    descriptor.half_bandwidth = static_cast<std::uint32_t>(plan.half_bandwidth);
    descriptor.forward_width = static_cast<std::uint32_t>(plan.forward_width);
    descriptor.transpose_offsets_base = checked_u32(
        packed.transpose_offsets.size(), "transpose offset base");
    descriptor.transpose_entries_base = checked_u32(
        packed.transpose_indices.size(), "transpose entry base");
    descriptor.lower_ld_base = checked_u32(packed.lower_ld.size(), "lower factor base");
    descriptor.upper_l_base = checked_u32(packed.upper_l.size(), "upper factor base");
    descriptor.inverse_diagonal_base = checked_u32(
        packed.inverse_diagonal.size(), "diagonal base");
    descriptor.forward_left_base = checked_u32(packed.forward_left.size(), "forward left base");
    descriptor.forward_weights_base = checked_u32(
        packed.forward_weights.size(), "forward weight base");
    descriptor.workspace_base = workspace_base;
    descriptor.direction = horizontal ? horizontal_axis : vertical_axis;
    descriptor.vector_count = vector_count;
    descriptor.reserved_0 = reserved_0;
    descriptor.reserved_1 = reserved_1;

    packed.transpose_offsets.append(plan.transpose_offsets.begin(), plan.transpose_offsets.end());
    for (const std::int32_t index : plan.transpose_indices) {
        if (index < 0 || index >= plan.source_size) {
            throw std::invalid_argument("Metal transpose index is outside the source axis");
        }
        packed.transpose_indices.push_back(static_cast<std::uint32_t>(index));
    }
    packed.transpose_weights.append(plan.transpose_weights.begin(), plan.transpose_weights.end());
    packed.lower_ld.append(plan.lower_ld.begin(), plan.lower_ld.end());
    packed.upper_l.append(plan.upper_l.begin(), plan.upper_l.end());
    packed.inverse_diagonal.append(plan.inverse_diagonal.begin(), plan.inverse_diagonal.end());
    for (std::int32_t row = 0; row < plan.source_size; ++row) {
        const std::uint32_t begin = plan.forward_offsets[static_cast<std::size_t>(row)];
        const std::uint32_t end = plan.forward_offsets[static_cast<std::size_t>(row) + 1U];
        if (end - begin != static_cast<std::uint32_t>(plan.forward_width)) {
            throw std::invalid_argument("Metal forward row width does not match the plan");
        }
        const std::int32_t left = plan.forward_indices[begin];
        if (left < 0 || left > plan.destination_size - plan.forward_width) {
            throw std::invalid_argument("Metal forward row is outside the native axis");
        }
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            if (plan.forward_indices[begin + static_cast<std::uint32_t>(tap)] != left + tap) {
                throw std::invalid_argument("Metal forward row indices must be contiguous");
            }
        }
        packed.forward_left.push_back(left);
        packed.forward_weights.append(
            plan.forward_weights.begin() + static_cast<std::ptrdiff_t>(begin),
            plan.forward_weights.begin() + static_cast<std::ptrdiff_t>(end));
    }
    packed.descriptors.push_back(descriptor);
}

void append_single_plan(PackedTile &packed, ConstImageView source,
                        const CandidateAnalysis &candidate,
                        MetalKernelDispatchPolicy policy) {
    (void)candidate_signature(source, candidate, policy);
    const bool horizontal = candidate.axes == AnalysisAxes::horizontal;
    const auto &plan = horizontal ? candidate.horizontal : candidate.vertical;
    const std::uint32_t vector_count = static_cast<std::uint32_t>(
        horizontal ? source.height : source.width);
    const std::size_t candidate_workspace = checked_product(
        static_cast<std::size_t>(vector_count),
        static_cast<std::size_t>(plan->destination_size), "candidate workspace");
    if (candidate_workspace > std::numeric_limits<std::size_t>::max() - packed.workspace_elements) {
        throw std::length_error("Metal tile workspace size overflow");
    }
    append_axis(packed, *plan, horizontal, vector_count,
                checked_u32(packed.workspace_elements, "workspace base"));
    packed.workspace_elements += candidate_workspace;
    packed.maximum_vector_count = std::max(packed.maximum_vector_count, vector_count);
}

void append_two_axis_plans(PackedTile &packed, ConstImageView source,
                           std::span<const CandidateAnalysis> candidates,
                           MetalKernelDispatchPolicy policy) {
    struct Bases { std::uint32_t intermediate; std::uint32_t native; };
    std::vector<Bases> bases;
    bases.reserve(candidates.size());
    for (const CandidateAnalysis &candidate : candidates) {
        (void)candidate_signature(source, candidate, policy);
        const std::size_t candidate_elements =
            candidate_workspace_elements(source, candidate, policy);
        const std::size_t native_elements = checked_product(
            static_cast<std::size_t>(candidate.horizontal->destination_size),
            static_cast<std::size_t>(candidate.vertical->destination_size),
            "two-axis native workspace");
        if (candidate_elements > std::numeric_limits<std::size_t>::max() - packed.workspace_elements) {
            throw std::length_error("Metal tile workspace size overflow");
        }
        const std::size_t native_base = packed.workspace_elements + candidate_elements - native_elements;
        bases.push_back({checked_u32(packed.workspace_elements, "intermediate workspace base"),
                         checked_u32(native_base, "native workspace base")});
        packed.workspace_elements += candidate_elements;
        packed.maximum_native_width = std::max(
            packed.maximum_native_width,
            static_cast<std::uint32_t>(candidate.horizontal->destination_size));
        packed.maximum_native_height = std::max(
            packed.maximum_native_height,
            static_cast<std::uint32_t>(candidate.vertical->destination_size));
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const CandidateAnalysis &candidate = candidates[i];
        append_axis(packed, *candidate.horizontal, true,
                    static_cast<std::uint32_t>(source.height), bases[i].intermediate,
                    bases[i].native,
                    static_cast<std::uint32_t>(candidate.vertical->destination_size));
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const CandidateAnalysis &candidate = candidates[i];
        append_axis(packed, *candidate.vertical, false,
                    static_cast<std::uint32_t>(candidate.horizontal->destination_size),
                    bases[i].native, bases[i].intermediate);
    }
}

[[nodiscard]] id<MTLComputePipelineState> make_pipeline(id<MTLDevice> device,
                                                        id<MTLLibrary> library,
                                                        NSString *name) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (function == nil) {
        throw std::runtime_error("embedded Metal library is missing function "
                                 + std::string{name.UTF8String});
    }
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) {
        throw std::runtime_error(ns_error(error, "Metal pipeline creation failed"));
    }
    return pipeline;
}

struct ShapePipelines {
    id<MTLComputePipelineState> inverse = nil;
    id<MTLComputePipelineState> metric = nil;
    id<MTLComputePipelineState> matrix_inverse = nil;
    id<MTLComputePipelineState> matrix_forward = nil;
    id<MTLComputePipelineState> horizontal_first_metric = nil;
};

struct WorkingBufferSet {
    id<MTLBuffer> source = nil;
    id<MTLBuffer> workspace = nil;
    id<MTLBuffer> partials = nil;
    std::size_t resident_bytes = 0;
};

struct MetalExecutionSlot {
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> retained_source_buffer = nil;
    id<MTLBuffer> retained_workspace_buffer = nil;
    id<MTLBuffer> retained_partial_buffer = nil;
    id<MTLBuffer> external_source_buffer = nil;
    std::size_t external_source_bytes = 0U;
};

thread_local MetalExecutionSlot *active_slot = nullptr;

} // namespace

struct MetalAnalysisEngine::Impl {
    explicit Impl(MetalAnalysisOptions requested_options) : options(requested_options) {
        if (options.tile_size == 0 || options.reduction_groups_per_candidate == 0
            || options.inverse_threads_per_threadgroup == 0) {
            throw std::invalid_argument("Metal execution configuration counts must be positive");
        }
        if (options.tile_size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Metal tile size exceeds the supported range");
        }
        if (options.execution_slots == 0U || options.execution_slots > 8U) {
            throw std::invalid_argument("Metal execution_slots must be in [1, 8]");
        }
        if (options.reduction_groups_per_candidate > maximum_reduction_groups) {
            throw std::invalid_argument("Metal reduction group count exceeds the 32-bit schedule range");
        }
        if (options.reuse_working_buffers
            && options.retained_working_buffer_limit_bytes == 0) {
            throw std::invalid_argument(
                "Metal retained-buffer limit must be positive when reuse is enabled");
        }
        @autoreleasepool {
            device = MTLCreateSystemDefaultDevice();
            if (device == nil) {
                throw std::runtime_error("no Metal device is available");
            }
            queue = [device newCommandQueue];
            if (queue == nil) throw std::runtime_error("Metal command queue creation failed");
            slots.reserve(options.execution_slots);
            for (std::size_t index = 0U; index < options.execution_slots; ++index) {
                auto slot = std::make_unique<MetalExecutionSlot>();
                slot->queue = [device newCommandQueue];
                if (slot->queue == nil) {
                    throw std::runtime_error("Metal slot command queue creation failed");
                }
                slots.push_back(std::move(slot));
            }
            slot_busy.assign(options.execution_slots, false);

            dispatch_data_t library_data = dispatch_data_create(
                getnative_metallib, getnative_metallib_size,
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            NSError *error = nil;
            library = [device newLibraryWithData:library_data error:&error];
            if (library == nil) {
                throw std::runtime_error(ns_error(error, "embedded Metal library could not be loaded"));
            }
            luma_normalize_r8 = make_pipeline(device, library, @"normalize_luma_r8");
            luma_normalize_r16 = make_pipeline(device, library, @"normalize_luma_r16");
            for (const PipelineStage stage : all_pipeline_stages) {
                (void)pipeline(KernelShape::generic, stage);
            }
            if (options.kernel_dispatch != MetalKernelDispatchPolicy::generic_only) {
                for (const KernelShape shape : {
                         KernelShape::bandwidth3, KernelShape::bandwidth7}) {
                    for (const PipelineStage stage : all_pipeline_stages) {
                        (void)pipeline(shape, stage);
                    }
                }
            }
            const char *name = device.name.UTF8String;
            info.name = name == nullptr ? "Metal device" : std::string{name};
            info.registry_id = device.registryID;
            info.maximum_buffer_bytes = device.maxBufferLength;
            info.unified_memory = device.hasUnifiedMemory;
        }
    }

    MetalAnalysisOptions options;
    MetalDeviceInfo info;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    std::vector<std::unique_ptr<MetalExecutionSlot>> slots;
    std::vector<bool> slot_busy;
    std::mutex slot_mutex;
    std::condition_variable slot_available;
    std::mutex pipeline_mutex;
    id<MTLLibrary> library;
    ShapePipelines bandwidth3;
    ShapePipelines bandwidth7;
    ShapePipelines bandwidth11;
    ShapePipelines bandwidth15;
    ShapePipelines generic;
    id<MTLComputePipelineState> luma_normalize_r8;
    id<MTLComputePipelineState> luma_normalize_r16;
    std::size_t peak_workspace_elements = 0;
    std::size_t peak_working_set_bytes = 0;
    std::size_t buffer_allocation_count = 0;
    std::size_t buffer_allocation_bytes = 0;
    std::size_t working_buffer_allocation_count = 0;
    std::size_t working_buffer_allocation_bytes = 0;
    std::size_t working_buffer_reuse_count = 0;
    std::size_t working_buffer_active_bytes = 0;
    std::size_t working_buffer_peak_active_bytes = 0;
    std::size_t working_buffer_peak_retained_bytes = 0;
    std::size_t command_buffer_submission_count = 0;
    std::size_t command_buffer_completion_count = 0;
    std::size_t source_direct_write_bytes = 0;
    std::size_t source_legacy_copy_bytes = 0;
    std::size_t plan_direct_write_bytes = 0;
    std::size_t plan_legacy_copy_bytes = 0;
    std::size_t plan_upload_bytes = 0;
    std::size_t analyzed_tile_count = 0;
    std::size_t generic_tile_count = 0;
    std::size_t specialized_tile_count = 0;
    double buffer_allocation_ms = 0.0;
    double working_buffer_allocation_ms = 0.0;
    double source_upload_ms = 0.0;
    double plan_upload_ms = 0.0;
    double source_pack_ms = 0.0;
    double plan_pack_ms = 0.0;
    double buffer_wiring_ms = 0.0;
    double pipeline_creation_ms = 0.0;
    double gpu_execution_ms = 0.0;
    double execution_slot_wait_ms = 0.0;
    bool external_source_zero_copy = false;
    std::string fallback_reason;
    std::vector<std::string> created_pipeline_names;
    mutable std::recursive_mutex mutex;

    [[nodiscard]] std::size_t acquire_slot(std::stop_token stop) {
        std::unique_lock lock(slot_mutex);
        for (;;) {
            for (std::size_t index = 0U; index < slots.size(); ++index) {
                if (!slot_busy[index]) {
                    slot_busy[index] = true;
                    return index;
                }
            }
            if (stop.stop_requested()) {
                throw std::runtime_error("Metal analysis cancelled while waiting for a slot");
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

    [[nodiscard]] id<MTLCommandQueue> active_queue() const noexcept {
        return active_slot != nullptr ? active_slot->queue : queue;
    }

    [[nodiscard]] static std::size_t buffer_bytes(id<MTLBuffer> buffer) noexcept {
        return buffer == nil ? 0 : static_cast<std::size_t>(buffer.length);
    }

    [[nodiscard]] std::size_t retained_working_buffer_bytes_for(
        const MetalExecutionSlot &slot) const {
        const std::size_t source_bytes = buffer_bytes(slot.retained_source_buffer);
        const std::size_t workspace_bytes = buffer_bytes(slot.retained_workspace_buffer);
        const std::size_t partial_bytes = buffer_bytes(slot.retained_partial_buffer);
        if (source_bytes > std::numeric_limits<std::size_t>::max() - workspace_bytes
            || source_bytes + workspace_bytes
                > std::numeric_limits<std::size_t>::max() - partial_bytes) {
            throw std::length_error("Metal retained-buffer telemetry overflow");
        }
        return source_bytes + workspace_bytes + partial_bytes;
    }

    [[nodiscard]] std::size_t retained_working_buffer_bytes() const {
        std::size_t total = 0U;
        for (const auto &slot : slots) {
            const std::size_t bytes = retained_working_buffer_bytes_for(*slot);
            if (bytes > std::numeric_limits<std::size_t>::max() - total) {
                throw std::length_error("Metal retained-buffer telemetry overflow");
            }
            total += bytes;
        }
        return total;
    }

    void record_buffer(id<MTLBuffer> buffer, double elapsed_ms, bool working_buffer) {
        const std::scoped_lock lock(mutex);
        const std::size_t bytes = static_cast<std::size_t>(buffer.length);
        if (bytes > std::numeric_limits<std::size_t>::max() - buffer_allocation_bytes) {
            throw std::length_error("Metal allocation telemetry overflow");
        }
        ++buffer_allocation_count;
        buffer_allocation_bytes += bytes;
        buffer_allocation_ms += elapsed_ms;
        if (working_buffer) {
            if (bytes > std::numeric_limits<std::size_t>::max()
                            - working_buffer_allocation_bytes) {
                throw std::length_error("Metal working-buffer telemetry overflow");
            }
            ++working_buffer_allocation_count;
            working_buffer_allocation_bytes += bytes;
            working_buffer_allocation_ms += elapsed_ms;
        }
    }

    [[nodiscard]] id<MTLBuffer> allocate_empty_buffer(std::size_t bytes,
                                                       NSString *label,
                                                       bool working_buffer) {
        if (bytes == 0) {
            throw std::invalid_argument("Metal buffer must not be empty");
        }
        if (bytes > info.maximum_buffer_bytes) {
            throw std::length_error("Metal buffer exceeds the device buffer limit");
        }
        const auto start = std::chrono::steady_clock::now();
        id<MTLBuffer> buffer = [device newBufferWithLength:bytes
                                                   options:MTLResourceStorageModeShared];
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (buffer == nil) {
            throw std::runtime_error("Metal buffer allocation failed");
        }
        buffer.label = label;
        record_buffer(buffer, elapsed_ms, working_buffer);
        return buffer;
    }

    template <class T>
    [[nodiscard]] id<MTLBuffer> allocate_plan_buffer(const std::vector<T> &values,
                                                      std::string_view name) {
        if (values.empty()) {
            throw std::invalid_argument(std::string{name} + " must not be empty");
        }
        const std::size_t bytes = checked_product(values.size(), sizeof(T), name);
        const std::string label_text{name};
        NSString *label = [NSString stringWithUTF8String:label_text.c_str()];
        if (label == nil) throw std::invalid_argument("Metal buffer label is not UTF-8");
        id<MTLBuffer> buffer = allocate_empty_buffer(bytes, label, false);
        const auto upload_start = std::chrono::steady_clock::now();
        std::memcpy(buffer.contents, values.data(), bytes);
        plan_upload_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - upload_start).count();
        if (bytes > std::numeric_limits<std::size_t>::max() - plan_upload_bytes) {
            throw std::length_error("Metal plan-upload telemetry overflow");
        }
        plan_upload_bytes += bytes;
        return buffer;
    }

    // One plan arena per tile instead of nine separate MTLBuffers: all plan
    // arrays are packed into a single shared allocation with 16-byte-aligned
    // regions, turning 9 allocations into 1 while the descriptor/base-offset
    // addressing the kernels already use stays unchanged.
    struct PlanArena {
        id<MTLBuffer> buffer;
        std::array<std::size_t, 9> offsets{};
        std::size_t total_bytes = 0;
    };

    [[nodiscard]] PlanArena allocate_plan_arena(
        const std::array<std::size_t, 9> &region_bytes,
        const std::array<std::string_view, 9> &names) {
        const std::scoped_lock lock(mutex);
        constexpr std::size_t alignment = 16;
        PlanArena arena;
        std::size_t cursor = 0;
        for (std::size_t index = 0; index < region_bytes.size(); ++index) {
            if (cursor > std::numeric_limits<std::size_t>::max() - (alignment - 1U)) {
                throw std::length_error(std::string{names[index]} + " alignment overflow");
            }
            arena.offsets[index] = (cursor + alignment - 1) / alignment * alignment;
            if (region_bytes[index] > std::numeric_limits<std::size_t>::max()
                                          - arena.offsets[index]) {
                throw std::length_error(std::string{names[index]} + " size overflow");
            }
            cursor = arena.offsets[index] + region_bytes[index];
        }
        if (cursor == 0U) throw std::invalid_argument("Metal plan arena must not be empty");
        arena.total_bytes = cursor;
        const std::string label_text{"GetNative plan arena"};
        arena.buffer = allocate_empty_buffer(
            cursor, [NSString stringWithUTF8String:label_text.c_str()], false);
        if (arena.total_bytes > std::numeric_limits<std::size_t>::max() - plan_upload_bytes) {
            throw std::length_error("Metal plan-upload telemetry overflow");
        }
        plan_upload_bytes += arena.total_bytes;
        return arena;
    }

    void copy_legacy_plan_regions(
        const PlanArena &arena,
        const std::array<std::pair<const void *, std::size_t>, 9> &regions) {
        const auto start = std::chrono::steady_clock::now();
        auto *base = static_cast<std::byte *>(arena.buffer.contents);
        for (std::size_t index = 0; index < regions.size(); ++index) {
            if (regions[index].second != 0U) {
                std::memcpy(base + arena.offsets[index], regions[index].first,
                            regions[index].second);
            }
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const std::scoped_lock lock(mutex);
        plan_pack_ms += elapsed;
        plan_upload_ms += elapsed;
        plan_legacy_copy_bytes += arena.total_bytes;
        fallback_reason = "legacy_plan_pack_requested";
    }

    void record_direct_plan_write(const PlanArena &arena, double elapsed) {
        const std::scoped_lock lock(mutex);
        plan_pack_ms += elapsed;
        plan_upload_ms += elapsed;
        plan_direct_write_bytes += arena.total_bytes;
    }

    [[nodiscard]] id<MTLBuffer> acquire_retained_buffer(id<MTLBuffer> __strong &buffer,
                                                         std::size_t required_bytes,
                                                         NSString *label) {
        const std::scoped_lock lock(mutex);
        if (buffer != nil && buffer_bytes(buffer) >= required_bytes) {
            ++working_buffer_reuse_count;
            return buffer;
        }
        buffer = nil;
        id<MTLBuffer> replacement = allocate_empty_buffer(required_bytes, label, true);
        buffer = replacement;
        return buffer;
    }

    void clear_retained_working_buffers(MetalExecutionSlot &slot) noexcept {
        slot.retained_source_buffer = nil;
        slot.retained_workspace_buffer = nil;
        slot.retained_partial_buffer = nil;
    }

    void clear_retained_working_buffers() noexcept {
        for (const auto &slot : slots) clear_retained_working_buffers(*slot);
    }

    [[nodiscard]] WorkingBufferSet prepare_working_buffers(
        MetalExecutionSlot &slot,
        std::size_t source_bytes, std::size_t workspace_bytes,
        std::size_t partial_bytes, ConstImageView source) {
        const std::scoped_lock lock(mutex);
        if (source_bytes > std::numeric_limits<std::size_t>::max() - workspace_bytes
            || source_bytes + workspace_bytes
                > std::numeric_limits<std::size_t>::max() - partial_bytes) {
            throw std::length_error("Metal working-buffer size overflow");
        }
        const std::size_t active_bytes = source_bytes + workspace_bytes + partial_bytes;
        working_buffer_active_bytes = active_bytes;
        working_buffer_peak_active_bytes = std::max(
            working_buffer_peak_active_bytes, active_bytes);

        WorkingBufferSet result;
        if (slot.external_source_buffer != nil) {
            if (slot.external_source_bytes != source_bytes) {
                throw std::logic_error("Metal external source-buffer size mismatch");
            }
            result.source = slot.external_source_buffer;
            result.workspace = allocate_empty_buffer(
                workspace_bytes, @"GetNative Metal workspace", true);
            result.partials = allocate_empty_buffer(
                partial_bytes, @"GetNative metric partials", true);
            result.resident_bytes = source_bytes + workspace_bytes + partial_bytes;
            external_source_zero_copy = true;
            return result;
        }
        bool retain = options.reuse_working_buffers;
        if (retain) {
            const std::size_t desired_source = std::max(
                source_bytes, buffer_bytes(slot.retained_source_buffer));
            const std::size_t desired_workspace = std::max(
                workspace_bytes, buffer_bytes(slot.retained_workspace_buffer));
            const std::size_t desired_partials = std::max(
                partial_bytes, buffer_bytes(slot.retained_partial_buffer));
            if (desired_source > std::numeric_limits<std::size_t>::max() - desired_workspace
                || desired_source + desired_workspace
                    > std::numeric_limits<std::size_t>::max() - desired_partials) {
                throw std::length_error("Metal retained-buffer capacity overflow");
            }
            retain = desired_source + desired_workspace + desired_partials
                <= options.retained_working_buffer_limit_bytes;
        }

        if (retain) {
            result.source = acquire_retained_buffer(
                slot.retained_source_buffer, source_bytes, @"GetNative source");
            result.workspace = acquire_retained_buffer(
                slot.retained_workspace_buffer, workspace_bytes,
                @"GetNative Metal workspace");
            result.partials = acquire_retained_buffer(
                slot.retained_partial_buffer, partial_bytes,
                @"GetNative metric partials");
            result.resident_bytes = retained_working_buffer_bytes_for(slot);
            working_buffer_peak_retained_bytes = std::max(
                working_buffer_peak_retained_bytes, result.resident_bytes);
        } else {
            clear_retained_working_buffers(slot);
            result.source = allocate_empty_buffer(
                source_bytes, @"GetNative source", true);
            result.workspace = allocate_empty_buffer(
                workspace_bytes, @"GetNative Metal workspace", true);
            result.partials = allocate_empty_buffer(
                partial_bytes, @"GetNative metric partials", true);
            result.resident_bytes = active_bytes;
        }

        const auto upload_start = std::chrono::steady_clock::now();
        auto *destination = static_cast<float *>(result.source.contents);
        if (options.direct_source_write) {
            if (source.stride == source.width) {
                std::memcpy(destination, source.data, source_bytes);
            } else {
                for (std::int32_t y = 0; y < source.height; ++y) {
                    std::memcpy(
                        destination + static_cast<std::ptrdiff_t>(y) * source.width,
                        source.data + static_cast<std::ptrdiff_t>(y) * source.stride,
                        static_cast<std::size_t>(source.width) * sizeof(float));
                }
            }
            source_direct_write_bytes += source_bytes;
        } else {
            std::vector<float> contiguous(
                static_cast<std::size_t>(source.width)
                * static_cast<std::size_t>(source.height));
            for (std::int32_t y = 0; y < source.height; ++y) {
                std::copy_n(
                    source.data + static_cast<std::ptrdiff_t>(y) * source.stride,
                    source.width,
                    contiguous.data() + static_cast<std::ptrdiff_t>(y) * source.width);
            }
            std::memcpy(destination, contiguous.data(), source_bytes);
            source_legacy_copy_bytes += source_bytes;
            fallback_reason = "legacy_source_pack_requested";
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - upload_start).count();
        source_pack_ms += elapsed;
        source_upload_ms += elapsed;
        return result;
    }

    template <class Function>
    void record_buffer_wiring(Function &&function) {
        const auto start = std::chrono::steady_clock::now();
        std::forward<Function>(function)();
        const std::scoped_lock lock(mutex);
        buffer_wiring_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    [[nodiscard]] ShapePipelines &pipelines(KernelShape shape) noexcept {
        switch (shape) {
        case KernelShape::bandwidth3: return bandwidth3;
        case KernelShape::bandwidth7: return bandwidth7;
        case KernelShape::bandwidth11: return bandwidth11;
        case KernelShape::bandwidth15: return bandwidth15;
        case KernelShape::generic: return generic;
        }
        return generic;
    }

    [[nodiscard]] static std::string_view shape_suffix(KernelShape shape) noexcept {
        switch (shape) {
        case KernelShape::bandwidth3: return "b3";
        case KernelShape::bandwidth7: return "b7";
        case KernelShape::bandwidth11: return "b11";
        case KernelShape::bandwidth15: return "b15";
        case KernelShape::generic: return "generic";
        }
        return "generic";
    }

    [[nodiscard]] static std::string_view stage_prefix(PipelineStage stage) noexcept {
        switch (stage) {
        case PipelineStage::image_inverse: return "inverse_axis";
        case PipelineStage::metric: return "metric_axis";
        case PipelineStage::matrix_inverse: return "inverse_axis_matrix";
        case PipelineStage::matrix_forward: return "forward_axis_matrix";
        case PipelineStage::horizontal_first_metric:
            return "metric_axis_horizontal_first";
        }
        return "Metal pipeline";
    }

    [[nodiscard]] static id<MTLComputePipelineState> existing_pipeline(
        const ShapePipelines &pipelines, PipelineStage stage) noexcept {
        switch (stage) {
        case PipelineStage::image_inverse: return pipelines.inverse;
        case PipelineStage::metric: return pipelines.metric;
        case PipelineStage::matrix_inverse: return pipelines.matrix_inverse;
        case PipelineStage::matrix_forward: return pipelines.matrix_forward;
        case PipelineStage::horizontal_first_metric: return pipelines.horizontal_first_metric;
        }
        return nil;
    }

    static void store_pipeline(ShapePipelines &pipelines, PipelineStage stage,
                               id<MTLComputePipelineState> value) noexcept {
        switch (stage) {
        case PipelineStage::image_inverse: pipelines.inverse = value; return;
        case PipelineStage::metric: pipelines.metric = value; return;
        case PipelineStage::matrix_inverse: pipelines.matrix_inverse = value; return;
        case PipelineStage::matrix_forward: pipelines.matrix_forward = value; return;
        case PipelineStage::horizontal_first_metric:
            pipelines.horizontal_first_metric = value;
            return;
        }
    }

    [[nodiscard]] id<MTLComputePipelineState> pipeline(KernelShape shape,
                                                       PipelineStage stage) {
        const std::scoped_lock lock(pipeline_mutex);
        ShapePipelines &shape_pipelines = pipelines(shape);
        id<MTLComputePipelineState> result = existing_pipeline(shape_pipelines, stage);
        if (result != nil) return result;

        std::string name{stage_prefix(stage)};
        name += '_';
        name += shape_suffix(shape);
        NSString *function_name = [NSString stringWithUTF8String:name.c_str()];
        if (function_name == nil) throw std::runtime_error("Metal function name is not UTF-8");
        const auto start = std::chrono::steady_clock::now();
        result = make_pipeline(device, library, function_name);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        {
            const std::scoped_lock telemetry_lock(mutex);
            pipeline_creation_ms += std::chrono::duration<double, std::milli>(elapsed).count();
        }
        if ((stage == PipelineStage::metric
             || stage == PipelineStage::horizontal_first_metric)
            && result.maxTotalThreadsPerThreadgroup < reduction_width) {
            throw std::runtime_error("Metal device cannot run the 256-thread reduction kernels");
        }
        store_pipeline(shape_pipelines, stage, result);
        {
            const std::scoped_lock telemetry_lock(mutex);
            created_pipeline_names.push_back(std::move(name));
        }
        return result;
    }
};

bool metal_backend_available() noexcept {
    @autoreleasepool {
        return MTLCreateSystemDefaultDevice() != nil;
    }
}

MetalAnalysisEngine::MetalAnalysisEngine(MetalAnalysisOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

MetalAnalysisEngine::~MetalAnalysisEngine() = default;
MetalAnalysisEngine::MetalAnalysisEngine(MetalAnalysisEngine &&) noexcept = default;
MetalAnalysisEngine &MetalAnalysisEngine::operator=(MetalAnalysisEngine &&) noexcept = default;

const MetalDeviceInfo &MetalAnalysisEngine::device_info() const noexcept { return impl_->info; }
const MetalAnalysisOptions &MetalAnalysisEngine::options() const noexcept { return impl_->options; }
std::size_t MetalAnalysisEngine::peak_workspace_elements() const noexcept {
    return impl_->peak_workspace_elements;
}
std::size_t MetalAnalysisEngine::peak_working_set_bytes() const noexcept {
    return impl_->peak_working_set_bytes;
}
MetalRuntimeTelemetry MetalAnalysisEngine::runtime_telemetry() const {
    const std::scoped_lock lock(impl_->mutex);
    MetalRuntimeTelemetry result;
    result.buffer_allocation_count = impl_->buffer_allocation_count;
    result.buffer_allocation_bytes = impl_->buffer_allocation_bytes;
    result.working_buffer_allocation_count = impl_->working_buffer_allocation_count;
    result.working_buffer_allocation_bytes = impl_->working_buffer_allocation_bytes;
    result.working_buffer_reuse_count = impl_->working_buffer_reuse_count;
    result.working_buffer_active_bytes = impl_->working_buffer_active_bytes;
    result.working_buffer_retained_bytes = impl_->retained_working_buffer_bytes();
    result.working_buffer_peak_active_bytes = impl_->working_buffer_peak_active_bytes;
    result.working_buffer_peak_retained_bytes = impl_->working_buffer_peak_retained_bytes;
    result.command_buffer_submission_count = impl_->command_buffer_submission_count;
    result.command_buffer_completion_count = impl_->command_buffer_completion_count;
    result.source_direct_write_bytes = impl_->source_direct_write_bytes;
    result.source_legacy_copy_bytes = impl_->source_legacy_copy_bytes;
    result.plan_direct_write_bytes = impl_->plan_direct_write_bytes;
    result.plan_legacy_copy_bytes = impl_->plan_legacy_copy_bytes;
    result.plan_upload_bytes = impl_->plan_upload_bytes;
    result.analyzed_tile_count = impl_->analyzed_tile_count;
    result.generic_tile_count = impl_->generic_tile_count;
    result.specialized_tile_count = impl_->specialized_tile_count;
    result.buffer_allocation_ms = impl_->buffer_allocation_ms;
    result.working_buffer_allocation_ms = impl_->working_buffer_allocation_ms;
    result.source_upload_ms = impl_->source_upload_ms;
    result.plan_upload_ms = impl_->plan_upload_ms;
    result.source_pack_ms = impl_->source_pack_ms;
    result.plan_pack_ms = impl_->plan_pack_ms;
    result.buffer_wiring_ms = impl_->buffer_wiring_ms;
    result.pipeline_creation_ms = impl_->pipeline_creation_ms;
    result.gpu_execution_ms = impl_->gpu_execution_ms;
    result.execution_slot_wait_ms = impl_->execution_slot_wait_ms;
    result.external_source_zero_copy = impl_->external_source_zero_copy;
    result.shared_uma_path = impl_->info.unified_memory;
    result.fallback_reason = impl_->fallback_reason;
    result.created_pipeline_names = impl_->created_pipeline_names;
    return result;
}
void MetalAnalysisEngine::reset_analysis_telemetry() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->buffer_allocation_count = 0;
    impl_->buffer_allocation_bytes = 0;
    impl_->working_buffer_allocation_count = 0;
    impl_->working_buffer_allocation_bytes = 0;
    impl_->working_buffer_reuse_count = 0;
    impl_->working_buffer_active_bytes = 0;
    impl_->working_buffer_peak_active_bytes = 0;
    impl_->working_buffer_peak_retained_bytes = impl_->retained_working_buffer_bytes();
    impl_->command_buffer_submission_count = 0;
    impl_->command_buffer_completion_count = 0;
    impl_->source_direct_write_bytes = 0;
    impl_->source_legacy_copy_bytes = 0;
    impl_->plan_direct_write_bytes = 0;
    impl_->plan_legacy_copy_bytes = 0;
    impl_->plan_upload_bytes = 0;
    impl_->analyzed_tile_count = 0;
    impl_->generic_tile_count = 0;
    impl_->specialized_tile_count = 0;
    impl_->buffer_allocation_ms = 0.0;
    impl_->working_buffer_allocation_ms = 0.0;
    impl_->source_upload_ms = 0.0;
    impl_->plan_upload_ms = 0.0;
    impl_->source_pack_ms = 0.0;
    impl_->plan_pack_ms = 0.0;
    impl_->buffer_wiring_ms = 0.0;
    impl_->gpu_execution_ms = 0.0;
    impl_->execution_slot_wait_ms = 0.0;
    impl_->external_source_zero_copy = false;
    impl_->fallback_reason.clear();
}

void MetalAnalysisEngine::trim_working_buffers() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->clear_retained_working_buffers();
}

void MetalAnalysisEngine::preflight_axis_batch(
    ConstImageView dimensions, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::size_t concurrency) const {
    if (dimensions.width <= 0 || dimensions.height <= 0
        || dimensions.stride < dimensions.width) {
        throw std::invalid_argument("invalid Metal preflight dimensions");
    }
    if (concurrency == 0U || concurrency > impl_->options.execution_slots) {
        throw std::length_error("Metal execution slots cannot satisfy requested concurrency");
    }
    if (metric.norm < metal_minimum_p_norm || metric.norm > metal_maximum_p_norm) {
        throw std::invalid_argument("Metal supports p-norm in 1..4");
    }
    if (candidates.empty()) return;
    const std::size_t elements = checked_product(
        static_cast<std::size_t>(dimensions.width),
        static_cast<std::size_t>(dimensions.height), "Metal preflight source");
    const std::size_t source_bytes = checked_product(elements, sizeof(float), "Metal preflight source");
    const std::size_t groups = impl_->options.reduction_groups_per_candidate;
    const std::size_t partial_bytes = checked_product(
        checked_product(candidates.size(), groups, "Metal preflight partials"),
        sizeof(float), "Metal preflight partials");
    std::size_t workspace = 0U;
    for (const CandidateAnalysis &candidate : candidates) {
        workspace = std::max(workspace, candidate_workspace_elements(
            dimensions, candidate, impl_->options.kernel_dispatch));
    }
    const std::size_t workspace_bytes = checked_product(workspace, sizeof(float), "Metal preflight workspace");
    const std::size_t per_slot = source_bytes + partial_bytes + workspace_bytes;
    if (source_bytes > impl_->info.maximum_buffer_bytes
        || partial_bytes > impl_->info.maximum_buffer_bytes
        || workspace_bytes > impl_->info.maximum_buffer_bytes
        || checked_product(per_slot, concurrency, "Metal concurrent working set")
               > impl_->options.retained_working_buffer_limit_bytes) {
        throw std::length_error("Metal device memory cannot satisfy requested concurrency");
    }
}

std::vector<CandidateResult> MetalAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    std::unique_ptr<MetalExecutionSlot, std::function<void(MetalExecutionSlot *)>> slot_guard(
        nullptr, [](MetalExecutionSlot *) {});
    if (active_slot == nullptr) {
        const auto slot_wait_start = std::chrono::steady_clock::now();
        const std::size_t slot_index = impl_->acquire_slot(stop);
        {
            const std::scoped_lock lock(impl_->mutex);
            impl_->execution_slot_wait_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - slot_wait_start).count();
        }
        active_slot = impl_->slots[slot_index].get();
        slot_guard = decltype(slot_guard)(active_slot,
            [impl = impl_.get(), slot_index](MetalExecutionSlot *) {
                active_slot = nullptr;
                impl->release_slot(slot_index);
            });
    }
    if (source.data == nullptr || source.width <= 0 || source.height <= 0
        || source.stride < source.width) {
        throw std::invalid_argument("invalid Metal source image");
    }
    const auto crop_bounds = metric_crop_bounds(source, metric);
    if (metric.norm < metal_minimum_p_norm || metric.norm > metal_maximum_p_norm) {
        throw std::invalid_argument("Metal supports p-norm in 1..4");
    }
    if (candidates.empty()) {
        return {};
    }
    if (stop.stop_requested()) {
        throw std::runtime_error("Metal analysis cancelled");
    }

    const std::size_t image_elements = checked_product(
        static_cast<std::size_t>(source.width), static_cast<std::size_t>(source.height),
        "source image");
    (void)checked_u32(image_elements, "source image element count");
    @autoreleasepool {
        const std::size_t source_bytes = checked_product(
            image_elements, sizeof(float), "Metal source buffer");
        if (source_bytes > impl_->info.maximum_buffer_bytes) {
            throw std::length_error("Metal source image exceeds the device buffer limit");
        }

        std::vector<CandidateResult> results(candidates.size());
        const std::size_t groups = impl_->options.reduction_groups_per_candidate;
        const double pixel_count = crop_bounds.pixel_count;

        const std::size_t device_workspace_limit =
            impl_->info.maximum_buffer_bytes / sizeof(float);
        const std::size_t requested_workspace_limit =
            impl_->options.workspace_limit_elements == 0
            ? device_workspace_limit
            : std::min(device_workspace_limit, impl_->options.workspace_limit_elements);
        const std::size_t configured_workspace_limit = std::min(
            requested_workspace_limit,
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
        std::vector<TileRange> tiles;
        tiles.reserve(candidates.size() / impl_->options.tile_size
                      + static_cast<std::size_t>(
                          candidates.size() % impl_->options.tile_size != 0));
        std::size_t maximum_workspace_elements = 0;
        std::size_t tile_begin = 0;
        while (tile_begin < candidates.size()) {
            if (stop.stop_requested()) {
                throw std::runtime_error("Metal analysis cancelled");
            }
            std::size_t tile_end = tile_begin;
            std::size_t estimated_workspace = 0;
            const TileSignature signature = candidate_signature(
                source, candidates[tile_begin], impl_->options.kernel_dispatch);
            const std::size_t maximum_tile_end = tile_begin + std::min(
                impl_->options.tile_size, candidates.size() - tile_begin);
            while (tile_end < maximum_tile_end) {
                const CandidateAnalysis &candidate = candidates[tile_end];
                if (candidate_signature(source, candidate, impl_->options.kernel_dispatch)
                    != signature) {
                    break;
                }
                const std::size_t candidate_workspace =
                    candidate_workspace_elements(
                        source, candidate, impl_->options.kernel_dispatch);
                if (candidate_workspace > configured_workspace_limit) {
                    throw std::length_error(
                        "one Metal candidate exceeds the workspace limit");
                }
                if (estimated_workspace > configured_workspace_limit
                    - candidate_workspace) {
                    break;
                }
                estimated_workspace += candidate_workspace;
                ++tile_end;
            }
            if (tile_end == tile_begin) {
                throw std::length_error("Metal could not fit one candidate in a tile");
            }
            tiles.push_back({tile_begin, tile_end, estimated_workspace, signature});
            maximum_workspace_elements = std::max(
                maximum_workspace_elements, estimated_workspace);
            tile_begin = tile_end;
        }

        {
            const std::scoped_lock lock(impl_->mutex);
            impl_->peak_workspace_elements = std::max(
                impl_->peak_workspace_elements, maximum_workspace_elements);
        }
        const std::size_t workspace_buffer_bytes = checked_product(
            maximum_workspace_elements, sizeof(float), "GetNative Metal workspace");
        const std::size_t partial_count = checked_product(
            candidates.size(), groups, "Metal partial results");
        const std::size_t partial_buffer_bytes = checked_product(
            partial_count, sizeof(float), "GetNative metric partials");
        const WorkingBufferSet working_buffers = impl_->prepare_working_buffers(
            *active_slot, source_bytes, workspace_buffer_bytes, partial_buffer_bytes, source);
        id<MTLBuffer> source_buffer = working_buffers.source;
        id<MTLBuffer> workspace_buffer = working_buffers.workspace;
        id<MTLBuffer> partial_buffer = working_buffers.partials;
        const std::size_t commands_per_tile = impl_->options.profile_split_kernels ? 4 : 1;
        NSMutableArray<id<MTLCommandBuffer>> *commands =
            [NSMutableArray arrayWithCapacity:std::min(tiles.size(), maximum_queued_tiles)
                                               * commands_per_tile];
        const auto drain_commands = [&] {
            std::string command_error;
            for (id<MTLCommandBuffer> command in commands) {
                [command waitUntilCompleted];
                {
                    const std::scoped_lock lock(impl_->mutex);
                    ++impl_->command_buffer_completion_count;
                }
                if (command.status == MTLCommandBufferStatusError &&
                    command_error.empty()) {
                    command_error = ns_error(command.error,
                                             "Metal command execution failed");
                }
                if (@available(macOS 10.15, *)) {
                    const double gpu_start = command.GPUStartTime;
                    const double gpu_end = command.GPUEndTime;
                    if (std::isfinite(gpu_start) && std::isfinite(gpu_end) &&
                        gpu_end >= gpu_start) {
                        const std::scoped_lock lock(impl_->mutex);
                        impl_->gpu_execution_ms += (gpu_end - gpu_start) * 1000.0;
                    }
                }
            }
            [commands removeAllObjects];
            if (!command_error.empty()) {
                throw std::runtime_error(command_error);
            }
        };
        std::size_t queued_tiles = 0;
        std::size_t queued_plan_bytes = 0;
        const std::size_t persistent_metal_bytes =
            working_buffers.resident_bytes;
        {
            const std::scoped_lock lock(impl_->mutex);
            impl_->peak_working_set_bytes = std::max(
                impl_->peak_working_set_bytes, persistent_metal_bytes);
        }

        const auto process_tile = [&](const TileRange &tile) {
            if (stop.stop_requested()) {
                throw std::runtime_error("Metal analysis cancelled");
            }
            @autoreleasepool {
                {
                    const std::scoped_lock lock(impl_->mutex);
                    ++impl_->analyzed_tile_count;
                    if (uses_specialized_pipeline(tile.signature)) {
                        ++impl_->specialized_tile_count;
                    } else {
                        ++impl_->generic_tile_count;
                    }
                }
                const std::size_t tile_candidate_count = tile.end - tile.begin;
                const auto pack_into = [&]<class Packed>(Packed &packed) {
                    packed.descriptors.reserve(
                        tile_candidate_count
                        * (tile.signature.axes == AnalysisAxes::both ? 2U : 1U));
                    if (tile.signature.axes == AnalysisAxes::both) {
                        append_two_axis_plans(
                            packed, source,
                            candidates.subspan(tile.begin, tile_candidate_count),
                            impl_->options.kernel_dispatch);
                    } else {
                        for (std::size_t index = tile.begin; index < tile.end; ++index) {
                            append_single_plan(
                                packed, source, candidates[index],
                                impl_->options.kernel_dispatch);
                        }
                    }
                };
                PackedTile packed;
                pack_into(packed);
                const std::size_t workspace_bytes = checked_product(
                    packed.workspace_elements, sizeof(float), "Metal workspace");
                if (workspace_bytes > impl_->info.maximum_buffer_bytes) {
                    throw std::length_error("Metal workspace exceeds the device buffer limit");
                }
                if (packed.workspace_elements > maximum_workspace_elements
                    || packed.workspace_elements != tile.workspace_elements) {
                    throw std::logic_error("Metal tile workspace preflight mismatch");
                }
                (void)checked_u32(packed.descriptors.size(), "plan descriptor table");
                (void)checked_u32(packed.transpose_offsets.size(), "transpose offset table");
                (void)checked_u32(packed.transpose_indices.size(), "transpose index table");
                (void)checked_u32(packed.transpose_weights.size(), "transpose weight table");
                (void)checked_u32(packed.lower_ld.size(), "lower factor table");
                (void)checked_u32(packed.upper_l.size(), "upper factor table");
                (void)checked_u32(packed.inverse_diagonal.size(), "inverse diagonal table");
                (void)checked_u32(packed.forward_left.size(), "forward left table");
                (void)checked_u32(packed.forward_weights.size(), "forward weight table");

                const AnalysisJob job{
                    static_cast<std::uint32_t>(source.width),
                    static_cast<std::uint32_t>(source.height),
                    crop_bounds.left,
                    crop_bounds.right,
                    crop_bounds.top,
                    crop_bounds.bottom,
                    metric.threshold,
                    checked_u32(groups, "reduction group count"),
                    checked_u32(tile_candidate_count, "candidate count"),
                    packed.maximum_vector_count,
                    metric.norm,
                };

                const auto region_bytes = [](const auto &values, std::string_view name) {
                    return checked_product(
                        values.size(), sizeof(typename std::decay_t<decltype(values)>::value_type),
                        name);
                };
                const std::array<std::size_t, 9> region_sizes{
                    region_bytes(packed.descriptors, "Metal plan descriptors"),
                    region_bytes(packed.transpose_offsets, "Metal transpose offsets"),
                    region_bytes(packed.transpose_indices, "Metal transpose indices"),
                    region_bytes(packed.transpose_weights, "Metal transpose weights"),
                    region_bytes(packed.lower_ld, "Metal lower factors"),
                    region_bytes(packed.upper_l, "Metal upper factors"),
                    region_bytes(packed.inverse_diagonal, "Metal inverse diagonal"),
                    region_bytes(packed.forward_left, "Metal forward left indices"),
                    region_bytes(packed.forward_weights, "Metal forward weights"),
                };
                constexpr std::array<std::string_view, 9> region_names{
                    "GetNative plan descriptors", "GetNative transpose offsets",
                    "GetNative transpose indices", "GetNative transpose weights",
                    "GetNative lower factors", "GetNative upper factors",
                    "GetNative inverse diagonal", "GetNative forward left indices",
                    "GetNative forward weights",
                };
                const auto same_shape = [](const auto &left, const auto &right) {
                    return left.descriptors.size() == right.descriptors.size()
                        && left.transpose_offsets.size() == right.transpose_offsets.size()
                        && left.transpose_indices.size() == right.transpose_indices.size()
                        && left.transpose_weights.size() == right.transpose_weights.size()
                        && left.lower_ld.size() == right.lower_ld.size()
                        && left.upper_l.size() == right.upper_l.size()
                        && left.inverse_diagonal.size() == right.inverse_diagonal.size()
                        && left.forward_left.size() == right.forward_left.size()
                        && left.forward_weights.size() == right.forward_weights.size()
                        && left.workspace_elements == right.workspace_elements
                        && left.maximum_vector_count == right.maximum_vector_count
                        && left.maximum_native_width == right.maximum_native_width
                        && left.maximum_native_height == right.maximum_native_height;
                };
                Impl::PlanArena arena;
                if (impl_->options.direct_plan_pack) {
                    arena = impl_->allocate_plan_arena(region_sizes, region_names);
                    auto *base = static_cast<std::byte *>(arena.buffer.contents);
                    PackedTile direct{
                        PlanRegion<AxisPlanDescriptor>{std::span<AxisPlanDescriptor>{
                            reinterpret_cast<AxisPlanDescriptor *>(base + arena.offsets[0]),
                            packed.descriptors.size()}},
                        PlanRegion<std::uint32_t>{std::span<std::uint32_t>{
                            reinterpret_cast<std::uint32_t *>(base + arena.offsets[1]),
                            packed.transpose_offsets.size()}},
                        PlanRegion<std::uint32_t>{std::span<std::uint32_t>{
                            reinterpret_cast<std::uint32_t *>(base + arena.offsets[2]),
                            packed.transpose_indices.size()}},
                        PlanRegion<float>{std::span<float>{
                            reinterpret_cast<float *>(base + arena.offsets[3]),
                            packed.transpose_weights.size()}},
                        PlanRegion<float>{std::span<float>{
                            reinterpret_cast<float *>(base + arena.offsets[4]),
                            packed.lower_ld.size()}},
                        PlanRegion<float>{std::span<float>{
                            reinterpret_cast<float *>(base + arena.offsets[5]),
                            packed.upper_l.size()}},
                        PlanRegion<float>{std::span<float>{
                            reinterpret_cast<float *>(base + arena.offsets[6]),
                            packed.inverse_diagonal.size()}},
                        PlanRegion<std::int32_t>{std::span<std::int32_t>{
                            reinterpret_cast<std::int32_t *>(base + arena.offsets[7]),
                            packed.forward_left.size()}},
                        PlanRegion<float>{std::span<float>{
                            reinterpret_cast<float *>(base + arena.offsets[8]),
                            packed.forward_weights.size()}},
                    };
                    const auto pack_start = std::chrono::steady_clock::now();
                    pack_into(direct);
                    const double pack_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - pack_start).count();
                    if (!same_shape(packed, direct)) {
                        throw std::logic_error("Metal direct plan pack preflight mismatch");
                    }
                    impl_->record_direct_plan_write(arena, pack_ms);
                } else {
                    PackedTile legacy = make_owning_packed_tile();
                    pack_into(legacy);
                    if (!same_shape(packed, legacy)) {
                        throw std::logic_error("Metal legacy plan pack preflight mismatch");
                    }
                    arena = impl_->allocate_plan_arena(region_sizes, region_names);
                    const auto region_of = [](const auto &values, std::string_view name) {
                        return std::pair<const void *, std::size_t>{
                            static_cast<const void *>(values.data()),
                            checked_product(
                                values.size(),
                                sizeof(typename std::decay_t<decltype(values)>::value_type),
                                name)};
                    };
                    impl_->copy_legacy_plan_regions(
                        arena,
                        {{region_of(legacy.descriptors, "Metal plan descriptors"),
                          region_of(legacy.transpose_offsets, "Metal transpose offsets"),
                          region_of(legacy.transpose_indices, "Metal transpose indices"),
                          region_of(legacy.transpose_weights, "Metal transpose weights"),
                          region_of(legacy.lower_ld, "Metal lower factors"),
                          region_of(legacy.upper_l, "Metal upper factors"),
                          region_of(legacy.inverse_diagonal, "Metal inverse diagonal"),
                          region_of(legacy.forward_left, "Metal forward left indices"),
                          region_of(legacy.forward_weights, "Metal forward weights")}});
                }
                id<MTLBuffer> plan_arena = arena.buffer;
                const std::size_t descriptor_base = arena.offsets[0];
                const std::size_t transpose_offsets_base = arena.offsets[1];
                const std::size_t transpose_indices_base = arena.offsets[2];
                const std::size_t transpose_weights_base = arena.offsets[3];
                const std::size_t lower_base = arena.offsets[4];
                const std::size_t upper_base = arena.offsets[5];
                const std::size_t diagonal_base = arena.offsets[6];
                const std::size_t forward_left_base = arena.offsets[7];
                const std::size_t forward_weights_base = arena.offsets[8];
                const std::size_t tile_plan_bytes = arena.total_bytes;
                if (tile_plan_bytes > std::numeric_limits<std::size_t>::max() - queued_plan_bytes) {
                    throw std::length_error("Metal queued plan working set overflow");
                }
                queued_plan_bytes += tile_plan_bytes;
                if (queued_plan_bytes > std::numeric_limits<std::size_t>::max()
                                            - persistent_metal_bytes) {
                    throw std::length_error("Metal total working set overflow");
                }
                {
                    const std::scoped_lock lock(impl_->mutex);
                    impl_->peak_working_set_bytes = std::max(
                        impl_->peak_working_set_bytes,
                        persistent_metal_bytes + queued_plan_bytes);
                }
                const std::size_t tile_partial_count = checked_product(
                    tile_candidate_count, groups, "Metal tile partial results");
                const std::size_t partial_offset_elements = checked_product(
                    tile.begin, groups, "Metal partial result offset");
                const std::size_t partial_offset_bytes = checked_product(
                    partial_offset_elements, sizeof(float), "Metal partial result byte offset");

                const auto make_command = [&](NSString *label) {
                    id<MTLCommandBuffer> command = [impl_->active_queue() commandBuffer];
                    if (command == nil) {
                        throw std::runtime_error("Metal command buffer creation failed");
                    }
                    command.label = label;
                    return command;
                };
                const auto encode_image_inverse = [&](id<MTLCommandBuffer> command,
                                                      id<MTLComputePipelineState> pipeline,
                                                      NSUInteger descriptor_offset,
                                                      AnalysisJob stage_job,
                                                      std::size_t dispatch_count) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> inverse = [command computeCommandEncoder];
                    if (inverse == nil) {
                        throw std::runtime_error("Metal inverse encoder creation failed");
                    }
                    inverse.label = @"GetNative inverse axis";
                    [inverse setComputePipelineState:pipeline];
                    impl_->record_buffer_wiring([&] {
                        [inverse setBuffer:source_buffer offset:0 atIndex:0];
                        [inverse setBytes:&stage_job length:sizeof(stage_job) atIndex:1];
                        [inverse setBuffer:plan_arena
                                   offset:descriptor_base + descriptor_offset atIndex:2];
                        [inverse setBuffer:plan_arena offset:transpose_offsets_base atIndex:3];
                        [inverse setBuffer:plan_arena offset:transpose_indices_base atIndex:4];
                        [inverse setBuffer:plan_arena offset:transpose_weights_base atIndex:5];
                        [inverse setBuffer:plan_arena offset:lower_base atIndex:6];
                        [inverse setBuffer:plan_arena offset:upper_base atIndex:7];
                        [inverse setBuffer:plan_arena offset:diagonal_base atIndex:8];
                        [inverse setBuffer:workspace_buffer offset:0 atIndex:9];
                    });
                    const NSUInteger threads = std::min<NSUInteger>(
                        static_cast<NSUInteger>(impl_->options.inverse_threads_per_threadgroup),
                        pipeline.maxTotalThreadsPerThreadgroup);
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(dispatch_count, "Metal inverse dispatch width"));
                    [inverse dispatchThreads:MTLSizeMake(dispatch_width, 1, 1)
                           threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
                    [inverse endEncoding];
                };
                const auto encode_matrix_inverse = [&](id<MTLCommandBuffer> command,
                                                       id<MTLComputePipelineState> pipeline,
                                                       NSUInteger descriptor_offset,
                                                       AnalysisJob stage_job,
                                                       std::size_t dispatch_count) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> inverse = [command computeCommandEncoder];
                    if (inverse == nil) throw std::runtime_error(
                        "Metal matrix inverse encoder creation "
                        "failed");
                    inverse.label = @"GetNative inverse vertical matrix";
                    [inverse setComputePipelineState:pipeline];
                    impl_->record_buffer_wiring([&] {
                        [inverse setBytes:&stage_job length:sizeof(stage_job) atIndex:0];
                        [inverse setBuffer:plan_arena
                                   offset:descriptor_base + descriptor_offset atIndex:1];
                        [inverse setBuffer:plan_arena offset:transpose_offsets_base atIndex:2];
                        [inverse setBuffer:plan_arena offset:transpose_indices_base atIndex:3];
                        [inverse setBuffer:plan_arena offset:transpose_weights_base atIndex:4];
                        [inverse setBuffer:plan_arena offset:lower_base atIndex:5];
                        [inverse setBuffer:plan_arena offset:upper_base atIndex:6];
                        [inverse setBuffer:plan_arena offset:diagonal_base atIndex:7];
                        [inverse setBuffer:workspace_buffer offset:0 atIndex:8];
                    });
                    const NSUInteger threads = std::min<NSUInteger>(
                        static_cast<NSUInteger>(impl_->options.inverse_threads_per_threadgroup),
                        pipeline.maxTotalThreadsPerThreadgroup);
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(dispatch_count, "Metal matrix inverse dispatch width"));
                    [inverse dispatchThreads:MTLSizeMake(dispatch_width, 1, 1)
                           threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
                    [inverse endEncoding];
                };
                const auto encode_matrix_forward = [&](id<MTLCommandBuffer> command,
                                                       id<MTLComputePipelineState> pipeline,
                                                       NSUInteger descriptor_offset,
                                                       AnalysisJob stage_job,
                                                       std::size_t dispatch_count) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> forward = [command computeCommandEncoder];
                    if (forward == nil) throw std::runtime_error(
                        "Metal matrix forward encoder creation "
                        "failed");
                    forward.label = @"GetNative forward first axis";
                    [forward setComputePipelineState:pipeline];
                    impl_->record_buffer_wiring([&] {
                        [forward setBytes:&stage_job length:sizeof(stage_job) atIndex:0];
                        [forward setBuffer:plan_arena
                                  offset:descriptor_base + descriptor_offset atIndex:1];
                        [forward setBuffer:plan_arena offset:forward_left_base atIndex:2];
                        [forward setBuffer:plan_arena offset:forward_weights_base atIndex:3];
                        [forward setBuffer:workspace_buffer offset:0 atIndex:4];
                    });
                    const NSUInteger threads = std::min<NSUInteger>(
                        static_cast<NSUInteger>(impl_->options.inverse_threads_per_threadgroup),
                        pipeline.maxTotalThreadsPerThreadgroup);
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(dispatch_count, "Metal matrix forward dispatch width"));
                    [forward dispatchThreads:MTLSizeMake(dispatch_width, 1, 1)
                           threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
                    [forward endEncoding];
                };
                const auto encode_reduction = [&](id<MTLCommandBuffer> command,
                                                  id<MTLComputePipelineState> pipeline,
                                                  NSUInteger descriptor_offset) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> reduction = [command computeCommandEncoder];
                    if (reduction == nil) {
                        throw std::runtime_error("Metal metric encoder creation failed");
                    }
                    reduction.label = @"GetNative p1 metric";
                    [reduction setComputePipelineState:pipeline];
                    impl_->record_buffer_wiring([&] {
                        [reduction setBuffer:source_buffer offset:0 atIndex:0];
                        [reduction setBytes:&job length:sizeof(job) atIndex:1];
                        [reduction setBuffer:plan_arena
                                    offset:descriptor_base + descriptor_offset atIndex:2];
                        [reduction setBuffer:plan_arena offset:forward_left_base atIndex:3];
                        [reduction setBuffer:plan_arena offset:forward_weights_base atIndex:4];
                        [reduction setBuffer:workspace_buffer offset:0 atIndex:5];
                        [reduction setBuffer:partial_buffer offset:partial_offset_bytes atIndex:6];
                    });
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(tile_partial_count, "Metal reduction dispatch width"));
                    [reduction dispatchThreadgroups:MTLSizeMake(dispatch_width, 1, 1)
                                 threadsPerThreadgroup:MTLSizeMake(reduction_width, 1, 1)];
                    [reduction endEncoding];
                };
                const auto submit = [&](id<MTLCommandBuffer> command) {
                    [command commit];
                    [commands addObject:command];
                    {
                        const std::scoped_lock lock(impl_->mutex);
                        ++impl_->command_buffer_submission_count;
                    }
                };

                if (tile.signature.axes != AnalysisAxes::both) {
                    const KernelShape inverse_shape = tile.signature.axes == AnalysisAxes::horizontal
                        ? tile.signature.horizontal_inverse_shape
                        : tile.signature.vertical_inverse_shape;
                    const KernelShape forward_shape = tile.signature.axes == AnalysisAxes::horizontal
                        ? tile.signature.horizontal_forward_shape
                        : tile.signature.vertical_forward_shape;
                    id<MTLComputePipelineState> inverse_pipeline = impl_->pipeline(
                        inverse_shape, PipelineStage::image_inverse);
                    id<MTLComputePipelineState> metric_pipeline = impl_->pipeline(
                        forward_shape, PipelineStage::metric);
                    AnalysisJob inverse_job = job;
                    inverse_job.maximum_vector_count = packed.maximum_vector_count;
                    const std::size_t inverse_count = checked_product(
                        tile_candidate_count, packed.maximum_vector_count, "Metal inverse dispatch");
                    if (impl_->options.profile_split_kernels) {
                        id<MTLCommandBuffer> inverse_command = make_command(@"GetNative inverse tile");
                        encode_image_inverse(
                            inverse_command, inverse_pipeline, 0, inverse_job, inverse_count);
                        submit(inverse_command);
                        id<MTLCommandBuffer> metric_command = make_command(@"GetNative metric tile");
                        encode_reduction(metric_command, metric_pipeline, 0);
                        submit(metric_command);
                    } else {
                        id<MTLCommandBuffer> command = make_command(@"GetNative axis analysis tile");
                        encode_image_inverse(
                            command, inverse_pipeline, 0, inverse_job, inverse_count);
                        encode_reduction(command, metric_pipeline, 0);
                        submit(command);
                    }
                } else {
                    const NSUInteger vertical_descriptor_offset = static_cast<NSUInteger>(
                        checked_product(tile_candidate_count, sizeof(AxisPlanDescriptor),
                                        "vertical descriptor offset"));
                    id<MTLComputePipelineState> horizontal_inverse_pipeline = impl_->pipeline(
                        tile.signature.horizontal_inverse_shape, PipelineStage::image_inverse);
                    id<MTLComputePipelineState> vertical_inverse_pipeline = impl_->pipeline(
                        tile.signature.vertical_inverse_shape, PipelineStage::matrix_inverse);
                    AnalysisJob horizontal_inverse_job = job;
                    horizontal_inverse_job.maximum_vector_count = static_cast<std::uint32_t>(source.height);
                    AnalysisJob vertical_inverse_job = job;
                    vertical_inverse_job.maximum_vector_count = packed.maximum_native_width;
                    AnalysisJob forward_job = job;
                    forward_job.maximum_vector_count =
                        tile.signature.forward_order == ForwardOrder::vertical_first
                        ? packed.maximum_native_width : packed.maximum_native_height;
                    const std::size_t horizontal_inverse_count = checked_product(
                        tile_candidate_count, static_cast<std::size_t>(source.height),
                        "two-axis horizontal inverse dispatch");
                    const std::size_t vertical_inverse_count = checked_product(
                        tile_candidate_count, packed.maximum_native_width,
                        "two-axis vertical inverse dispatch");
                    const std::size_t forward_count = checked_product(
                        tile_candidate_count, forward_job.maximum_vector_count,
                        "two-axis first forward dispatch");
                    const bool vertical_first =
                        tile.signature.forward_order == ForwardOrder::vertical_first;
                    const KernelShape first_forward_shape = vertical_first
                        ? tile.signature.vertical_forward_shape
                        : tile.signature.horizontal_forward_shape;
                    id<MTLComputePipelineState> first_forward_pipeline = impl_->pipeline(
                        first_forward_shape, PipelineStage::matrix_forward);
                    const NSUInteger first_forward_descriptor_offset =
                        vertical_first ? vertical_descriptor_offset : 0;
                    id<MTLComputePipelineState> final_metric = vertical_first
                        ? impl_->pipeline(tile.signature.horizontal_forward_shape,
                                          PipelineStage::metric)
                        : impl_->pipeline(tile.signature.vertical_forward_shape,
                                          PipelineStage::horizontal_first_metric);
                    const NSUInteger final_metric_descriptor_offset =
                        vertical_first ? 0 : vertical_descriptor_offset;

                    if (impl_->options.profile_split_kernels) {
                        id<MTLCommandBuffer> h_inverse = make_command(@"GetNative two-axis inverse H");
                        encode_image_inverse(h_inverse, horizontal_inverse_pipeline, 0,
                                             horizontal_inverse_job, horizontal_inverse_count);
                        submit(h_inverse);
                        id<MTLCommandBuffer> v_inverse = make_command(@"GetNative two-axis inverse V");
                        encode_matrix_inverse(v_inverse, vertical_inverse_pipeline,
                                              vertical_descriptor_offset, vertical_inverse_job,
                                              vertical_inverse_count);
                        submit(v_inverse);
                        id<MTLCommandBuffer> first_forward = make_command(@"GetNative two-axis forward first");
                        encode_matrix_forward(first_forward, first_forward_pipeline,
                                              first_forward_descriptor_offset, forward_job,
                                              forward_count);
                        submit(first_forward);
                        id<MTLCommandBuffer> metric_command = make_command(@"GetNative two-axis metric final");
                        encode_reduction(metric_command, final_metric,
                                         final_metric_descriptor_offset);
                        submit(metric_command);
                    } else {
                        id<MTLCommandBuffer> command = make_command(@"GetNative two-axis analysis tile");
                        encode_image_inverse(command, horizontal_inverse_pipeline, 0,
                                             horizontal_inverse_job, horizontal_inverse_count);
                        encode_matrix_inverse(command, vertical_inverse_pipeline,
                                              vertical_descriptor_offset, vertical_inverse_job,
                                              vertical_inverse_count);
                        encode_matrix_forward(command, first_forward_pipeline,
                                              first_forward_descriptor_offset, forward_job,
                                              forward_count);
                        encode_reduction(command, final_metric, final_metric_descriptor_offset);
                        submit(command);
                    }
                }
            }
            ++queued_tiles;
            if (queued_tiles == maximum_queued_tiles || tile.end == candidates.size()) {
                drain_commands();
                queued_tiles = 0;
                queued_plan_bytes = 0;
                if (stop.stop_requested()) {
                    throw std::runtime_error("Metal analysis cancelled");
                }
            }
        };

        try {
            for (const TileRange &tile : tiles) {
                process_tile(tile);
            }
        } catch (...) {
            const std::exception_ptr original = std::current_exception();
            try {
                drain_commands();
            } catch (...) {
                // Draining is mandatory, but the initiating failure owns the API result.
            }
            std::rethrow_exception(original);
        }

        const float *partials = static_cast<const float *>(partial_buffer.contents);
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            double sum = 0.0;
            for (std::size_t part = 0; part < groups; ++part) {
                sum += static_cast<double>(partials[index * groups + part]);
            }
            const double mean = sum / pixel_count;
            results[index] = {candidates[index].id,
                              metric.norm == 1U
                                  ? mean
                                  : std::pow(mean, 1.0 / static_cast<double>(metric.norm))};
        }
        return results;
    }
}

std::vector<CandidateResult> MetalAnalysisEngine::analyze_axis_batch_metal_luma(
    const MetalLumaFrameView &source,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    if (source.pixel_buffer == 0U || source.width <= 0 || source.height <= 0) {
        throw std::invalid_argument("metal_zero_copy_unsupported: invalid CVPixelBuffer");
    }
    if (source.surface_format != "420v" && source.surface_format != "420f"
        && source.surface_format != "x420" && source.surface_format != "xf20") {
        throw std::invalid_argument("metal_zero_copy_unsupported: unsupported CVPixelBuffer surface");
    }
    if (stop.stop_requested()) {
        throw std::runtime_error("Metal analysis cancelled");
    }
    std::unique_ptr<MetalExecutionSlot, std::function<void(MetalExecutionSlot *)>> slot_guard(
        nullptr, [](MetalExecutionSlot *) {});
    if (active_slot == nullptr) {
        const auto slot_wait_start = std::chrono::steady_clock::now();
        const std::size_t slot_index = impl_->acquire_slot(stop);
        {
            const std::scoped_lock lock(impl_->mutex);
            impl_->execution_slot_wait_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - slot_wait_start).count();
        }
        active_slot = impl_->slots[slot_index].get();
        slot_guard = decltype(slot_guard)(active_slot,
            [impl = impl_.get(), slot_index](MetalExecutionSlot *) {
                active_slot = nullptr;
                impl->release_slot(slot_index);
            });
    }
    @autoreleasepool {
        CVPixelBufferRef pixel_buffer = reinterpret_cast<CVPixelBufferRef>(source.pixel_buffer);
        if (CVPixelBufferGetIOSurface(pixel_buffer) == nullptr) {
            throw std::runtime_error("metal_zero_copy_unsupported: CVPixelBuffer has no IOSurface backing");
        }
        const bool ten_bit = source.bit_depth > 8;
        const OSType format = CVPixelBufferGetPixelFormatType(pixel_buffer);
        const bool format_is_10_bit =
            format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
            || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        if (format_is_10_bit != ten_bit) {
            throw std::runtime_error("metal_zero_copy_unsupported: CVPixelBuffer bit depth mismatch");
        }
        const bool full_range =
            format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        const std::string_view expected_surface = ten_bit
            ? (full_range ? "xf20" : "x420")
            : (full_range ? "420f" : "420v");
        if (source.surface_format != expected_surface) {
            throw std::runtime_error("metal_zero_copy_unsupported: CVPixelBuffer surface metadata mismatch");
        }
        CVMetalTextureCacheRef cache = nullptr;
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr,
                                      impl_->device, nullptr, &cache) != kCVReturnSuccess) {
            throw std::runtime_error("metal_zero_copy_unsupported: texture cache creation failed");
        }
        const std::size_t width = static_cast<std::size_t>(source.width);
        const std::size_t height = static_cast<std::size_t>(source.height);
        const std::size_t elements = checked_product(width, height, "Metal luma frame");
        id<MTLBuffer> normalized = impl_->allocate_empty_buffer(
            checked_product(elements, sizeof(float), "Metal normalized luma"),
            @"GetNative normalized luma", true);
        CVMetalTextureRef cv_texture = nullptr;
        const MTLPixelFormat texture_format = ten_bit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
        const CVReturn texture_result = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pixel_buffer, nullptr, texture_format,
            width, height, 0, &cv_texture);
        if (texture_result != kCVReturnSuccess || cv_texture == nullptr) {
            CFRelease(cache);
            throw std::runtime_error("metal_zero_copy_unsupported: luma texture import failed");
        }
        id<MTLCommandBuffer> command = [active_slot->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = command == nil ? nil : [command computeCommandEncoder];
        if (encoder == nil) {
            CFRelease(cv_texture); CFRelease(cache);
            throw std::runtime_error("metal_zero_copy_unsupported: normalization encoder creation failed");
        }
        LumaNormalizeJob normalize_job{
            static_cast<std::uint32_t>(source.width),
            static_cast<std::uint32_t>(source.height),
            static_cast<std::uint32_t>(source.bit_depth),
            static_cast<std::uint32_t>(source.surface_format == "420f"
                                       || source.surface_format == "xf20")};
        [encoder setComputePipelineState:ten_bit ? impl_->luma_normalize_r16
                                                  : impl_->luma_normalize_r8];
        [encoder setTexture:CVMetalTextureGetTexture(cv_texture) atIndex:0];
        [encoder setBuffer:normalized offset:0 atIndex:0];
        [encoder setBytes:&normalize_job length:sizeof(normalize_job) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(width, height, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        CFRelease(cv_texture); CFRelease(cache);
        if (command.status == MTLCommandBufferStatusError) {
            throw std::runtime_error("metal_zero_copy_unsupported: luma normalization failed");
        }
        struct ExternalSourceReset {
            MetalExecutionSlot *slot;
            ~ExternalSourceReset() {
                slot->external_source_buffer = nil;
                slot->external_source_bytes = 0;
            }
        } reset{active_slot};
        active_slot->external_source_buffer = normalized;
        active_slot->external_source_bytes = elements * sizeof(float);
        ConstImageView view{
            reinterpret_cast<const float *>(std::uintptr_t{1}),
            source.width, source.height, source.width};
        return analyze_axis_batch_f32(view, candidates, metric, stop);
    }
}

} // namespace getnative

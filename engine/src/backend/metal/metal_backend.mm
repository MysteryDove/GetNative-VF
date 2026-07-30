#include "getnative/metal_analysis.hpp"

#include "getnative_metallib.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <bit>
#include <cmath>
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
    generic,
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
};

struct MetricCropBounds {
    std::uint32_t left;
    std::uint32_t right;
    std::uint32_t top;
    std::uint32_t bottom;
    double pixel_count;
};

static_assert(sizeof(AxisPlanDescriptor) == 64);
static_assert(sizeof(AnalysisJob) == 40);

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
[[nodiscard]] id<MTLBuffer> make_buffer(id<MTLDevice> device,
                                        const std::vector<T> &values,
                                        std::string_view name) {
    if (values.empty()) {
        throw std::invalid_argument(std::string{name} + " must not be empty");
    }
    const std::size_t bytes = checked_product(values.size(), sizeof(T), name);
    id<MTLBuffer> buffer = [device newBufferWithBytes:values.data()
                                               length:bytes
                                              options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw std::runtime_error("Metal could not allocate " + std::string{name});
    }
    buffer.label = [NSString stringWithUTF8String:std::string{name}.c_str()];
    return buffer;
}

[[nodiscard]] id<MTLBuffer> make_empty_buffer(id<MTLDevice> device,
                                              std::size_t elements,
                                              std::string_view name) {
    const std::size_t bytes = checked_product(elements, sizeof(float), name);
    if (bytes == 0) {
        throw std::invalid_argument(std::string{name} + " must not be empty");
    }
    id<MTLBuffer> buffer = [device newBufferWithLength:bytes
                                               options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw std::runtime_error("Metal could not allocate " + std::string{name});
    }
    buffer.label = [NSString stringWithUTF8String:std::string{name}.c_str()];
    return buffer;
}

struct PackedTile {
    std::vector<AxisPlanDescriptor> descriptors;
    std::vector<std::uint32_t> transpose_offsets;
    std::vector<std::uint32_t> transpose_indices;
    std::vector<float> transpose_weights;
    std::vector<float> lower_ld;
    std::vector<float> upper_l;
    std::vector<float> inverse_diagonal;
    std::vector<std::int32_t> forward_left;
    std::vector<float> forward_weights;
    std::size_t workspace_elements = 0;
    std::uint32_t maximum_vector_count = 0;
    std::uint32_t maximum_native_width = 0;
    std::uint32_t maximum_native_height = 0;
};

struct TileSignature {
    AnalysisAxes axes = AnalysisAxes::vertical;
    KernelShape horizontal_shape = KernelShape::generic;
    KernelShape vertical_shape = KernelShape::generic;
    ForwardOrder forward_order = ForwardOrder::vertical_first;

    friend bool operator==(const TileSignature &, const TileSignature &) = default;
};

struct TileRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t workspace_elements = 0;
    TileSignature signature{};
};

[[nodiscard]] KernelShape axis_shape(const std::shared_ptr<const AxisPlan> &plan_pointer,
                                     std::int32_t expected_source) {
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
    if (plan.half_bandwidth == 1 && plan.forward_width == 2) {
        return KernelShape::bandwidth3;
    }
    if (plan.half_bandwidth == 3 && plan.forward_width == 4) {
        return KernelShape::bandwidth7;
    }
    return KernelShape::generic;
}

[[nodiscard]] TileSignature candidate_signature(ConstImageView source,
                                                const CandidateAnalysis &candidate) {
    TileSignature signature;
    signature.axes = candidate.axes;
    if (candidate.axes == AnalysisAxes::horizontal || candidate.axes == AnalysisAxes::both) {
        signature.horizontal_shape = axis_shape(candidate.horizontal, source.width);
    }
    if (candidate.axes == AnalysisAxes::vertical || candidate.axes == AnalysisAxes::both) {
        signature.vertical_shape = axis_shape(candidate.vertical, source.height);
    }
    if (candidate.axes == AnalysisAxes::both) {
        signature.forward_order = select_forward_order(*candidate.horizontal, *candidate.vertical);
    }
    return signature;
}

[[nodiscard]] std::size_t candidate_workspace_elements(ConstImageView source,
                                                        const CandidateAnalysis &candidate) {
    (void)candidate_signature(source, candidate);
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

    packed.transpose_offsets.insert(packed.transpose_offsets.end(),
                                    plan.transpose_offsets.begin(), plan.transpose_offsets.end());
    for (const std::int32_t index : plan.transpose_indices) {
        if (index < 0 || index >= plan.source_size) {
            throw std::invalid_argument("Metal transpose index is outside the source axis");
        }
        packed.transpose_indices.push_back(static_cast<std::uint32_t>(index));
    }
    packed.transpose_weights.insert(packed.transpose_weights.end(),
                                    plan.transpose_weights.begin(), plan.transpose_weights.end());
    packed.lower_ld.insert(packed.lower_ld.end(), plan.lower_ld.begin(), plan.lower_ld.end());
    packed.upper_l.insert(packed.upper_l.end(), plan.upper_l.begin(), plan.upper_l.end());
    packed.inverse_diagonal.insert(packed.inverse_diagonal.end(),
                                   plan.inverse_diagonal.begin(), plan.inverse_diagonal.end());
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
        packed.forward_weights.insert(
            packed.forward_weights.end(),
            plan.forward_weights.begin() + static_cast<std::ptrdiff_t>(begin),
            plan.forward_weights.begin() + static_cast<std::ptrdiff_t>(end));
    }
    packed.descriptors.push_back(descriptor);
}

void append_single_plan(PackedTile &packed, ConstImageView source,
                        const CandidateAnalysis &candidate) {
    (void)candidate_signature(source, candidate);
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
                           std::span<const CandidateAnalysis> candidates) {
    struct Bases { std::uint32_t intermediate; std::uint32_t native; };
    std::vector<Bases> bases;
    bases.reserve(candidates.size());
    for (const CandidateAnalysis &candidate : candidates) {
        (void)candidate_signature(source, candidate);
        const std::size_t candidate_elements = candidate_workspace_elements(source, candidate);
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
    id<MTLComputePipelineState> inverse;
    id<MTLComputePipelineState> metric;
    id<MTLComputePipelineState> matrix_inverse;
    id<MTLComputePipelineState> matrix_forward;
    id<MTLComputePipelineState> horizontal_first_metric;
};

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
        if (options.reduction_groups_per_candidate > maximum_reduction_groups) {
            throw std::invalid_argument("Metal reduction group count exceeds the 32-bit schedule range");
        }
        @autoreleasepool {
            device = MTLCreateSystemDefaultDevice();
            if (device == nil) {
                throw std::runtime_error("no Metal device is available");
            }
            queue = [device newCommandQueue];
            if (queue == nil) {
                throw std::runtime_error("Metal command queue creation failed");
            }

            dispatch_data_t library_data = dispatch_data_create(
                getnative_metallib, getnative_metallib_size,
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithData:library_data error:&error];
            if (library == nil) {
                throw std::runtime_error(ns_error(error, "embedded Metal library could not be loaded"));
            }
            bandwidth3 = {
                make_pipeline(device, library, @"inverse_axis_b3"),
                make_pipeline(device, library, @"metric_axis_p1_b3"),
                make_pipeline(device, library, @"inverse_axis_matrix_b3"),
                make_pipeline(device, library, @"forward_axis_matrix_b3"),
                make_pipeline(device, library, @"metric_axis_p1_horizontal_first_b3"),
            };
            bandwidth7 = {
                make_pipeline(device, library, @"inverse_axis_b7"),
                make_pipeline(device, library, @"metric_axis_p1_b7"),
                make_pipeline(device, library, @"inverse_axis_matrix_b7"),
                make_pipeline(device, library, @"forward_axis_matrix_b7"),
                make_pipeline(device, library, @"metric_axis_p1_horizontal_first_b7"),
            };
            generic = {
                make_pipeline(device, library, @"inverse_axis_generic"),
                make_pipeline(device, library, @"metric_axis_p1_generic"),
                make_pipeline(device, library, @"inverse_axis_matrix_generic"),
                make_pipeline(device, library, @"forward_axis_matrix_generic"),
                make_pipeline(device, library, @"metric_axis_p1_horizontal_first_generic"),
            };
            if (bandwidth3.metric.maxTotalThreadsPerThreadgroup < reduction_width
                || bandwidth7.metric.maxTotalThreadsPerThreadgroup < reduction_width
                || generic.metric.maxTotalThreadsPerThreadgroup < reduction_width) {
                throw std::runtime_error("Metal device cannot run the 256-thread reduction kernels");
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
    ShapePipelines bandwidth3;
    ShapePipelines bandwidth7;
    ShapePipelines generic;
    std::size_t peak_workspace_elements = 0;
    std::size_t peak_working_set_bytes = 0;
    std::mutex mutex;

    [[nodiscard]] const ShapePipelines &pipelines(KernelShape shape) const noexcept {
        switch (shape) {
        case KernelShape::bandwidth3: return bandwidth3;
        case KernelShape::bandwidth7: return bandwidth7;
        case KernelShape::generic: return generic;
        }
        return generic;
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

std::vector<CandidateResult> MetalAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::stop_token stop) {
    const std::scoped_lock call_lock(impl_->mutex);
    if (source.data == nullptr || source.width <= 0 || source.height <= 0
        || source.stride < source.width) {
        throw std::invalid_argument("invalid Metal source image");
    }
    const auto crop_bounds = metric_crop_bounds(source, metric);
    if (metric.norm != 1U) {
        throw std::invalid_argument("Metal currently supports only p=1 metrics");
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
    std::vector<float> contiguous_source;
    const float *source_data = source.data;
    if (source.stride != source.width) {
        contiguous_source.resize(image_elements);
        for (std::int32_t y = 0; y < source.height; ++y) {
            std::copy_n(source.data + static_cast<std::ptrdiff_t>(y) * source.stride,
                        source.width,
                        contiguous_source.data() + static_cast<std::ptrdiff_t>(y) * source.width);
        }
        source_data = contiguous_source.data();
    }

    @autoreleasepool {
        const std::size_t source_bytes = checked_product(
            image_elements, sizeof(float), "Metal source buffer");
        if (source_bytes > impl_->info.maximum_buffer_bytes) {
            throw std::length_error("Metal source image exceeds the device buffer limit");
        }
        id<MTLBuffer> source_buffer = [impl_->device newBufferWithBytes:source_data
                                                                length:source_bytes
                                                               options:MTLResourceStorageModeShared];
        if (source_buffer == nil) {
            throw std::runtime_error("Metal source buffer allocation failed");
        }
        source_buffer.label = @"GetNative source";

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
            const TileSignature signature = candidate_signature(source, candidates[tile_begin]);
            const std::size_t maximum_tile_end = tile_begin + std::min(
                impl_->options.tile_size, candidates.size() - tile_begin);
            while (tile_end < maximum_tile_end) {
                const CandidateAnalysis &candidate = candidates[tile_end];
                if (candidate_signature(source, candidate) != signature) {
                    break;
                }
                const std::size_t candidate_workspace =
                    candidate_workspace_elements(source, candidate);
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

        impl_->peak_workspace_elements = std::max(
            impl_->peak_workspace_elements, maximum_workspace_elements);
        id<MTLBuffer> workspace_buffer = make_empty_buffer(
            impl_->device, maximum_workspace_elements, "GetNative Metal workspace");
        const std::size_t partial_count = checked_product(
            candidates.size(), groups, "Metal partial results");
        id<MTLBuffer> partial_buffer = make_empty_buffer(
            impl_->device, partial_count, "GetNative metric partials");
        const std::size_t commands_per_tile = impl_->options.profile_split_kernels ? 4 : 1;
        NSMutableArray<id<MTLCommandBuffer>> *commands =
            [NSMutableArray arrayWithCapacity:std::min(tiles.size(), maximum_queued_tiles)
                                               * commands_per_tile];
        std::size_t queued_tiles = 0;
        std::size_t queued_plan_bytes = 0;
        const std::size_t workspace_buffer_bytes = static_cast<std::size_t>(workspace_buffer.length);
        const std::size_t partial_buffer_bytes = static_cast<std::size_t>(partial_buffer.length);
        if (source_bytes > std::numeric_limits<std::size_t>::max() - workspace_buffer_bytes
            || source_bytes + workspace_buffer_bytes
                > std::numeric_limits<std::size_t>::max() - partial_buffer_bytes) {
            throw std::length_error("Metal persistent working set overflow");
        }
        const std::size_t persistent_metal_bytes =
            source_bytes + workspace_buffer_bytes + partial_buffer_bytes;
        impl_->peak_working_set_bytes = std::max(
            impl_->peak_working_set_bytes, persistent_metal_bytes);

        for (const TileRange &tile : tiles) {
            if (stop.stop_requested()) {
                throw std::runtime_error("Metal analysis cancelled");
            }
            @autoreleasepool {
                PackedTile packed;
                const std::size_t tile_candidate_count = tile.end - tile.begin;
                packed.descriptors.reserve(
                    tile_candidate_count * (tile.signature.axes == AnalysisAxes::both ? 2U : 1U));
                if (tile.signature.axes == AnalysisAxes::both) {
                    append_two_axis_plans(
                        packed, source, candidates.subspan(tile.begin, tile_candidate_count));
                } else {
                    for (std::size_t index = tile.begin; index < tile.end; ++index) {
                        append_single_plan(packed, source, candidates[index]);
                    }
                }
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
                };

                id<MTLBuffer> descriptor_buffer = make_buffer(
                    impl_->device, packed.descriptors, "GetNative plan descriptors");
                id<MTLBuffer> transpose_offset_buffer = make_buffer(
                    impl_->device, packed.transpose_offsets, "GetNative transpose offsets");
                id<MTLBuffer> transpose_index_buffer = make_buffer(
                    impl_->device, packed.transpose_indices, "GetNative transpose indices");
                id<MTLBuffer> transpose_weight_buffer = make_buffer(
                    impl_->device, packed.transpose_weights, "GetNative transpose weights");
                id<MTLBuffer> lower_buffer = make_buffer(
                    impl_->device, packed.lower_ld, "GetNative lower factors");
                id<MTLBuffer> upper_buffer = make_buffer(
                    impl_->device, packed.upper_l, "GetNative upper factors");
                id<MTLBuffer> diagonal_buffer = make_buffer(
                    impl_->device, packed.inverse_diagonal, "GetNative inverse diagonal");
                id<MTLBuffer> forward_left_buffer = make_buffer(
                    impl_->device, packed.forward_left, "GetNative forward left indices");
                id<MTLBuffer> forward_weight_buffer = make_buffer(
                    impl_->device, packed.forward_weights, "GetNative forward weights");
                const std::size_t tile_plan_bytes =
                    static_cast<std::size_t>(descriptor_buffer.length)
                    + static_cast<std::size_t>(transpose_offset_buffer.length)
                    + static_cast<std::size_t>(transpose_index_buffer.length)
                    + static_cast<std::size_t>(transpose_weight_buffer.length)
                    + static_cast<std::size_t>(lower_buffer.length)
                    + static_cast<std::size_t>(upper_buffer.length)
                    + static_cast<std::size_t>(diagonal_buffer.length)
                    + static_cast<std::size_t>(forward_left_buffer.length)
                    + static_cast<std::size_t>(forward_weight_buffer.length);
                if (tile_plan_bytes > std::numeric_limits<std::size_t>::max() - queued_plan_bytes) {
                    throw std::length_error("Metal queued plan working set overflow");
                }
                queued_plan_bytes += tile_plan_bytes;
                if (queued_plan_bytes > std::numeric_limits<std::size_t>::max()
                                            - persistent_metal_bytes) {
                    throw std::length_error("Metal total working set overflow");
                }
                impl_->peak_working_set_bytes = std::max(
                    impl_->peak_working_set_bytes,
                    persistent_metal_bytes + queued_plan_bytes);
                const std::size_t tile_partial_count = checked_product(
                    tile_candidate_count, groups, "Metal tile partial results");
                const std::size_t partial_offset_elements = checked_product(
                    tile.begin, groups, "Metal partial result offset");
                const std::size_t partial_offset_bytes = checked_product(
                    partial_offset_elements, sizeof(float), "Metal partial result byte offset");

                const auto make_command = [&](NSString *label) {
                    id<MTLCommandBuffer> command = [impl_->queue commandBuffer];
                    if (command == nil) {
                        throw std::runtime_error("Metal command buffer creation failed");
                    }
                    command.label = label;
                    return command;
                };
                const auto encode_image_inverse = [&](id<MTLCommandBuffer> command,
                                                      const ShapePipelines &pipelines,
                                                      NSUInteger descriptor_offset,
                                                      AnalysisJob stage_job,
                                                      std::size_t dispatch_count) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> inverse = [command computeCommandEncoder];
                    if (inverse == nil) {
                        throw std::runtime_error("Metal inverse encoder creation failed");
                    }
                    inverse.label = @"GetNative inverse axis";
                    [inverse setComputePipelineState:pipelines.inverse];
                    [inverse setBuffer:source_buffer offset:0 atIndex:0];
                    [inverse setBytes:&stage_job length:sizeof(stage_job) atIndex:1];
                    [inverse setBuffer:descriptor_buffer offset:descriptor_offset atIndex:2];
                    [inverse setBuffer:transpose_offset_buffer offset:0 atIndex:3];
                    [inverse setBuffer:transpose_index_buffer offset:0 atIndex:4];
                    [inverse setBuffer:transpose_weight_buffer offset:0 atIndex:5];
                    [inverse setBuffer:lower_buffer offset:0 atIndex:6];
                    [inverse setBuffer:upper_buffer offset:0 atIndex:7];
                    [inverse setBuffer:diagonal_buffer offset:0 atIndex:8];
                    [inverse setBuffer:workspace_buffer offset:0 atIndex:9];
                    const NSUInteger threads = std::min<NSUInteger>(
                        static_cast<NSUInteger>(impl_->options.inverse_threads_per_threadgroup),
                        pipelines.inverse.maxTotalThreadsPerThreadgroup);
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(dispatch_count, "Metal inverse dispatch width"));
                    [inverse dispatchThreads:MTLSizeMake(dispatch_width, 1, 1)
                           threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
                    [inverse endEncoding];
                };
                const auto encode_matrix_inverse = [&](id<MTLCommandBuffer> command,
                                                       const ShapePipelines &pipelines,
                                                       NSUInteger descriptor_offset,
                                                       AnalysisJob stage_job,
                                                       std::size_t dispatch_count) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> inverse = [command computeCommandEncoder];
                    if (inverse == nil) throw std::runtime_error("Metal matrix inverse encoder creation failed");
                    inverse.label = @"GetNative inverse vertical matrix";
                    [inverse setComputePipelineState:pipelines.matrix_inverse];
                    [inverse setBytes:&stage_job length:sizeof(stage_job) atIndex:0];
                    [inverse setBuffer:descriptor_buffer offset:descriptor_offset atIndex:1];
                    [inverse setBuffer:transpose_offset_buffer offset:0 atIndex:2];
                    [inverse setBuffer:transpose_index_buffer offset:0 atIndex:3];
                    [inverse setBuffer:transpose_weight_buffer offset:0 atIndex:4];
                    [inverse setBuffer:lower_buffer offset:0 atIndex:5];
                    [inverse setBuffer:upper_buffer offset:0 atIndex:6];
                    [inverse setBuffer:diagonal_buffer offset:0 atIndex:7];
                    [inverse setBuffer:workspace_buffer offset:0 atIndex:8];
                    const NSUInteger threads = std::min<NSUInteger>(
                        static_cast<NSUInteger>(impl_->options.inverse_threads_per_threadgroup),
                        pipelines.matrix_inverse.maxTotalThreadsPerThreadgroup);
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(dispatch_count, "Metal matrix inverse dispatch width"));
                    [inverse dispatchThreads:MTLSizeMake(dispatch_width, 1, 1)
                           threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
                    [inverse endEncoding];
                };
                const auto encode_matrix_forward = [&](id<MTLCommandBuffer> command,
                                                       const ShapePipelines &pipelines,
                                                       NSUInteger descriptor_offset,
                                                       AnalysisJob stage_job,
                                                       std::size_t dispatch_count) {
                    if (stop.stop_requested()) throw std::runtime_error("Metal analysis cancelled");
                    id<MTLComputeCommandEncoder> forward = [command computeCommandEncoder];
                    if (forward == nil) throw std::runtime_error("Metal matrix forward encoder creation failed");
                    forward.label = @"GetNative forward first axis";
                    [forward setComputePipelineState:pipelines.matrix_forward];
                    [forward setBytes:&stage_job length:sizeof(stage_job) atIndex:0];
                    [forward setBuffer:descriptor_buffer offset:descriptor_offset atIndex:1];
                    [forward setBuffer:forward_left_buffer offset:0 atIndex:2];
                    [forward setBuffer:forward_weight_buffer offset:0 atIndex:3];
                    [forward setBuffer:workspace_buffer offset:0 atIndex:4];
                    const NSUInteger threads = std::min<NSUInteger>(
                        static_cast<NSUInteger>(impl_->options.inverse_threads_per_threadgroup),
                        pipelines.matrix_forward.maxTotalThreadsPerThreadgroup);
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
                    [reduction setBuffer:source_buffer offset:0 atIndex:0];
                    [reduction setBytes:&job length:sizeof(job) atIndex:1];
                    [reduction setBuffer:descriptor_buffer offset:descriptor_offset atIndex:2];
                    [reduction setBuffer:forward_left_buffer offset:0 atIndex:3];
                    [reduction setBuffer:forward_weight_buffer offset:0 atIndex:4];
                    [reduction setBuffer:workspace_buffer offset:0 atIndex:5];
                    [reduction setBuffer:partial_buffer offset:partial_offset_bytes atIndex:6];
                    const NSUInteger dispatch_width = static_cast<NSUInteger>(
                        checked_u32(tile_partial_count, "Metal reduction dispatch width"));
                    [reduction dispatchThreadgroups:MTLSizeMake(dispatch_width, 1, 1)
                                 threadsPerThreadgroup:MTLSizeMake(reduction_width, 1, 1)];
                    [reduction endEncoding];
                };
                const auto submit = [&](id<MTLCommandBuffer> command) {
                    [command commit];
                    [commands addObject:command];
                };

                if (tile.signature.axes != AnalysisAxes::both) {
                    const KernelShape shape = tile.signature.axes == AnalysisAxes::horizontal
                        ? tile.signature.horizontal_shape : tile.signature.vertical_shape;
                    const ShapePipelines &pipelines = impl_->pipelines(shape);
                    AnalysisJob inverse_job = job;
                    inverse_job.maximum_vector_count = packed.maximum_vector_count;
                    const std::size_t inverse_count = checked_product(
                        tile_candidate_count, packed.maximum_vector_count, "Metal inverse dispatch");
                    if (impl_->options.profile_split_kernels) {
                        id<MTLCommandBuffer> inverse_command = make_command(@"GetNative inverse tile");
                        encode_image_inverse(inverse_command, pipelines, 0, inverse_job, inverse_count);
                        submit(inverse_command);
                        id<MTLCommandBuffer> metric_command = make_command(@"GetNative metric tile");
                        encode_reduction(metric_command, pipelines.metric, 0);
                        submit(metric_command);
                    } else {
                        id<MTLCommandBuffer> command = make_command(@"GetNative axis analysis tile");
                        encode_image_inverse(command, pipelines, 0, inverse_job, inverse_count);
                        encode_reduction(command, pipelines.metric, 0);
                        submit(command);
                    }
                } else {
                    const NSUInteger vertical_descriptor_offset = static_cast<NSUInteger>(
                        checked_product(tile_candidate_count, sizeof(AxisPlanDescriptor),
                                        "vertical descriptor offset"));
                    const ShapePipelines &horizontal_pipelines =
                        impl_->pipelines(tile.signature.horizontal_shape);
                    const ShapePipelines &vertical_pipelines =
                        impl_->pipelines(tile.signature.vertical_shape);
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
                    const ShapePipelines &first_forward_pipelines =
                        vertical_first ? vertical_pipelines : horizontal_pipelines;
                    const NSUInteger first_forward_descriptor_offset =
                        vertical_first ? vertical_descriptor_offset : 0;
                    id<MTLComputePipelineState> final_metric = vertical_first
                        ? horizontal_pipelines.metric
                        : vertical_pipelines.horizontal_first_metric;
                    const NSUInteger final_metric_descriptor_offset =
                        vertical_first ? 0 : vertical_descriptor_offset;

                    if (impl_->options.profile_split_kernels) {
                        id<MTLCommandBuffer> h_inverse = make_command(@"GetNative two-axis inverse H");
                        encode_image_inverse(h_inverse, horizontal_pipelines, 0,
                                             horizontal_inverse_job, horizontal_inverse_count);
                        submit(h_inverse);
                        id<MTLCommandBuffer> v_inverse = make_command(@"GetNative two-axis inverse V");
                        encode_matrix_inverse(v_inverse, vertical_pipelines,
                                              vertical_descriptor_offset, vertical_inverse_job,
                                              vertical_inverse_count);
                        submit(v_inverse);
                        id<MTLCommandBuffer> first_forward = make_command(@"GetNative two-axis forward first");
                        encode_matrix_forward(first_forward, first_forward_pipelines,
                                              first_forward_descriptor_offset, forward_job,
                                              forward_count);
                        submit(first_forward);
                        id<MTLCommandBuffer> metric_command = make_command(@"GetNative two-axis metric final");
                        encode_reduction(metric_command, final_metric,
                                         final_metric_descriptor_offset);
                        submit(metric_command);
                    } else {
                        id<MTLCommandBuffer> command = make_command(@"GetNative two-axis analysis tile");
                        encode_image_inverse(command, horizontal_pipelines, 0,
                                             horizontal_inverse_job, horizontal_inverse_count);
                        encode_matrix_inverse(command, vertical_pipelines,
                                              vertical_descriptor_offset, vertical_inverse_job,
                                              vertical_inverse_count);
                        encode_matrix_forward(command, first_forward_pipelines,
                                              first_forward_descriptor_offset, forward_job,
                                              forward_count);
                        encode_reduction(command, final_metric, final_metric_descriptor_offset);
                        submit(command);
                    }
                }
            }
            ++queued_tiles;
            if (queued_tiles == maximum_queued_tiles || tile.end == candidates.size()) {
                for (id<MTLCommandBuffer> command in commands) {
                    [command waitUntilCompleted];
                    if (command.status == MTLCommandBufferStatusError) {
                        throw std::runtime_error(ns_error(
                            command.error, "Metal command execution failed"));
                    }
                    if (stop.stop_requested()) {
                        throw std::runtime_error("Metal analysis cancelled");
                    }
                }
                [commands removeAllObjects];
                queued_tiles = 0;
                queued_plan_bytes = 0;
            }
        }

        const float *partials = static_cast<const float *>(partial_buffer.contents);
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            double sum = 0.0;
            for (std::size_t part = 0; part < groups; ++part) {
                sum += static_cast<double>(partials[index * groups + part]);
            }
            results[index] = {candidates[index].id, sum / pixel_count};
        }
        return results;
    }
}

} // namespace getnative

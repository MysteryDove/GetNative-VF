#include "gpu_batch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace getnative::detail::gpu {
namespace {

constexpr std::uint32_t horizontal_axis = 0;
constexpr std::uint32_t vertical_axis = 1;

[[noreturn]] void invalid_plan(const char *detail) {
    throw std::invalid_argument(std::string{"GPU axis plan "} + detail);
}

void validate_finite(std::span<const float> values, const char *name) {
    if (!std::ranges::all_of(values, [](float value) { return std::isfinite(value); })) {
        invalid_plan(name);
    }
}

void validate_axis_plan(const AxisPlan &plan, std::int32_t expected_source) {
    if (!plan.valid()) invalid_plan("is invalid");
    if (plan.source_size != expected_source) {
        invalid_plan("does not match the source image");
    }
    if (plan.half_bandwidth < 1 || plan.half_bandwidth > maximum_half_bandwidth
        || plan.forward_width < 1 || plan.forward_width > maximum_forward_width) {
        invalid_plan("shape exceeds the supported B31/F16 limit");
    }

    if (plan.transpose_offsets.empty() || plan.transpose_offsets.front() != 0U
        || plan.transpose_offsets.back() != plan.transpose_indices.size()
        || plan.transpose_indices.size() != plan.transpose_weights.size()) {
        invalid_plan("has an inconsistent transpose table");
    }
    for (std::size_t row = 0; row + 1U < plan.transpose_offsets.size(); ++row) {
        const std::uint32_t begin = plan.transpose_offsets[row];
        const std::uint32_t end = plan.transpose_offsets[row + 1U];
        if (begin > end || end > plan.transpose_indices.size()) {
            invalid_plan("has a non-monotonic transpose offset table");
        }
    }
    for (const std::int32_t index : plan.transpose_indices) {
        if (index < 0 || index >= plan.source_size) {
            invalid_plan("contains an out-of-range transpose index");
        }
    }

    if (plan.forward_offsets.empty() || plan.forward_offsets.front() != 0U
        || plan.forward_offsets.back() != plan.forward_indices.size()
        || plan.forward_indices.size() != plan.forward_weights.size()) {
        invalid_plan("has an inconsistent forward table");
    }
    for (std::int32_t row = 0; row < plan.source_size; ++row) {
        const std::uint32_t begin = plan.forward_offsets[static_cast<std::size_t>(row)];
        const std::uint32_t end = plan.forward_offsets[static_cast<std::size_t>(row) + 1U];
        if (begin > end || end > plan.forward_indices.size()
            || end - begin != static_cast<std::uint32_t>(plan.forward_width)) {
            invalid_plan("has an invalid forward row offset");
        }
        const std::int32_t left = plan.forward_indices[begin];
        if (left < 0 || left > plan.destination_size - plan.forward_width) {
            invalid_plan("contains an out-of-range forward row");
        }
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            const auto tap_index = begin + static_cast<std::uint32_t>(tap);
            if (plan.forward_indices[tap_index] != left + tap) {
                invalid_plan("contains a non-contiguous forward row");
            }
        }
    }

    validate_finite(plan.transpose_weights, "contains a nonfinite transpose weight");
    validate_finite(plan.forward_weights, "contains a nonfinite forward weight");
    validate_finite(plan.lower_ld, "contains a nonfinite lower factor");
    validate_finite(plan.upper_l, "contains a nonfinite upper factor");
    validate_finite(plan.inverse_diagonal, "contains a nonfinite inverse diagonal");
}

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
        packed.inverse_diagonal.size(), "inverse diagonal base");
    descriptor.forward_left_base = checked_u32(
        packed.forward_left.size(), "forward left base");
    descriptor.forward_weights_base = checked_u32(
        packed.forward_weights.size(), "forward weight base");
    descriptor.workspace_base = workspace_base;
    descriptor.direction = horizontal ? horizontal_axis : vertical_axis;
    descriptor.vector_count = vector_count;
    descriptor.reserved_0 = reserved_0;
    descriptor.reserved_1 = reserved_1;

    packed.transpose_offsets.insert(packed.transpose_offsets.end(),
                                    plan.transpose_offsets.begin(),
                                    plan.transpose_offsets.end());
    for (const std::int32_t index : plan.transpose_indices) {
        packed.transpose_indices.push_back(static_cast<std::uint32_t>(index));
    }
    packed.transpose_weights.insert(packed.transpose_weights.end(),
                                    plan.transpose_weights.begin(),
                                    plan.transpose_weights.end());
    packed.lower_ld.insert(packed.lower_ld.end(), plan.lower_ld.begin(), plan.lower_ld.end());
    packed.upper_l.insert(packed.upper_l.end(), plan.upper_l.begin(), plan.upper_l.end());
    packed.inverse_diagonal.insert(packed.inverse_diagonal.end(),
                                   plan.inverse_diagonal.begin(),
                                   plan.inverse_diagonal.end());
    for (std::int32_t row = 0; row < plan.source_size; ++row) {
        const std::uint32_t begin = plan.forward_offsets[static_cast<std::size_t>(row)];
        const std::uint32_t end = plan.forward_offsets[static_cast<std::size_t>(row) + 1U];
        packed.forward_left.push_back(plan.forward_indices[begin]);
        packed.forward_weights.insert(
            packed.forward_weights.end(),
            plan.forward_weights.begin() + static_cast<std::ptrdiff_t>(begin),
            plan.forward_weights.begin() + static_cast<std::ptrdiff_t>(end));
    }
    packed.descriptors.push_back(descriptor);
}

void append_single_plan(PackedTile &packed, ConstImageView source,
                        const CandidateAnalysis &candidate) {
    const bool horizontal = candidate.axes == AnalysisAxes::horizontal;
    const auto &plan = horizontal ? candidate.horizontal : candidate.vertical;
    const std::uint32_t vector_count = static_cast<std::uint32_t>(
        horizontal ? source.height : source.width);
    const std::size_t candidate_workspace = checked_product(
        static_cast<std::size_t>(vector_count),
        static_cast<std::size_t>(plan->destination_size), "candidate workspace");
    if (candidate_workspace > std::numeric_limits<std::size_t>::max()
            - packed.workspace_elements) {
        throw std::length_error("GPU tile workspace size overflow");
    }
    append_axis(packed, *plan, horizontal, vector_count,
                checked_u32(packed.workspace_elements, "workspace base"));
    packed.workspace_elements += candidate_workspace;
    packed.maximum_vector_count = std::max(packed.maximum_vector_count, vector_count);
}

void append_two_axis_plans(PackedTile &packed, ConstImageView source,
                           std::span<const CandidateAnalysis> candidates,
                           KernelDispatchPolicy policy) {
    struct Bases {
        std::uint32_t intermediate;
        std::uint32_t native;
    };
    std::vector<Bases> bases;
    bases.reserve(candidates.size());
    for (const CandidateAnalysis &candidate : candidates) {
        const std::size_t candidate_elements =
            candidate_workspace_elements(source, candidate, policy);
        const std::size_t native_elements = checked_product(
            static_cast<std::size_t>(candidate.horizontal->destination_size),
            static_cast<std::size_t>(candidate.vertical->destination_size),
            "two-axis native workspace");
        if (candidate_elements > std::numeric_limits<std::size_t>::max()
                - packed.workspace_elements) {
            throw std::length_error("GPU tile workspace size overflow");
        }
        const std::size_t native_base =
            packed.workspace_elements + candidate_elements - native_elements;
        bases.push_back({
            checked_u32(packed.workspace_elements, "intermediate workspace base"),
            checked_u32(native_base, "native workspace base"),
        });
        packed.workspace_elements += candidate_elements;
        packed.maximum_native_width = std::max(
            packed.maximum_native_width,
            static_cast<std::uint32_t>(candidate.horizontal->destination_size));
        packed.maximum_native_height = std::max(
            packed.maximum_native_height,
            static_cast<std::uint32_t>(candidate.vertical->destination_size));
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const CandidateAnalysis &candidate = candidates[index];
        append_axis(packed, *candidate.horizontal, true,
                    static_cast<std::uint32_t>(source.height), bases[index].intermediate,
                    bases[index].native,
                    static_cast<std::uint32_t>(candidate.vertical->destination_size));
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const CandidateAnalysis &candidate = candidates[index];
        append_axis(packed, *candidate.vertical, false,
                    static_cast<std::uint32_t>(candidate.horizontal->destination_size),
                    bases[index].native, bases[index].intermediate);
    }
}

template <typename T>
[[nodiscard]] std::size_t vector_bytes(const std::vector<T> &values,
                                       const char *name) {
    return checked_product(values.size(), sizeof(T), name);
}

} // namespace

std::uint32_t checked_u32(std::size_t value, const char *name) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string{name} + " exceeds the GPU 32-bit index range");
    }
    return static_cast<std::uint32_t>(value);
}

std::size_t checked_product(std::size_t a, std::size_t b, const char *name) {
    if (a != 0U && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::length_error(std::string{name} + " size overflow");
    }
    return a * b;
}

MetricCropBounds validate_source_and_metric(ConstImageView source,
                                             const MetricSpec &metric) {
    if (source.data == nullptr || source.width <= 0 || source.height <= 0
        || source.stride < source.width) {
        throw std::invalid_argument("invalid GPU source image");
    }
    (void)checked_u32(static_cast<std::size_t>(source.width), "source width");
    (void)checked_u32(static_cast<std::size_t>(source.height), "source height");
    if (metric.crop_left < 0 || metric.crop_right < 0 || metric.crop_top < 0
        || metric.crop_bottom < 0 || !std::isfinite(metric.threshold)
        || metric.threshold < 0.0F || metric.norm != 1U) {
        throw std::invalid_argument("invalid GPU metric configuration");
    }
    const auto cropped_width = static_cast<std::int64_t>(source.width)
        - metric.crop_left - metric.crop_right;
    const auto cropped_height = static_cast<std::int64_t>(source.height)
        - metric.crop_top - metric.crop_bottom;
    if (cropped_width <= 0 || cropped_height <= 0) {
        throw std::invalid_argument("invalid GPU metric crop");
    }
    return {
        static_cast<std::uint32_t>(metric.crop_left),
        static_cast<std::uint32_t>(metric.crop_right),
        static_cast<std::uint32_t>(metric.crop_top),
        static_cast<std::uint32_t>(metric.crop_bottom),
        static_cast<double>(cropped_width) * static_cast<double>(cropped_height),
    };
}

AxisKernelShapes axis_shapes(const AxisPlan &plan, KernelDispatchPolicy policy) {
    if (policy == KernelDispatchPolicy::generic_only) return {};
    const AxisKernelShapes shapes{
        inverse_shape(plan.half_bandwidth), forward_shape(plan.forward_width),
    };
    if (policy == KernelDispatchPolicy::required_specialized
        && (shapes.inverse == KernelShape::generic
            || shapes.forward == KernelShape::generic)) {
        throw std::runtime_error("required GPU B/F specialization is unavailable");
    }
    return shapes;
}

TileSignature candidate_signature(ConstImageView source,
                                  const CandidateAnalysis &candidate,
                                  KernelDispatchPolicy policy) {
    TileSignature signature;
    signature.axes = candidate.axes;
    switch (candidate.axes) {
    case AnalysisAxes::horizontal: {
        if (!candidate.horizontal) invalid_plan("pointer is null");
        validate_axis_plan(*candidate.horizontal, source.width);
        const AxisKernelShapes shapes = axis_shapes(*candidate.horizontal, policy);
        signature.horizontal_inverse_shape = shapes.inverse;
        signature.horizontal_forward_shape = shapes.forward;
        break;
    }
    case AnalysisAxes::vertical: {
        if (!candidate.vertical) invalid_plan("pointer is null");
        validate_axis_plan(*candidate.vertical, source.height);
        const AxisKernelShapes shapes = axis_shapes(*candidate.vertical, policy);
        signature.vertical_inverse_shape = shapes.inverse;
        signature.vertical_forward_shape = shapes.forward;
        break;
    }
    case AnalysisAxes::both: {
        if (!candidate.horizontal || !candidate.vertical) invalid_plan("pointer is null");
        validate_axis_plan(*candidate.horizontal, source.width);
        validate_axis_plan(*candidate.vertical, source.height);
        const AxisKernelShapes horizontal = axis_shapes(*candidate.horizontal, policy);
        const AxisKernelShapes vertical = axis_shapes(*candidate.vertical, policy);
        signature.horizontal_inverse_shape = horizontal.inverse;
        signature.horizontal_forward_shape = horizontal.forward;
        signature.vertical_inverse_shape = vertical.inverse;
        signature.vertical_forward_shape = vertical.forward;
        signature.forward_order = select_forward_order(
            *candidate.horizontal, *candidate.vertical);
        break;
    }
    default:
        throw std::invalid_argument("invalid GPU analysis axis mode");
    }
    return signature;
}

bool uses_specialized_pipeline(const TileSignature &signature) noexcept {
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

std::size_t candidate_workspace_elements(ConstImageView source,
                                         const CandidateAnalysis &candidate,
                                         KernelDispatchPolicy policy) {
    (void)candidate_signature(source, candidate, policy);
    if (candidate.axes != AnalysisAxes::both) {
        const auto &plan = candidate.axes == AnalysisAxes::horizontal
            ? candidate.horizontal : candidate.vertical;
        const std::size_t vectors = static_cast<std::size_t>(
            candidate.axes == AnalysisAxes::horizontal ? source.height : source.width);
        return checked_product(vectors,
                               static_cast<std::size_t>(plan->destination_size),
                               "candidate workspace");
    }
    const std::size_t native_width =
        static_cast<std::size_t>(candidate.horizontal->destination_size);
    const std::size_t native_height =
        static_cast<std::size_t>(candidate.vertical->destination_size);
    const std::size_t native = checked_product(
        native_width, native_height, "two-axis native workspace");
    const std::size_t inverse_intermediate = checked_product(
        native_width, static_cast<std::size_t>(source.height),
        "two-axis inverse intermediate");
    const std::size_t horizontal_first_intermediate = checked_product(
        static_cast<std::size_t>(source.width), native_height,
        "two-axis forward intermediate");
    const std::size_t intermediate =
        std::max(inverse_intermediate, horizontal_first_intermediate);
    if (native > std::numeric_limits<std::size_t>::max() - intermediate) {
        throw std::length_error("two-axis candidate workspace size overflow");
    }
    return intermediate + native;
}

TiledBatch plan_tiles(ConstImageView source,
                      std::span<const CandidateAnalysis> candidates,
                      std::size_t tile_size,
                      std::size_t workspace_limit_elements,
                      KernelDispatchPolicy policy,
                      std::stop_token stop) {
    if (tile_size == 0U || workspace_limit_elements == 0U) {
        throw std::invalid_argument("GPU tile and workspace limits must be positive");
    }
    const std::size_t effective_workspace_limit = std::min(
        workspace_limit_elements,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
    TiledBatch result;
    result.tiles.reserve((candidates.size() + tile_size - 1U) / tile_size);
    std::size_t tile_begin = 0U;
    while (tile_begin < candidates.size()) {
        if (stop.stop_requested()) throw std::runtime_error("GPU analysis cancelled");
        const TileSignature signature = candidate_signature(
            source, candidates[tile_begin], policy);
        const std::size_t maximum_tile_end = tile_begin
            + std::min(tile_size, candidates.size() - tile_begin);
        std::size_t tile_end = tile_begin;
        std::size_t workspace_elements = 0U;
        while (tile_end < maximum_tile_end) {
            if (stop.stop_requested()) throw std::runtime_error("GPU analysis cancelled");
            const CandidateAnalysis &candidate = candidates[tile_end];
            if (candidate_signature(source, candidate, policy) != signature) break;
            const std::size_t candidate_workspace = candidate_workspace_elements(
                source, candidate, policy);
            if (candidate_workspace > effective_workspace_limit) {
                throw std::length_error("one GPU candidate exceeds the workspace limit");
            }
            if (workspace_elements > effective_workspace_limit - candidate_workspace) break;
            workspace_elements += candidate_workspace;
            ++tile_end;
        }
        if (tile_end == tile_begin) {
            throw std::length_error("GPU could not fit one candidate in a tile");
        }
        result.tiles.push_back({tile_begin, tile_end, workspace_elements, signature});
        result.maximum_workspace_elements = std::max(
            result.maximum_workspace_elements, workspace_elements);
        tile_begin = tile_end;
    }
    return result;
}

PackedTile pack_tile(ConstImageView source,
                     std::span<const CandidateAnalysis> candidates,
                     KernelDispatchPolicy policy) {
    PackedTile packed;
    if (candidates.empty()) return packed;
    const TileSignature signature = candidate_signature(source, candidates.front(), policy);
    for (const CandidateAnalysis &candidate : candidates.subspan(1U)) {
        if (candidate_signature(source, candidate, policy) != signature) {
            throw std::invalid_argument("GPU tile contains mixed dispatch signatures");
        }
    }
    packed.descriptors.reserve(
        candidates.size() * (signature.axes == AnalysisAxes::both ? 2U : 1U));
    if (signature.axes == AnalysisAxes::both) {
        append_two_axis_plans(packed, source, candidates, policy);
    } else {
        for (const CandidateAnalysis &candidate : candidates) {
            append_single_plan(packed, source, candidate);
        }
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
    return packed;
}

std::size_t packed_plan_bytes(const PackedTile &packed) {
    const std::size_t fields[] = {
        vector_bytes(packed.descriptors, "plan descriptors"),
        vector_bytes(packed.transpose_offsets, "transpose offsets"),
        vector_bytes(packed.transpose_indices, "transpose indices"),
        vector_bytes(packed.transpose_weights, "transpose weights"),
        vector_bytes(packed.lower_ld, "lower factors"),
        vector_bytes(packed.upper_l, "upper factors"),
        vector_bytes(packed.inverse_diagonal, "inverse diagonal"),
        vector_bytes(packed.forward_left, "forward left indices"),
        vector_bytes(packed.forward_weights, "forward weights"),
    };
    std::size_t total = 0U;
    for (const std::size_t bytes : fields) {
        if (bytes > std::numeric_limits<std::size_t>::max() - total) {
            throw std::length_error("packed GPU plan byte size overflow");
        }
        total += bytes;
    }
    return total;
}

std::vector<CandidateResult> merge_metric_partials(
    std::span<const CandidateAnalysis> candidates,
    std::span<const float> partials,
    std::size_t groups_per_candidate,
    double pixel_count) {
    if (groups_per_candidate == 0U || !std::isfinite(pixel_count) || pixel_count <= 0.0) {
        throw std::invalid_argument("invalid GPU metric merge configuration");
    }
    const std::size_t expected = checked_product(
        candidates.size(), groups_per_candidate, "metric partial table");
    if (partials.size() != expected) {
        throw std::invalid_argument("GPU metric partial table has the wrong size");
    }
    std::vector<CandidateResult> results;
    results.reserve(candidates.size());
    for (std::size_t candidate_index = 0; candidate_index < candidates.size();
         ++candidate_index) {
        double sum = 0.0;
        for (std::size_t group = 0; group < groups_per_candidate; ++group) {
            const float partial = partials[candidate_index * groups_per_candidate + group];
            if (!std::isfinite(partial)) {
                throw std::runtime_error("GPU metric partial is nonfinite");
            }
            sum += static_cast<double>(partial);
        }
        const double error = sum / pixel_count;
        if (!std::isfinite(error)) throw std::runtime_error("GPU metric result is nonfinite");
        results.push_back({candidates[candidate_index].id, error});
    }
    return results;
}

} // namespace getnative::detail::gpu

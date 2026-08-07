#include "getnative/axis_plan.hpp"

#include "axis_plan_diagnostics.hpp"
#include "axis_plan_key.hpp"
#include "axis_planner.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

#ifndef GETNATIVE_PLANNER_REUSE_TAPS
#define GETNATIVE_PLANNER_REUSE_TAPS 1
#endif

#ifndef GETNATIVE_PLANNER_FAST_INTERIOR
#define GETNATIVE_PLANNER_FAST_INTERIOR 1
#endif

#ifndef GETNATIVE_PLANNER_DIRECT_TRANSPOSE
#define GETNATIVE_PLANNER_DIRECT_TRANSPOSE 1
#endif

// The sampling geometry and banded LDLT strategy are independently expressed from
// Frechdachs/descale (MIT, Copyright 2021 Frechdachs). See upstream/descale/LICENSE.

namespace getnative {

namespace detail {

struct AxisPlanGeometry {
    std::int32_t source_size = 0;
    std::int32_t destination_size = 0;
    std::int32_t support = 0;
    std::int32_t forward_filter_size = 0;
    std::uint64_t active_length_bits = 0;
    std::uint64_t shift_bits = 0;
    BorderMode border = BorderMode::mirror;
    double forward_step = 1.0;
    std::vector<double> descale_distances;
    std::vector<std::uint32_t> descale_row_offsets;
    std::vector<std::int32_t> descale_unique_indices;
    std::vector<std::int32_t> descale_tap_slots;
    std::vector<double> forward_distances;
    std::vector<std::int32_t> forward_tap_indices;
    std::vector<std::int32_t> forward_left;
    std::vector<std::int32_t> forward_right;
};

} // namespace detail

namespace {

constexpr bool reuse_tap_weights = GETNATIVE_PLANNER_REUSE_TAPS != 0;
constexpr bool fast_interior_indices = GETNATIVE_PLANNER_FAST_INTERIOR != 0;
constexpr bool direct_transpose = GETNATIVE_PLANNER_DIRECT_TRANSPOSE != 0;

[[nodiscard]] constexpr bool finite_binary64(double value) noexcept {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559);
    constexpr std::uint64_t exponent_mask = 0x7ff0000000000000ULL;
    return (std::bit_cast<std::uint64_t>(value) & exponent_mask) != exponent_mask;
}

[[nodiscard]] double round_half_up(double x) noexcept {
    // Preserves round(x - 1) == round(x) - 1 on the pixel grid, as zimg does.
    return x < 0.0 ? std::floor(x + 0.5)
                   : std::floor(x + 0.49999999999999994);
}

[[nodiscard]] constexpr double poly3(double x, double c0, double c1,
                                     double c2, double c3) noexcept {
    return c0 + x * (c1 + x * (c2 + x * c3));
}

struct ZimgKernel {
    explicit ZimgKernel(const Filter &filter) noexcept
        : filter(filter),
          p0((6.0 - 2.0 * filter.b) / 6.0),
          p2((-18.0 + 12.0 * filter.b + 6.0 * filter.c) / 6.0),
          p3((12.0 - 9.0 * filter.b - 6.0 * filter.c) / 6.0),
          q0((8.0 * filter.b + 24.0 * filter.c) / 6.0),
          q1((-12.0 * filter.b - 48.0 * filter.c) / 6.0),
          q2((6.0 * filter.b + 30.0 * filter.c) / 6.0),
          q3((-filter.b - 6.0 * filter.c) / 6.0) {}

    [[nodiscard]] double weight(double distance) const noexcept {
        double x = std::abs(distance);
        switch (filter.type) {
        case KernelType::bilinear:
            return std::max(1.0 - x, 0.0);
        case KernelType::bicubic:
            if (x < 1.0) return poly3(x, p0, 0.0, p2, p3);
            if (x < 2.0) return poly3(x, q0, q1, q2, q3);
            return 0.0;
        case KernelType::lanczos:
            if (x >= static_cast<double>(filter.taps)) return 0.0;
            return sinc(x) * sinc(x / static_cast<double>(filter.taps));
        case KernelType::spline16:
            if (x < 1.0) return poly3(x, 1.0, -1.0 / 5.0, -9.0 / 5.0, 1.0);
            if (x < 2.0) {
                x -= 1.0;
                return poly3(x, 0.0, -7.0 / 15.0, 4.0 / 5.0, -1.0 / 3.0);
            }
            return 0.0;
        case KernelType::spline36:
            if (x < 1.0) return poly3(x, 1.0, -3.0 / 209.0, -453.0 / 209.0, 13.0 / 11.0);
            if (x < 2.0) {
                x -= 1.0;
                return poly3(x, 0.0, -156.0 / 209.0, 270.0 / 209.0, -6.0 / 11.0);
            }
            if (x < 3.0) {
                x -= 2.0;
                return poly3(x, 0.0, 26.0 / 209.0, -45.0 / 209.0, 1.0 / 11.0);
            }
            return 0.0;
        case KernelType::spline64:
            if (x < 1.0) return poly3(x, 1.0, -3.0 / 2911.0, -6387.0 / 2911.0, 49.0 / 41.0);
            if (x < 2.0) {
                x -= 1.0;
                return poly3(x, 0.0, -2328.0 / 2911.0, 4032.0 / 2911.0, -24.0 / 41.0);
            }
            if (x < 3.0) {
                x -= 2.0;
                return poly3(x, 0.0, 582.0 / 2911.0, -1008.0 / 2911.0, 6.0 / 41.0);
            }
            if (x < 4.0) {
                x -= 3.0;
                return poly3(x, 0.0, -97.0 / 2911.0, 168.0 / 2911.0, -1.0 / 41.0);
            }
            return 0.0;
        }
        return 0.0;
    }

private:
    [[nodiscard]] static double sinc(double x) noexcept {
        constexpr double pi = 3.14159265358979323846;
        return x == 0.0 ? 1.0 : std::sin(x * pi) / (x * pi);
    }

    Filter filter;
    double p0;
    double p2;
    double p3;
    double q0;
    double q1;
    double q2;
    double q3;
};

[[nodiscard]] std::int32_t border_index(double pixel_center, std::int32_t size,
                                        BorderMode border) {
    double mapped = pixel_center;
    if (pixel_center < 0.0 || pixel_center >= static_cast<double>(size)) {
        if (border == BorderMode::zero) {
            return -1;
        }
        if (border == BorderMode::repeat) {
            mapped = pixel_center < 0.0 ? 0.0 : static_cast<double>(size) - 0.5;
        } else {
            mapped = pixel_center < 0.0
                ? -pixel_center
                : std::min(2.0 * static_cast<double>(size) - pixel_center,
                           static_cast<double>(size) - 0.5);
        }
    }
    constexpr double minimum_index =
        static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr double maximum_index_exclusive =
        static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 1.0;
    if (!finite_binary64(mapped) || mapped < minimum_index
        || mapped >= maximum_index_exclusive) {
        throw std::out_of_range("shift places filter support outside the 32-bit pixel grid");
    }
    const auto index = static_cast<std::int32_t>(std::floor(mapped));
    return index >= 0 && index < size ? index : -1;
}

[[nodiscard]] std::int32_t validated_support(const AxisPlanRequest &request) {
    if (request.source_size <= 0 || request.destination_size <= 0) {
        throw std::invalid_argument("axis dimensions must be positive");
    }
    if (!finite_binary64(request.active_length) || !(request.active_length > 0.0)
        || !finite_binary64(request.shift)) {
        throw std::invalid_argument(
            "active length and shift must be finite, with positive active length");
    }
    const std::int32_t support = request.filter.support();
    if (support > (std::numeric_limits<std::int32_t>::max() - 1) / 2) {
        throw std::invalid_argument("filter support is too large");
    }
    return support;
}

[[nodiscard]] std::size_t checked_geometry_elements(
    std::int32_t rows,
    std::int32_t width,
    std::string_view name) {
    const std::size_t row_count = static_cast<std::size_t>(rows);
    const std::size_t row_width = static_cast<std::size_t>(width);
    if (row_width != 0U
        && row_count > std::numeric_limits<std::size_t>::max() / row_width) {
        throw std::length_error(std::string{name} + " is too large");
    }
    return row_count * row_width;
}

[[nodiscard]] bool geometry_matches(
    const detail::AxisPlanGeometry &geometry,
    const AxisPlanRequest &request,
    std::int32_t support) noexcept {
    return geometry.source_size == request.source_size
        && geometry.destination_size == request.destination_size
        && geometry.support == support
        && geometry.active_length_bits
            == std::bit_cast<std::uint64_t>(request.active_length)
        && geometry.shift_bits == std::bit_cast<std::uint64_t>(request.shift)
        && geometry.border == request.border;
}

[[nodiscard]] std::shared_ptr<const detail::AxisPlanGeometry>
make_axis_plan_geometry(const AxisPlanRequest &request) {
    const std::int32_t support = validated_support(request);
    const std::int32_t tap_count = 2 * support;
    auto geometry = std::make_shared<detail::AxisPlanGeometry>();
    geometry->source_size = request.source_size;
    geometry->destination_size = request.destination_size;
    geometry->support = support;
    geometry->active_length_bits = std::bit_cast<std::uint64_t>(request.active_length);
    geometry->shift_bits = std::bit_cast<std::uint64_t>(request.shift);
    geometry->border = request.border;

    const std::size_t descale_elements = checked_geometry_elements(
        request.source_size, tap_count, "descale geometry");
    geometry->descale_distances.resize(descale_elements);
    geometry->descale_tap_slots.resize(descale_elements, -1);
    geometry->descale_row_offsets.reserve(
        static_cast<std::size_t>(request.source_size) + 1U);
    geometry->descale_unique_indices.reserve(descale_elements);
    geometry->descale_row_offsets.push_back(0U);

    const double ratio = static_cast<double>(request.source_size)
        / request.active_length;
    std::array<std::int32_t, 30> tap_indices{};
    std::array<std::int32_t, 30> unique_indices{};
    for (std::int32_t row = 0; row < request.source_size; ++row) {
        const double position = (static_cast<double>(row) + 0.5) / ratio
            + request.shift;
        const double begin = round_half_up(
            position - static_cast<double>(support)) + 0.5;
        std::int32_t unique_count = 0;
        const std::size_t row_base = static_cast<std::size_t>(row)
            * static_cast<std::size_t>(tap_count);
        if constexpr (fast_interior_indices) {
            const double final_center = begin + static_cast<double>(tap_count - 1);
            if (begin >= 0.0
                && final_center < static_cast<double>(request.destination_size)) {
                const auto first_index = static_cast<std::int32_t>(std::floor(begin));
                for (std::int32_t tap = 0; tap < tap_count; ++tap) {
                    const std::size_t offset = row_base + static_cast<std::size_t>(tap);
                    geometry->descale_distances[offset] =
                        begin + static_cast<double>(tap) - position;
                    geometry->descale_unique_indices.push_back(first_index + tap);
                    geometry->descale_tap_slots[offset] = tap;
                }
                geometry->descale_row_offsets.push_back(static_cast<std::uint32_t>(
                    geometry->descale_unique_indices.size()));
                continue;
            }
        }
        for (std::int32_t tap = 0; tap < tap_count; ++tap) {
            const double center = begin + static_cast<double>(tap);
            const std::size_t offset = row_base + static_cast<std::size_t>(tap);
            geometry->descale_distances[offset] = center - position;
            const std::int32_t index = border_index(
                center, request.destination_size, request.border);
            tap_indices[static_cast<std::size_t>(tap)] = index;
            if (index < 0) continue;
            const auto found = std::find(
                unique_indices.begin(), unique_indices.begin() + unique_count, index);
            if (found == unique_indices.begin() + unique_count) {
                unique_indices[static_cast<std::size_t>(unique_count++)] = index;
            }
        }
        std::sort(unique_indices.begin(), unique_indices.begin() + unique_count);
        geometry->descale_unique_indices.insert(
            geometry->descale_unique_indices.end(), unique_indices.begin(),
            unique_indices.begin() + unique_count);
        for (std::int32_t tap = 0; tap < tap_count; ++tap) {
            const std::int32_t index = tap_indices[static_cast<std::size_t>(tap)];
            if (index < 0) continue;
            const auto slot = std::lower_bound(
                unique_indices.begin(), unique_indices.begin() + unique_count, index);
            geometry->descale_tap_slots[
                row_base + static_cast<std::size_t>(tap)] = static_cast<std::int32_t>(
                    std::distance(unique_indices.begin(), slot));
        }
        geometry->descale_row_offsets.push_back(static_cast<std::uint32_t>(
            geometry->descale_unique_indices.size()));
    }

    const double scale = static_cast<double>(request.source_size)
        / request.active_length;
    geometry->forward_step = std::min(scale, 1.0);
    const double expanded_support = static_cast<double>(support)
        / geometry->forward_step;
    if (!finite_binary64(expanded_support)
        || expanded_support
            > static_cast<double>(std::numeric_limits<std::int32_t>::max() / 2)) {
        throw std::length_error("zimg forward filter width is too large");
    }
    geometry->forward_filter_size = std::max(
        static_cast<std::int32_t>(std::ceil(expanded_support)) * 2, 1);
    const std::size_t forward_elements = checked_geometry_elements(
        request.source_size, geometry->forward_filter_size, "forward geometry");
    geometry->forward_distances.resize(forward_elements);
    geometry->forward_tap_indices.resize(forward_elements);
    geometry->forward_left.resize(static_cast<std::size_t>(request.source_size));
    geometry->forward_right.resize(static_cast<std::size_t>(request.source_size));
    const double maximum_mapped = std::nextafter(
        static_cast<double>(request.destination_size),
        -std::numeric_limits<double>::max());
    for (std::int32_t row = 0; row < request.source_size; ++row) {
        const double position = (static_cast<double>(row) + 0.5) / scale
            + request.shift;
        const double begin = round_half_up(
            position - static_cast<double>(geometry->forward_filter_size) / 2.0)
            + 0.5;
        std::int32_t left = request.destination_size;
        std::int32_t right = 0;
        const std::size_t row_base = static_cast<std::size_t>(row)
            * static_cast<std::size_t>(geometry->forward_filter_size);
        if constexpr (fast_interior_indices) {
            const double final_center = begin
                + static_cast<double>(geometry->forward_filter_size - 1);
            if (begin >= 0.0
                && final_center < static_cast<double>(request.destination_size)) {
                const auto first_index = static_cast<std::int32_t>(std::floor(begin));
                for (std::int32_t tap = 0; tap < geometry->forward_filter_size; ++tap) {
                    const std::size_t offset = row_base + static_cast<std::size_t>(tap);
                    geometry->forward_distances[offset] =
                        (begin + static_cast<double>(tap) - position)
                        * geometry->forward_step;
                    geometry->forward_tap_indices[offset] = first_index + tap;
                }
                geometry->forward_left[static_cast<std::size_t>(row)] = first_index;
                geometry->forward_right[static_cast<std::size_t>(row)] =
                    first_index + geometry->forward_filter_size;
                continue;
            }
        }
        for (std::int32_t tap = 0; tap < geometry->forward_filter_size; ++tap) {
            const double pixel_center = begin + static_cast<double>(tap);
            const std::size_t offset = row_base + static_cast<std::size_t>(tap);
            geometry->forward_distances[offset] =
                (pixel_center - position) * geometry->forward_step;
            double mapped = pixel_center;
            if (pixel_center < 0.0) {
                mapped = -pixel_center;
            } else if (pixel_center >= static_cast<double>(request.destination_size)) {
                mapped = 2.0 * static_cast<double>(request.destination_size)
                    - pixel_center;
            }
            mapped = std::clamp(mapped, 0.0, maximum_mapped);
            const auto index = static_cast<std::int32_t>(std::floor(mapped));
            geometry->forward_tap_indices[offset] = index;
            left = std::min(left, index);
            right = std::max(right, index + 1);
        }
        geometry->forward_left[static_cast<std::size_t>(row)] = left;
        geometry->forward_right[static_cast<std::size_t>(row)] = right;
    }
    return geometry;
}

struct DoubleCsr {
    std::vector<std::uint32_t> offsets;
    std::vector<std::int32_t> indices;
    std::vector<double> weights;
};

struct DoubleCsrView {
    std::vector<std::uint32_t> &offsets;
    std::vector<std::int32_t> &indices;
    std::vector<double> &weights;
};

template <bool ReuseTapWeights, bool ReuseGeometry = false>
void make_descale_matrix(DoubleCsrView result,
                         const AxisPlanRequest &request,
                         std::int32_t support,
                         const detail::AxisPlanGeometry *geometry = nullptr) {
    const std::int32_t rows = request.source_size;
    const std::int32_t columns = request.destination_size;
    const double ratio = static_cast<double>(rows) / request.active_length;
    if constexpr (ReuseGeometry) {
        if (geometry == nullptr || !geometry_matches(*geometry, request, support)) {
            throw std::invalid_argument("axis plan geometry does not match the request");
        }
    }

    result.offsets.clear();
    result.indices.clear();
    result.weights.clear();
    result.offsets.reserve(static_cast<std::size_t>(rows) + 1U);
    result.indices.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(2 * support));
    result.weights.reserve(result.indices.capacity());
    result.offsets.push_back(0);

    std::vector<std::pair<std::int32_t, double>> row;
    row.reserve(static_cast<std::size_t>(2 * support));
    // Filter::support() caps Lanczos at 15 taps, so the widest row has 30 taps.
    std::array<double, 30> tap_weights{};
    std::array<double, 30> coalesced_weights{};
    std::array<bool, 30> coalesced_seen{};
    for (std::int32_t i = 0; i < rows; ++i) {
        double position = 0.0;
        double begin = 0.0;
        if constexpr (!ReuseGeometry) {
            position = (static_cast<double>(i) + 0.5) / ratio + request.shift;
            begin = round_half_up(position - static_cast<double>(support)) + 0.5;
        }
        const std::size_t geometry_row_base = static_cast<std::size_t>(i)
            * static_cast<std::size_t>(2 * support);
        double total = 0.0;
        for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
            double distance = 0.0;
            if constexpr (ReuseGeometry) {
                distance = geometry->descale_distances[
                    geometry_row_base + static_cast<std::size_t>(tap)];
            } else {
                distance = begin + static_cast<double>(tap) - position;
            }
            const double weight = request.filter.weight(distance);
            if constexpr (ReuseTapWeights) {
                tap_weights[static_cast<std::size_t>(tap)] = weight;
            }
            total += weight;
        }
        if (!finite_binary64(total) || total == 0.0) {
            throw std::runtime_error("filter produced a zero or non-finite weight sum");
        }

        if constexpr (!ReuseGeometry && fast_interior_indices) {
            const double final_center = begin + static_cast<double>(2 * support - 1);
            if (begin >= 0.0 && final_center < static_cast<double>(columns)) {
                const auto first_index = static_cast<std::int32_t>(std::floor(begin));
                for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
                    const double raw_weight = ReuseTapWeights
                        ? tap_weights[static_cast<std::size_t>(tap)]
                        : request.filter.weight(
                            begin + static_cast<double>(tap) - position);
                    const double weight = raw_weight / total;
                    if (weight == 0.0) continue;
                    result.indices.push_back(first_index + tap);
                    result.weights.push_back(weight);
                }
                result.offsets.push_back(static_cast<std::uint32_t>(
                    result.indices.size()));
                continue;
            }
        }

        if constexpr (ReuseGeometry) {
            coalesced_weights.fill(0.0);
            coalesced_seen.fill(false);
        } else {
            row.clear();
        }
        for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
            std::int32_t index = -1;
            std::int32_t slot = -1;
            double center = 0.0;
            if constexpr (ReuseGeometry) {
                slot = geometry->descale_tap_slots[
                    geometry_row_base + static_cast<std::size_t>(tap)];
                if (slot < 0) continue;
            } else {
                center = begin + static_cast<double>(tap);
                index = border_index(center, columns, request.border);
                if (index < 0) continue;
            }
            double raw_weight = 0.0;
            if constexpr (ReuseTapWeights) {
                raw_weight = tap_weights[static_cast<std::size_t>(tap)];
            } else {
                if constexpr (ReuseGeometry) {
                    raw_weight = request.filter.weight(geometry->descale_distances[
                        geometry_row_base + static_cast<std::size_t>(tap)]);
                } else {
                    raw_weight = request.filter.weight(center - position);
                }
            }
            const double weight = raw_weight / total;
            if (weight == 0.0) continue;
            if constexpr (ReuseGeometry) {
                const std::size_t output_slot = static_cast<std::size_t>(slot);
                if (!coalesced_seen[output_slot]) {
                    coalesced_weights[output_slot] = weight;
                    coalesced_seen[output_slot] = true;
                } else {
                    coalesced_weights[output_slot] += weight;
                }
            } else {
                const auto duplicate = std::find_if(
                    row.begin(), row.end(), [index](const auto &entry) {
                        return entry.first == index;
                    });
                if (duplicate == row.end()) {
                    row.emplace_back(index, weight);
                } else {
                    duplicate->second += weight;
                }
            }
        }
        if constexpr (ReuseGeometry) {
            const std::uint32_t index_begin = geometry->descale_row_offsets[
                static_cast<std::size_t>(i)];
            const std::uint32_t index_end = geometry->descale_row_offsets[
                static_cast<std::size_t>(i) + 1U];
            for (std::uint32_t slot = 0; slot < index_end - index_begin; ++slot) {
                if (!coalesced_seen[slot]) continue;
                result.indices.push_back(
                    geometry->descale_unique_indices[index_begin + slot]);
                result.weights.push_back(coalesced_weights[slot]);
            }
        } else {
            std::sort(row.begin(), row.end(), [](const auto &a, const auto &b) {
                return a.first < b.first;
            });
            for (const auto &[index, weight] : row) {
                result.indices.push_back(index);
                result.weights.push_back(weight);
            }
        }
        result.offsets.push_back(static_cast<std::uint32_t>(result.indices.size()));
    }
}

template <bool ReuseTapWeights, bool ReuseGeometry = false>
void make_zimg_forward(DoubleCsrView result,
                       const AxisPlanRequest &request,
                       std::int32_t support,
                       const detail::AxisPlanGeometry *geometry = nullptr) {
    const std::int32_t rows = request.source_size;
    const std::int32_t columns = request.destination_size;
    const double scale = static_cast<double>(rows) / request.active_length;
    double step = std::min(scale, 1.0);
    std::int32_t filter_size = 0;
    if constexpr (ReuseGeometry) {
        if (geometry == nullptr || !geometry_matches(*geometry, request, support)) {
            throw std::invalid_argument("axis plan geometry does not match the request");
        }
        step = geometry->forward_step;
        filter_size = geometry->forward_filter_size;
    } else {
        const double expanded_support = static_cast<double>(support) / step;
        if (!finite_binary64(expanded_support)
            || expanded_support
                > static_cast<double>(std::numeric_limits<std::int32_t>::max() / 2)) {
            throw std::length_error("zimg forward filter width is too large");
        }
        filter_size = std::max(
            static_cast<std::int32_t>(std::ceil(expanded_support)) * 2, 1);
    }
    const ZimgKernel kernel(request.filter);

    result.offsets.clear();
    result.indices.clear();
    result.weights.clear();
    result.offsets.reserve(static_cast<std::size_t>(rows) + 1U);
    result.indices.reserve(static_cast<std::size_t>(rows)
                           * static_cast<std::size_t>(std::min(filter_size, columns)));
    result.weights.reserve(result.indices.capacity());
    result.offsets.push_back(0U);

    std::vector<std::int32_t> tap_indices;
    if constexpr (!ReuseGeometry) {
        tap_indices.resize(static_cast<std::size_t>(filter_size));
    }
    std::vector<double> tap_weights(static_cast<std::size_t>(filter_size));
    std::vector<double> row_weights;
    for (std::int32_t row = 0; row < rows; ++row) {
        double position = 0.0;
        double begin = 0.0;
        if constexpr (!ReuseGeometry) {
            position = (static_cast<double>(row) + 0.5) / scale + request.shift;
            begin = round_half_up(
                position - static_cast<double>(filter_size) / 2.0) + 0.5;
        }
        const std::size_t geometry_row_base = static_cast<std::size_t>(row)
            * static_cast<std::size_t>(filter_size);
        double total = 0.0;
        for (std::int32_t tap = 0; tap < filter_size; ++tap) {
            double distance = 0.0;
            if constexpr (ReuseGeometry) {
                distance = geometry->forward_distances[
                    geometry_row_base + static_cast<std::size_t>(tap)];
            } else {
                distance = (begin + static_cast<double>(tap) - position) * step;
            }
            const double weight = kernel.weight(distance);
            if constexpr (ReuseTapWeights) {
                tap_weights[static_cast<std::size_t>(tap)] = weight;
            }
            total += weight;
        }
        if (!finite_binary64(total) || total == 0.0) {
            throw std::runtime_error("zimg forward filter produced a zero or non-finite weight sum");
        }

        if constexpr (!ReuseGeometry && fast_interior_indices) {
            const double final_center = begin + static_cast<double>(filter_size - 1);
            if (begin >= 0.0 && final_center < static_cast<double>(columns)) {
                const auto first_index = static_cast<std::int32_t>(std::floor(begin));
                for (std::int32_t tap = 0; tap < filter_size; ++tap) {
                    const double raw_weight = ReuseTapWeights
                        ? tap_weights[static_cast<std::size_t>(tap)]
                        : kernel.weight(
                            (begin + static_cast<double>(tap) - position) * step);
                    double weight = 0.0;
                    weight += raw_weight / total;
                    result.indices.push_back(first_index + tap);
                    result.weights.push_back(weight);
                }
                result.offsets.push_back(static_cast<std::uint32_t>(
                    result.indices.size()));
                continue;
            }
        }

        std::int32_t left = ReuseGeometry
            ? geometry->forward_left[static_cast<std::size_t>(row)] : columns;
        std::int32_t right = ReuseGeometry
            ? geometry->forward_right[static_cast<std::size_t>(row)] : 0;
        for (std::int32_t tap = 0; tap < filter_size; ++tap) {
            double pixel_center = 0.0;
            std::int32_t index = 0;
            if constexpr (ReuseGeometry) {
                index = geometry->forward_tap_indices[
                    geometry_row_base + static_cast<std::size_t>(tap)];
            } else {
                pixel_center = begin + static_cast<double>(tap);
                double mapped = pixel_center;
                if (pixel_center < 0.0) {
                    mapped = -pixel_center;
                } else if (pixel_center >= static_cast<double>(columns)) {
                    mapped = 2.0 * static_cast<double>(columns) - pixel_center;
                }
                mapped = std::clamp(
                    mapped, 0.0,
                    std::nextafter(static_cast<double>(columns),
                                   -std::numeric_limits<double>::max()));
                index = static_cast<std::int32_t>(std::floor(mapped));
                tap_indices[static_cast<std::size_t>(tap)] = index;
            }
            double raw_weight = 0.0;
            if constexpr (ReuseTapWeights) {
                raw_weight = tap_weights[static_cast<std::size_t>(tap)];
            } else {
                if constexpr (ReuseGeometry) {
                    raw_weight = kernel.weight(geometry->forward_distances[
                        geometry_row_base + static_cast<std::size_t>(tap)]);
                } else {
                    raw_weight = kernel.weight((pixel_center - position) * step);
                }
            }
            tap_weights[static_cast<std::size_t>(tap)] = raw_weight / total;
            if constexpr (!ReuseGeometry) {
                left = std::min(left, index);
                right = std::max(right, index + 1);
            }
        }

        row_weights.assign(static_cast<std::size_t>(right - left), 0.0);
        for (std::int32_t tap = 0; tap < filter_size; ++tap) {
            const std::int32_t index = ReuseGeometry
                ? geometry->forward_tap_indices[
                    geometry_row_base + static_cast<std::size_t>(tap)]
                : tap_indices[static_cast<std::size_t>(tap)];
            row_weights[static_cast<std::size_t>(index - left)] +=
                tap_weights[static_cast<std::size_t>(tap)];
        }
        for (std::int32_t index = left; index < right; ++index) {
            result.indices.push_back(index);
            result.weights.push_back(row_weights[static_cast<std::size_t>(index - left)]);
        }
        result.offsets.push_back(static_cast<std::uint32_t>(result.indices.size()));
    }
}

[[nodiscard]] DoubleCsr transpose_csr(const DoubleCsrView &input, std::int32_t rows,
                                       std::int32_t columns) {
    DoubleCsr result;
    result.offsets.assign(static_cast<std::size_t>(columns) + 1U, 0U);
    for (const std::int32_t index : input.indices) {
        ++result.offsets[static_cast<std::size_t>(index) + 1U];
    }
    for (std::int32_t i = 0; i < columns; ++i) {
        result.offsets[static_cast<std::size_t>(i) + 1U] += result.offsets[static_cast<std::size_t>(i)];
    }
    result.indices.resize(input.indices.size());
    result.weights.resize(input.weights.size());
    std::vector<std::uint32_t> cursor = result.offsets;
    for (std::int32_t row = 0; row < rows; ++row) {
        const auto begin = input.offsets[static_cast<std::size_t>(row)];
        const auto end = input.offsets[static_cast<std::size_t>(row) + 1U];
        for (std::uint32_t p = begin; p < end; ++p) {
            const auto column = static_cast<std::size_t>(input.indices[p]);
            const std::uint32_t target = cursor[column]++;
            result.indices[target] = row;
            result.weights[target] = input.weights[p];
        }
    }
    return result;
}

void transpose_csr_direct(const DoubleCsrView &input,
                          std::int32_t rows,
                          std::int32_t columns,
                          AxisPlan &plan) {
    plan.transpose_offsets.assign(static_cast<std::size_t>(columns) + 1U, 0U);
    for (const std::int32_t index : input.indices) {
        ++plan.transpose_offsets[static_cast<std::size_t>(index) + 1U];
    }
    for (std::int32_t column = 0; column < columns; ++column) {
        plan.transpose_offsets[static_cast<std::size_t>(column) + 1U] +=
            plan.transpose_offsets[static_cast<std::size_t>(column)];
    }
    plan.transpose_indices.resize(input.indices.size());
    plan.transpose_weights.resize(input.weights.size());
    for (std::int32_t row = 0; row < rows; ++row) {
        const std::uint32_t begin = input.offsets[static_cast<std::size_t>(row)];
        const std::uint32_t end = input.offsets[static_cast<std::size_t>(row) + 1U];
        for (std::uint32_t position = begin; position < end; ++position) {
            const auto column = static_cast<std::size_t>(input.indices[position]);
            const std::uint32_t target = plan.transpose_offsets[column]++;
            plan.transpose_indices[target] = row;
            plan.transpose_weights[target] = static_cast<float>(input.weights[position]);
        }
    }
    for (std::size_t column = static_cast<std::size_t>(columns);
         column > 0U; --column) {
        plan.transpose_offsets[column] = plan.transpose_offsets[column - 1U];
    }
    plan.transpose_offsets[0] = 0U;
}

void form_normal_bands(const DoubleCsrView &forward,
                       std::int32_t rows,
                       std::int32_t columns,
                       std::int32_t half_bandwidth,
                       std::vector<double> &bands) {
    const auto n = static_cast<std::size_t>(columns);
    bands.assign((static_cast<std::size_t>(half_bandwidth) + 1U) * n, 0.0);
    for (std::int32_t row = 0; row < rows; ++row) {
        const std::uint32_t begin = forward.offsets[static_cast<std::size_t>(row)];
        const std::uint32_t end = forward.offsets[static_cast<std::size_t>(row) + 1U];
        for (std::uint32_t p = begin; p < end; ++p) {
            const std::int32_t a = forward.indices[p];
            for (std::uint32_t q = p; q < end; ++q) {
                const std::int32_t b = forward.indices[q];
                const std::int32_t low = std::min(a, b);
                const std::int32_t distance = std::abs(a - b);
                if (distance <= half_bandwidth) {
                    bands[static_cast<std::size_t>(distance) * n + static_cast<std::size_t>(low)] +=
                        forward.weights[p] * forward.weights[q];
                }
            }
        }
    }
}

void factor_banded_ldlt(std::vector<double> &bands, std::int32_t n,
                        std::int32_t half_bandwidth) noexcept {
    const auto width = static_cast<std::size_t>(n);
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    for (std::int32_t i = 0; i < n; ++i) {
        const std::int32_t end = std::min(half_bandwidth + 1, n - i);
        const double pivot = bands[static_cast<std::size_t>(i)] + epsilon;
        for (std::int32_t j = 1; j < end; ++j) {
            const std::size_t upper = static_cast<std::size_t>(j) * width + static_cast<std::size_t>(i);
            const double multiplier = bands[upper] / pivot;
            for (std::int32_t k = 0; k < end - j; ++k) {
                bands[static_cast<std::size_t>(k) * width + static_cast<std::size_t>(i + j)] -=
                    multiplier * bands[static_cast<std::size_t>(j + k) * width + static_cast<std::size_t>(i)];
            }
        }
        const double inverse_pivot = 1.0 / pivot;
        for (std::int32_t j = 1; j < end; ++j) {
            bands[static_cast<std::size_t>(j) * width + static_cast<std::size_t>(i)] *= inverse_pivot;
        }
    }
}

} // namespace

bool AxisPlan::valid() const noexcept {
    const auto src = static_cast<std::size_t>(std::max(source_size, 0));
    const auto dst = static_cast<std::size_t>(std::max(destination_size, 0));
    const auto factors = static_cast<std::size_t>(std::max(half_bandwidth, 0)) * dst;
    const auto forward_elements = src * static_cast<std::size_t>(std::max(forward_width, 0));
    return source_size > 0 && destination_size > 0 && support > 0 && forward_width > 0
        && active_length > 0.0 && finite_binary64(active_length) && finite_binary64(shift)
        && forward_offsets.size() == src + 1U
        && forward_indices.size() == forward_elements
        && forward_weights.size() == forward_elements
        && transpose_offsets.size() == dst + 1U
        && transpose_indices.size() == transpose_weights.size()
        && lower_ld.size() == factors && upper_l.size() == factors
        && inverse_diagonal.size() == dst;
}

std::size_t AxisPlan::packed_factor_elements() const noexcept {
    return lower_ld.size() + upper_l.size() + inverse_diagonal.size();
}

std::size_t axis_plan_storage_bytes(const AxisPlan &plan) noexcept {
    return sizeof(plan)
        + plan.forward_offsets.size() * sizeof(std::uint32_t)
        + plan.forward_indices.size() * sizeof(std::int32_t)
        + plan.forward_weights.size() * sizeof(float)
        + plan.transpose_offsets.size() * sizeof(std::uint32_t)
        + plan.transpose_indices.size() * sizeof(std::int32_t)
        + plan.transpose_weights.size() * sizeof(float)
        + plan.lower_ld.size() * sizeof(float)
        + plan.upper_l.size() * sizeof(float)
        + plan.inverse_diagonal.size() * sizeof(float);
}

namespace {

[[nodiscard]] AxisPlan build_axis_plan_impl(
    const AxisPlanRequest &request,
    detail::TapEvaluationMode tap_evaluation,
    const detail::AxisPlanGeometry *geometry,
    detail::AxisPlanBuildScratch *reusable_scratch) {
    const std::int32_t support = validated_support(request);
    const std::int32_t half_bandwidth = std::min(2 * support - 1, request.destination_size - 1);
    detail::AxisPlanBuildScratch local_scratch;
    detail::AxisPlanBuildScratch &scratch = reusable_scratch != nullptr
        ? *reusable_scratch : local_scratch;
    DoubleCsrView descale_matrix{
        scratch.descale_offsets,
        scratch.descale_indices,
        scratch.descale_weights,
    };
    DoubleCsrView zimg_forward{
        scratch.forward_offsets,
        scratch.forward_indices,
        scratch.forward_weights,
    };
    if (geometry != nullptr) {
        if (tap_evaluation == detail::TapEvaluationMode::reuse) {
            make_descale_matrix<true, true>(
                descale_matrix, request, support, geometry);
            make_zimg_forward<true, true>(
                zimg_forward, request, support, geometry);
        } else {
            make_descale_matrix<false, true>(
                descale_matrix, request, support, geometry);
            make_zimg_forward<false, true>(
                zimg_forward, request, support, geometry);
        }
    } else {
        if (tap_evaluation == detail::TapEvaluationMode::reuse) {
            make_descale_matrix<true>(descale_matrix, request, support);
            make_zimg_forward<true>(zimg_forward, request, support);
        } else {
            make_descale_matrix<false>(descale_matrix, request, support);
            make_zimg_forward<false>(zimg_forward, request, support);
        }
    }

    AxisPlan plan;
    if constexpr (direct_transpose) {
        transpose_csr_direct(
            descale_matrix, request.source_size, request.destination_size, plan);
    } else {
        DoubleCsr transpose = transpose_csr(
            descale_matrix, request.source_size, request.destination_size);
        plan.transpose_offsets = std::move(transpose.offsets);
        plan.transpose_indices = std::move(transpose.indices);
        plan.transpose_weights.assign(
            transpose.weights.begin(), transpose.weights.end());
    }
    form_normal_bands(
        descale_matrix, request.source_size, request.destination_size,
        half_bandwidth, scratch.normal_bands);
    factor_banded_ldlt(
        scratch.normal_bands, request.destination_size, half_bandwidth);

    plan.source_size = request.source_size;
    plan.destination_size = request.destination_size;
    plan.support = support;
    plan.half_bandwidth = half_bandwidth;
    plan.active_length = request.active_length;
    plan.shift = request.shift;

    for (std::int32_t row = 0; row < request.source_size; ++row) {
        const auto begin = zimg_forward.offsets[static_cast<std::size_t>(row)];
        const auto end = zimg_forward.offsets[static_cast<std::size_t>(row) + 1U];
        plan.forward_width = std::max(
            plan.forward_width, static_cast<std::int32_t>(end - begin));
    }
    const auto forward_elements = static_cast<std::size_t>(request.source_size)
        * static_cast<std::size_t>(plan.forward_width);
    if (forward_elements > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("forward coefficient table is too large");
    }
    plan.forward_offsets.resize(static_cast<std::size_t>(request.source_size) + 1U);
    plan.forward_indices.resize(forward_elements);
    plan.forward_weights.resize(forward_elements);
    for (std::int32_t row = 0; row < request.source_size; ++row) {
        const std::size_t output_begin = static_cast<std::size_t>(row)
            * static_cast<std::size_t>(plan.forward_width);
        plan.forward_offsets[static_cast<std::size_t>(row)] =
            static_cast<std::uint32_t>(output_begin);
        const auto input_begin = zimg_forward.offsets[static_cast<std::size_t>(row)];
        const auto input_end = zimg_forward.offsets[static_cast<std::size_t>(row) + 1U];
        const std::int32_t row_left = zimg_forward.indices[input_begin];
        const std::int32_t row_right = zimg_forward.indices[input_end - 1U] + 1;
        const std::int32_t left = std::min(
            row_left, request.destination_size - plan.forward_width);
        double residual = 0.0;
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            const std::int32_t index = left + tap;
            double coefficient = 0.0;
            if (index >= row_left && index < row_right) {
                coefficient = zimg_forward.weights[
                    input_begin + static_cast<std::uint32_t>(index - row_left)];
            }
            const double expected = coefficient - residual;
            const float projected = static_cast<float>(expected);
            const std::size_t output = output_begin + static_cast<std::size_t>(tap);
            plan.forward_indices[output] = index;
            plan.forward_weights[output] = projected;
            residual = static_cast<double>(projected) - expected;
        }
    }
    plan.forward_offsets[static_cast<std::size_t>(request.source_size)] =
        static_cast<std::uint32_t>(forward_elements);
    const auto n = static_cast<std::size_t>(request.destination_size);
    const auto factor_count = static_cast<std::size_t>(half_bandwidth) * n;
    plan.lower_ld.assign(factor_count, 0.0F);
    plan.upper_l.assign(factor_count, 0.0F);
    plan.inverse_diagonal.resize(n);
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    for (std::int32_t i = 0; i < request.destination_size; ++i) {
        const double diagonal = scratch.normal_bands[static_cast<std::size_t>(i)];
        plan.inverse_diagonal[static_cast<std::size_t>(i)] = static_cast<float>(1.0 / (diagonal + epsilon));
        const std::int32_t available = std::min(half_bandwidth, request.destination_size - i - 1);
        for (std::int32_t distance = 1; distance <= available; ++distance) {
            const float l = static_cast<float>(scratch.normal_bands[
                static_cast<std::size_t>(distance) * n + static_cast<std::size_t>(i)]);
            plan.upper_l[static_cast<std::size_t>(distance - 1) * n + static_cast<std::size_t>(i)] = l;
            const std::int32_t row = i + distance;
            plan.lower_ld[static_cast<std::size_t>(distance - 1) * n + static_cast<std::size_t>(row)] =
                static_cast<float>(scratch.normal_bands[
                    static_cast<std::size_t>(distance) * n + static_cast<std::size_t>(i)]
                    * diagonal);
        }
    }
    return plan;
}

} // namespace

AxisPlan build_axis_plan(const AxisPlanRequest &request) {
    return build_axis_plan_impl(
        request,
        reuse_tap_weights ? detail::TapEvaluationMode::reuse
                          : detail::TapEvaluationMode::recompute,
        nullptr, nullptr);
}

namespace detail {

AxisPlan build_axis_plan_with_tap_evaluation(
    const AxisPlanRequest &request,
    TapEvaluationMode mode) {
    return build_axis_plan_impl(request, mode, nullptr, nullptr);
}

std::shared_ptr<const AxisPlanGeometry> build_axis_plan_geometry(
    const AxisPlanRequest &request) {
    return make_axis_plan_geometry(request);
}

std::size_t axis_plan_geometry_bytes(const AxisPlanGeometry &geometry) noexcept {
    return sizeof(geometry)
        + geometry.descale_distances.capacity() * sizeof(double)
        + geometry.descale_row_offsets.capacity() * sizeof(std::uint32_t)
        + geometry.descale_unique_indices.capacity() * sizeof(std::int32_t)
        + geometry.descale_tap_slots.capacity() * sizeof(std::int32_t)
        + geometry.forward_distances.capacity() * sizeof(double)
        + geometry.forward_tap_indices.capacity() * sizeof(std::int32_t)
        + geometry.forward_left.capacity() * sizeof(std::int32_t)
        + geometry.forward_right.capacity() * sizeof(std::int32_t);
}

AxisPlan build_axis_plan_with_geometry(
    const AxisPlanRequest &request,
    TapEvaluationMode tap_evaluation,
    const AxisPlanGeometry *geometry,
    AxisPlanBuildScratch *scratch) {
    return build_axis_plan_impl(request, tap_evaluation, geometry, scratch);
}

} // namespace detail

struct AxisPlanCache::Impl {
    struct Entry {
        std::shared_ptr<const AxisPlan> plan;
        std::size_t bytes = 0U;
        std::list<detail::PlanKey>::iterator lru_position;
    };

    explicit Impl(AxisPlanCacheLimits limits) : limits(limits) {}

    // All three require `mutex` held.
    void touch(const std::unordered_map<detail::PlanKey, Entry, detail::PlanKeyHash>::iterator &found) {
        lru.splice(lru.begin(), lru, found->second.lru_position);
    }

    void evict_lru() {
        const detail::PlanKey &oldest = lru.back();
        const auto found = plans.find(oldest);
        resident_bytes -= found->second.bytes;
        lru.erase(found->second.lru_position);
        plans.erase(found);
    }

    bool admit(detail::PlanKey key, std::shared_ptr<const AxisPlan> plan,
               std::size_t bytes) {
        if (bytes > limits.maximum_resident_bytes) {
            return false; // a single plan larger than the cap is never retained
        }
        while (!lru.empty()
               && (plans.size() >= limits.maximum_entries
                   || resident_bytes + bytes > limits.maximum_resident_bytes)) {
            evict_lru();
        }
        if (plans.size() >= limits.maximum_entries
            || resident_bytes + bytes > limits.maximum_resident_bytes) {
            return false;
        }
        lru.push_front(key);
        plans.emplace(key, Entry{std::move(plan), bytes, lru.begin()});
        resident_bytes += bytes;
        return true;
    }

    mutable std::mutex mutex;
    // Serializes miss paths across calls: a concurrent caller waits, then
    // re-checks before building — single-flight at call granularity.
    std::mutex build_mutex;
    AxisPlanCacheLimits limits;
    std::unordered_map<detail::PlanKey, Entry, detail::PlanKeyHash> plans;
    std::list<detail::PlanKey> lru; // front = most recently used
    std::size_t resident_bytes = 0U;
};

AxisPlanCache::AxisPlanCache(AxisPlanCacheLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}
AxisPlanCache::~AxisPlanCache() = default;

std::shared_ptr<const AxisPlan> AxisPlanCache::get_or_build(const AxisPlanRequest &request) {
    const detail::PlanKey key = detail::plan_key(request);
    {
        const std::scoped_lock lock(impl_->mutex);
        if (const auto found = impl_->plans.find(key); found != impl_->plans.end()) {
            impl_->touch(found);
            return found->second.plan;
        }
    }
    const std::scoped_lock build_lock(impl_->build_mutex);
    {
        const std::scoped_lock lock(impl_->mutex);
        if (const auto found = impl_->plans.find(key); found != impl_->plans.end()) {
            impl_->touch(found);
            return found->second.plan;
        }
    }
    auto candidate = std::make_shared<const AxisPlan>(build_axis_plan(request));
    const std::size_t candidate_bytes = axis_plan_storage_bytes(*candidate);
    const std::scoped_lock lock(impl_->mutex);
    impl_->admit(key, candidate, candidate_bytes);
    return candidate;
}

void AxisPlanCache::publish(const AxisPlanRequest &request,
                            std::shared_ptr<const AxisPlan> plan) {
    if (plan == nullptr || !plan->valid()) {
        throw std::invalid_argument("cannot publish an invalid plan");
    }
    const detail::PlanKey key = detail::plan_key(request);
    const std::size_t bytes = axis_plan_storage_bytes(*plan);
    const std::scoped_lock lock(impl_->mutex);
    if (const auto found = impl_->plans.find(key); found != impl_->plans.end()) {
        impl_->touch(found);
        return;
    }
    impl_->admit(key, std::move(plan), bytes);
}

std::vector<std::shared_ptr<const AxisPlan>> AxisPlanCache::lookup_batch(
    std::span<const AxisPlanRequest> requests) {
    std::vector<std::shared_ptr<const AxisPlan>> result(requests.size());
    const std::scoped_lock lock(impl_->mutex);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (const auto found = impl_->plans.find(detail::plan_key(requests[index]));
            found != impl_->plans.end()) {
            impl_->touch(found);
            result[index] = found->second.plan;
        }
    }
    return result;
}

AxisPlanCacheBatchResult AxisPlanCache::get_or_build_batch(
    std::span<const AxisPlanRequest> requests,
    std::size_t worker_count) {
    AxisPlanCacheBatchResult result;
    if (requests.empty()) {
        const std::scoped_lock lock(impl_->mutex);
        result.resident_entry_count = impl_->plans.size();
        result.resident_bytes = impl_->resident_bytes;
        return result;
    }

    std::vector<detail::PlanKey> unique_keys;
    std::vector<AxisPlanRequest> unique_requests;
    std::vector<std::size_t> request_to_unique;
    std::unordered_map<detail::PlanKey, std::size_t, detail::PlanKeyHash> unique_indices;
    unique_keys.reserve(requests.size());
    unique_requests.reserve(requests.size());
    request_to_unique.reserve(requests.size());
    unique_indices.reserve(requests.size());
    for (const AxisPlanRequest &request : requests) {
        const detail::PlanKey key = detail::plan_key(request);
        const auto [position, inserted] = unique_indices.emplace(key, unique_keys.size());
        if (inserted) {
            unique_keys.push_back(key);
            unique_requests.push_back(request);
        }
        request_to_unique.push_back(position->second);
    }

    result.unique_key_count = unique_keys.size();
    std::vector<std::shared_ptr<const AxisPlan>> unique_plans(unique_keys.size());
    std::vector<bool> ready(unique_keys.size(), false);
    {
        const std::scoped_lock lock(impl_->mutex);
        for (std::size_t index = 0; index < unique_keys.size(); ++index) {
            if (const auto found = impl_->plans.find(unique_keys[index]);
                found != impl_->plans.end()) {
                impl_->touch(found);
                unique_plans[index] = found->second.plan;
                ready[index] = true;
            }
        }
    }
    for (const std::size_t unique_index : request_to_unique) {
        if (ready[unique_index]) ++result.ready_hit_count;
    }

    // Single-flight: concurrent miss paths serialize on build_mutex, then
    // re-check — a caller that raced a publisher builds nothing twice.
    const std::scoped_lock build_lock(impl_->build_mutex);
    {
        const std::scoped_lock lock(impl_->mutex);
        for (std::size_t index = 0; index < unique_keys.size(); ++index) {
            if (ready[index]) continue;
            if (const auto found = impl_->plans.find(unique_keys[index]);
                found != impl_->plans.end()) {
                impl_->touch(found);
                unique_plans[index] = found->second.plan;
                ready[index] = true;
            }
        }
    }

    std::vector<AxisPlanRequest> missing_requests;
    std::vector<std::size_t> missing_to_unique;
    missing_requests.reserve(unique_keys.size());
    missing_to_unique.reserve(unique_keys.size());
    for (std::size_t index = 0; index < unique_keys.size(); ++index) {
        if (!ready[index]) {
            missing_requests.push_back(unique_requests[index]);
            missing_to_unique.push_back(index);
        }
    }

    detail::AxisPlanBatchResult built = detail::build_axis_plans(
        missing_requests, {worker_count, {}, {}});
    result.physical_build_count = built.physical_build_count;
    result.peak_active_builds = built.peak_active_builds;
    result.effective_worker_count = built.effective_worker_count;
    {
        const std::scoped_lock lock(impl_->mutex);
        for (std::size_t index = 0; index < missing_to_unique.size(); ++index) {
            const std::size_t unique_index = missing_to_unique[index];
            if (const auto found = impl_->plans.find(unique_keys[unique_index]);
                found != impl_->plans.end()) {
                impl_->touch(found);
                unique_plans[unique_index] = found->second.plan;
                continue;
            }
            const auto &candidate = built.plans[index];
            const std::size_t candidate_bytes = axis_plan_storage_bytes(*candidate);
            unique_plans[unique_index] = candidate;
            if (impl_->admit(unique_keys[unique_index], candidate, candidate_bytes)) {
                ++result.published_plan_count;
            }
        }
        result.resident_entry_count = impl_->plans.size();
        result.resident_bytes = impl_->resident_bytes;
    }

    result.plans.reserve(requests.size());
    for (const std::size_t unique_index : request_to_unique) {
        result.plans.push_back(unique_plans[unique_index]);
    }
    return result;
}

AxisPlanCacheLimits AxisPlanCache::limits() const noexcept {
    return impl_->limits;
}

std::size_t AxisPlanCache::size() const {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->plans.size();
}

std::size_t AxisPlanCache::resident_bytes() const {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->resident_bytes;
}

void AxisPlanCache::clear() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->plans.clear();
    impl_->lru.clear();
    impl_->resident_bytes = 0U;
}

namespace {

template <std::int32_t FixedHalfBandwidth>
void inverse_axis_impl(const AxisPlan &plan, const float *input,
                       std::ptrdiff_t input_stride, float *output,
                       std::ptrdiff_t output_stride) noexcept {
    const std::int32_t n = plan.destination_size;
    const std::int32_t c = FixedHalfBandwidth == 0 ? plan.half_bandwidth : FixedHalfBandwidth;
    const auto width = static_cast<std::size_t>(n);
    for (std::int32_t i = 0; i < n; ++i) {
        float sum = 0.0F;
        for (std::uint32_t p = plan.transpose_offsets[static_cast<std::size_t>(i)];
             p < plan.transpose_offsets[static_cast<std::size_t>(i) + 1U]; ++p) {
            sum = std::fma(
                plan.transpose_weights[p],
                input[static_cast<std::ptrdiff_t>(plan.transpose_indices[p])
                      * input_stride],
                sum);
        }
        const std::int32_t available = std::min(c, i);
        // Far-to-near ordering matches descale's scalar float solve.
        for (std::int32_t distance = available; distance >= 1; --distance) {
            sum = std::fma(
                -plan.lower_ld[static_cast<std::size_t>(distance - 1) * width
                               + static_cast<std::size_t>(i)],
                output[static_cast<std::ptrdiff_t>(i - distance) * output_stride],
                sum);
        }
        output[static_cast<std::ptrdiff_t>(i) * output_stride] =
            sum * plan.inverse_diagonal[static_cast<std::size_t>(i)];
    }
    for (std::int32_t i = n - 2; i >= 0; --i) {
        float sum = 0.0F;
        const std::int32_t available = std::min(c, n - i - 1);
        if constexpr (FixedHalfBandwidth == 3) {
            // Descale's bandwidth-7 path accumulates the backward solve near-to-far.
            for (std::int32_t distance = 1; distance <= available; ++distance) {
                sum = std::fma(
                    plan.upper_l[static_cast<std::size_t>(distance - 1) * width
                                 + static_cast<std::size_t>(i)],
                    output[static_cast<std::ptrdiff_t>(i + distance) * output_stride],
                    sum);
            }
        } else {
            for (std::int32_t distance = available; distance >= 1; --distance) {
                sum = std::fma(
                    plan.upper_l[static_cast<std::size_t>(distance - 1) * width
                                 + static_cast<std::size_t>(i)],
                    output[static_cast<std::ptrdiff_t>(i + distance) * output_stride],
                    sum);
            }
        }
        output[static_cast<std::ptrdiff_t>(i) * output_stride] -= sum;
    }
}

template <std::int32_t FixedWidth>
void forward_axis_impl(const AxisPlan &plan, const float *input,
                       std::ptrdiff_t input_stride, float *output,
                       std::ptrdiff_t output_stride) noexcept {
    const std::int32_t width = FixedWidth == 0 ? plan.forward_width : FixedWidth;
    const auto row_stride = static_cast<std::size_t>(plan.forward_width);
    for (std::int32_t row = 0; row < plan.source_size; ++row) {
        const std::size_t begin = static_cast<std::size_t>(row) * row_stride;
        const std::int32_t left = plan.forward_indices[begin];
        float sum = 0.0F;
        for (std::int32_t tap = 0; tap < width; ++tap) {
            sum = std::fma(
                plan.forward_weights[begin + static_cast<std::size_t>(tap)],
                input[static_cast<std::ptrdiff_t>(left + tap) * input_stride],
                sum);
        }
        output[static_cast<std::ptrdiff_t>(row) * output_stride] = sum;
    }
}

} // namespace

void inverse_axis_f32(const AxisPlan &plan, const float *input, std::ptrdiff_t input_stride,
                      float *output, std::ptrdiff_t output_stride) {
    if (!plan.valid() || input == nullptr || output == nullptr || input_stride == 0 || output_stride == 0) {
        throw std::invalid_argument("invalid inverse axis arguments");
    }
    if (plan.half_bandwidth == 1) {
        inverse_axis_impl<1>(plan, input, input_stride, output, output_stride);
    } else if (plan.half_bandwidth == 3) {
        inverse_axis_impl<3>(plan, input, input_stride, output, output_stride);
    } else {
        inverse_axis_impl<0>(plan, input, input_stride, output, output_stride);
    }
}

void forward_axis_f32(const AxisPlan &plan, const float *input, std::ptrdiff_t input_stride,
                      float *output, std::ptrdiff_t output_stride) {
    if (!plan.valid() || input == nullptr || output == nullptr || input_stride == 0 || output_stride == 0) {
        throw std::invalid_argument("invalid forward axis arguments");
    }
    switch (plan.forward_width) {
    case 2: forward_axis_impl<2>(plan, input, input_stride, output, output_stride); break;
    case 4: forward_axis_impl<4>(plan, input, input_stride, output, output_stride); break;
    case 6: forward_axis_impl<6>(plan, input, input_stride, output, output_stride); break;
    case 8: forward_axis_impl<8>(plan, input, input_stride, output, output_stride); break;
    default: forward_axis_impl<0>(plan, input, input_stride, output, output_stride); break;
    }
}

} // namespace getnative

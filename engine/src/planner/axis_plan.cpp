#include "getnative/axis_plan.hpp"

#include "axis_plan_diagnostics.hpp"
#include "axis_plan_key.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <vector>

// The sampling geometry and banded LDLT strategy are independently expressed from
// Frechdachs/descale (MIT, Copyright 2021 Frechdachs). See upstream/descale/LICENSE.

namespace getnative {
namespace {

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
    if (!std::isfinite(mapped) || mapped < minimum_index
        || mapped >= maximum_index_exclusive) {
        throw std::out_of_range("shift places filter support outside the 32-bit pixel grid");
    }
    const auto index = static_cast<std::int32_t>(std::floor(mapped));
    return index >= 0 && index < size ? index : -1;
}

struct DoubleCsr {
    std::vector<std::uint32_t> offsets;
    std::vector<std::int32_t> indices;
    std::vector<double> weights;
};

template <bool ReuseTapWeights>
[[nodiscard]] DoubleCsr make_descale_matrix(const AxisPlanRequest &request,
                                            std::int32_t support) {
    const std::int32_t rows = request.source_size;
    const std::int32_t columns = request.destination_size;
    const double ratio = static_cast<double>(rows) / request.active_length;

    DoubleCsr result;
    result.offsets.reserve(static_cast<std::size_t>(rows) + 1U);
    result.indices.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(2 * support));
    result.weights.reserve(result.indices.capacity());
    result.offsets.push_back(0);

    std::vector<std::pair<std::int32_t, double>> row;
    row.reserve(static_cast<std::size_t>(2 * support));
    // Filter::support() caps Lanczos at 15 taps, so the widest row has 30 taps.
    std::array<double, 30> tap_weights{};
    for (std::int32_t i = 0; i < rows; ++i) {
        const double position = (static_cast<double>(i) + 0.5) / ratio + request.shift;
        const double begin = round_half_up(position - static_cast<double>(support)) + 0.5;
        double total = 0.0;
        for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
            const double weight = request.filter.weight(
                begin + static_cast<double>(tap) - position);
            if constexpr (ReuseTapWeights) {
                tap_weights[static_cast<std::size_t>(tap)] = weight;
            }
            total += weight;
        }
        if (!std::isfinite(total) || total == 0.0) {
            throw std::runtime_error("filter produced a zero or non-finite weight sum");
        }

        row.clear();
        for (std::int32_t tap = 0; tap < 2 * support; ++tap) {
            const double center = begin + static_cast<double>(tap);
            const std::int32_t index = border_index(center, columns, request.border);
            if (index < 0) {
                continue;
            }
            double raw_weight = 0.0;
            if constexpr (ReuseTapWeights) {
                raw_weight = tap_weights[static_cast<std::size_t>(tap)];
            } else {
                raw_weight = request.filter.weight(center - position);
            }
            const double weight = raw_weight / total;
            if (weight == 0.0) {
                continue;
            }
            const auto duplicate = std::find_if(row.begin(), row.end(), [index](const auto &entry) {
                return entry.first == index;
            });
            if (duplicate == row.end()) {
                row.emplace_back(index, weight);
            } else {
                duplicate->second += weight;
            }
        }
        std::sort(row.begin(), row.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[index, weight] : row) {
            result.indices.push_back(index);
            result.weights.push_back(weight);
        }
        result.offsets.push_back(static_cast<std::uint32_t>(result.indices.size()));
    }
    return result;
}

template <bool ReuseTapWeights>
[[nodiscard]] DoubleCsr make_zimg_forward(const AxisPlanRequest &request,
                                          std::int32_t support) {
    const std::int32_t rows = request.source_size;
    const std::int32_t columns = request.destination_size;
    const double scale = static_cast<double>(rows) / request.active_length;
    const double step = std::min(scale, 1.0);
    const double expanded_support = static_cast<double>(support) / step;
    if (!std::isfinite(expanded_support)
        || expanded_support > static_cast<double>(std::numeric_limits<std::int32_t>::max() / 2)) {
        throw std::length_error("zimg forward filter width is too large");
    }
    const std::int32_t filter_size = std::max(
        static_cast<std::int32_t>(std::ceil(expanded_support)) * 2, 1);
    const ZimgKernel kernel(request.filter);

    DoubleCsr result;
    result.offsets.reserve(static_cast<std::size_t>(rows) + 1U);
    result.indices.reserve(static_cast<std::size_t>(rows)
                           * static_cast<std::size_t>(std::min(filter_size, columns)));
    result.weights.reserve(result.indices.capacity());
    result.offsets.push_back(0U);

    std::vector<std::int32_t> tap_indices(static_cast<std::size_t>(filter_size));
    std::vector<double> tap_weights(static_cast<std::size_t>(filter_size));
    std::vector<double> row_weights;
    for (std::int32_t row = 0; row < rows; ++row) {
        const double position = (static_cast<double>(row) + 0.5) / scale + request.shift;
        const double begin = round_half_up(position - static_cast<double>(filter_size) / 2.0) + 0.5;
        double total = 0.0;
        for (std::int32_t tap = 0; tap < filter_size; ++tap) {
            const double weight = kernel.weight(
                (begin + static_cast<double>(tap) - position) * step);
            if constexpr (ReuseTapWeights) {
                tap_weights[static_cast<std::size_t>(tap)] = weight;
            }
            total += weight;
        }
        if (!std::isfinite(total) || total == 0.0) {
            throw std::runtime_error("zimg forward filter produced a zero or non-finite weight sum");
        }

        std::int32_t left = columns;
        std::int32_t right = 0;
        for (std::int32_t tap = 0; tap < filter_size; ++tap) {
            const double pixel_center = begin + static_cast<double>(tap);
            double mapped = pixel_center;
            if (pixel_center < 0.0) {
                mapped = -pixel_center;
            } else if (pixel_center >= static_cast<double>(columns)) {
                mapped = 2.0 * static_cast<double>(columns) - pixel_center;
            }
            mapped = std::clamp(
                mapped, 0.0,
                std::nextafter(static_cast<double>(columns),
                               -std::numeric_limits<double>::infinity()));
            const auto index = static_cast<std::int32_t>(std::floor(mapped));
            tap_indices[static_cast<std::size_t>(tap)] = index;
            double raw_weight = 0.0;
            if constexpr (ReuseTapWeights) {
                raw_weight = tap_weights[static_cast<std::size_t>(tap)];
            } else {
                raw_weight = kernel.weight((pixel_center - position) * step);
            }
            tap_weights[static_cast<std::size_t>(tap)] = raw_weight / total;
            left = std::min(left, index);
            right = std::max(right, index + 1);
        }

        row_weights.assign(static_cast<std::size_t>(right - left), 0.0);
        for (std::int32_t tap = 0; tap < filter_size; ++tap) {
            row_weights[static_cast<std::size_t>(tap_indices[static_cast<std::size_t>(tap)] - left)] +=
                tap_weights[static_cast<std::size_t>(tap)];
        }
        for (std::int32_t index = left; index < right; ++index) {
            result.indices.push_back(index);
            result.weights.push_back(row_weights[static_cast<std::size_t>(index - left)]);
        }
        result.offsets.push_back(static_cast<std::uint32_t>(result.indices.size()));
    }
    return result;
}

[[nodiscard]] DoubleCsr transpose_csr(const DoubleCsr &input, std::int32_t rows,
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

[[nodiscard]] std::vector<double> form_normal_bands(const DoubleCsr &forward,
                                                    std::int32_t rows,
                                                    std::int32_t columns,
                                                    std::int32_t half_bandwidth) {
    const auto n = static_cast<std::size_t>(columns);
    std::vector<double> bands((static_cast<std::size_t>(half_bandwidth) + 1U) * n, 0.0);
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
    return bands;
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
        && active_length > 0.0 && std::isfinite(active_length) && std::isfinite(shift)
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

namespace {

[[nodiscard]] AxisPlan build_axis_plan_impl(
    const AxisPlanRequest &request,
    detail::TapEvaluationMode tap_evaluation) {
    if (request.source_size <= 0 || request.destination_size <= 0) {
        throw std::invalid_argument("axis dimensions must be positive");
    }
    if (!(request.active_length > 0.0) || !std::isfinite(request.active_length)
        || !std::isfinite(request.shift)) {
        throw std::invalid_argument("active length and shift must be finite, with positive active length");
    }
    const std::int32_t support = request.filter.support();
    if (support > (std::numeric_limits<std::int32_t>::max() - 1) / 2) {
        throw std::invalid_argument("filter support is too large");
    }
    const std::int32_t half_bandwidth = std::min(2 * support - 1, request.destination_size - 1);
    DoubleCsr descale_matrix = tap_evaluation == detail::TapEvaluationMode::reuse
        ? make_descale_matrix<true>(request, support)
        : make_descale_matrix<false>(request, support);
    DoubleCsr transpose = transpose_csr(
        descale_matrix, request.source_size, request.destination_size);
    DoubleCsr zimg_forward = tap_evaluation == detail::TapEvaluationMode::reuse
        ? make_zimg_forward<true>(request, support)
        : make_zimg_forward<false>(request, support);
    std::vector<double> factors = form_normal_bands(
        descale_matrix, request.source_size, request.destination_size, half_bandwidth);
    factor_banded_ldlt(factors, request.destination_size, half_bandwidth);

    AxisPlan plan;
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
    plan.transpose_offsets = std::move(transpose.offsets);
    plan.transpose_indices = std::move(transpose.indices);
    plan.transpose_weights.assign(transpose.weights.begin(), transpose.weights.end());

    const auto n = static_cast<std::size_t>(request.destination_size);
    const auto factor_count = static_cast<std::size_t>(half_bandwidth) * n;
    plan.lower_ld.assign(factor_count, 0.0F);
    plan.upper_l.assign(factor_count, 0.0F);
    plan.inverse_diagonal.resize(n);
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    for (std::int32_t i = 0; i < request.destination_size; ++i) {
        const double diagonal = factors[static_cast<std::size_t>(i)];
        plan.inverse_diagonal[static_cast<std::size_t>(i)] = static_cast<float>(1.0 / (diagonal + epsilon));
        const std::int32_t available = std::min(half_bandwidth, request.destination_size - i - 1);
        for (std::int32_t distance = 1; distance <= available; ++distance) {
            const float l = static_cast<float>(factors[static_cast<std::size_t>(distance) * n
                                                        + static_cast<std::size_t>(i)]);
            plan.upper_l[static_cast<std::size_t>(distance - 1) * n + static_cast<std::size_t>(i)] = l;
            const std::int32_t row = i + distance;
            plan.lower_ld[static_cast<std::size_t>(distance - 1) * n + static_cast<std::size_t>(row)] =
                static_cast<float>(factors[static_cast<std::size_t>(distance) * n + static_cast<std::size_t>(i)]
                                   * diagonal);
        }
    }
    return plan;
}

} // namespace

AxisPlan build_axis_plan(const AxisPlanRequest &request) {
    return build_axis_plan_impl(request, detail::TapEvaluationMode::reuse);
}

namespace detail {

AxisPlan build_axis_plan_with_tap_evaluation(
    const AxisPlanRequest &request,
    TapEvaluationMode mode) {
    return build_axis_plan_impl(request, mode);
}

} // namespace detail

struct AxisPlanCache::Impl {
    mutable std::mutex mutex;
    std::unordered_map<detail::PlanKey, std::shared_ptr<const AxisPlan>,
                       detail::PlanKeyHash> plans;
};

AxisPlanCache::AxisPlanCache() : impl_(std::make_unique<Impl>()) {}
AxisPlanCache::~AxisPlanCache() = default;

std::shared_ptr<const AxisPlan> AxisPlanCache::get_or_build(const AxisPlanRequest &request) {
    const detail::PlanKey key = detail::plan_key(request);
    {
        const std::scoped_lock lock(impl_->mutex);
        if (const auto found = impl_->plans.find(key); found != impl_->plans.end()) {
            return found->second;
        }
    }
    auto candidate = std::make_shared<const AxisPlan>(build_axis_plan(request));
    const std::scoped_lock lock(impl_->mutex);
    const auto [position, inserted] = impl_->plans.emplace(key, candidate);
    return inserted ? std::move(candidate) : position->second;
}

std::size_t AxisPlanCache::size() const {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->plans.size();
}

void AxisPlanCache::clear() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->plans.clear();
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
            sum += plan.transpose_weights[p]
                * input[static_cast<std::ptrdiff_t>(plan.transpose_indices[p]) * input_stride];
        }
        const std::int32_t available = std::min(c, i);
        // Far-to-near ordering matches descale's scalar float solve.
        for (std::int32_t distance = available; distance >= 1; --distance) {
            sum -= plan.lower_ld[static_cast<std::size_t>(distance - 1) * width + static_cast<std::size_t>(i)]
                * output[static_cast<std::ptrdiff_t>(i - distance) * output_stride];
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
                sum += plan.upper_l[static_cast<std::size_t>(distance - 1) * width + static_cast<std::size_t>(i)]
                    * output[static_cast<std::ptrdiff_t>(i + distance) * output_stride];
            }
        } else {
            for (std::int32_t distance = available; distance >= 1; --distance) {
                sum += plan.upper_l[static_cast<std::size_t>(distance - 1) * width + static_cast<std::size_t>(i)]
                    * output[static_cast<std::ptrdiff_t>(i + distance) * output_stride];
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
            sum += plan.forward_weights[begin + static_cast<std::size_t>(tap)]
                * input[static_cast<std::ptrdiff_t>(left + tap) * input_stride];
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

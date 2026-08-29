#include "getnative/cpu_analysis.hpp"
#include "getnative/joining_thread.hpp"

#include "inverse_columns.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace getnative {
namespace {

[[nodiscard]] std::size_t checked_area(std::int32_t width, std::int32_t height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("image dimensions must be positive");
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h) {
        throw std::length_error("image dimensions overflow addressable memory");
    }
    return w * h;
}

void validate(ConstImageView image) {
    (void)checked_area(image.width, image.height);
    if (image.data == nullptr || image.stride < image.width) {
        throw std::invalid_argument("invalid const image view");
    }
}

void validate(ImageView image) {
    (void)checked_area(image.width, image.height);
    if (image.data == nullptr || image.stride < image.width) {
        throw std::invalid_argument("invalid mutable image view");
    }
}

struct MetricBounds {
    std::int32_t x_begin;
    std::int32_t x_end;
    std::int32_t y_begin;
    std::int32_t y_end;
    double count;
};

[[nodiscard]] MetricBounds metric_bounds(ConstImageView source, const MetricSpec &metric) {
    if (metric.crop_left < 0 || metric.crop_right < 0 || metric.crop_top < 0
        || metric.crop_bottom < 0 || !std::isfinite(metric.threshold) || metric.threshold < 0.0F
        || metric.norm == 0U) {
        throw std::invalid_argument("invalid metric configuration");
    }
    MetricBounds bounds{
        metric.crop_left,
        source.width - metric.crop_right,
        metric.crop_top,
        source.height - metric.crop_bottom,
        0.0,
    };
    if (bounds.x_begin >= bounds.x_end || bounds.y_begin >= bounds.y_end) {
        throw std::invalid_argument("metric crop removes the entire image");
    }
    bounds.count = static_cast<double>(bounds.x_end - bounds.x_begin)
        * static_cast<double>(bounds.y_end - bounds.y_begin);
    return bounds;
}

class MetricAccumulator {
public:
    explicit MetricAccumulator(const MetricSpec &metric) noexcept
        : metric_(metric) {}

    void add(float difference) {
        if (!(difference > metric_.threshold)) {
            return;
        }
        if (metric_.norm == 1U) {
            sum_ += static_cast<double>(difference);
        } else if (metric_.norm == 2U) {
            sum_ += static_cast<double>(difference * difference);
        } else if (metric_.norm == 3U) {
            sum_ += static_cast<double>(difference * difference * difference);
        } else if (metric_.norm == 4U) {
            const float square = difference * difference;
            sum_ += static_cast<double>(square * square);
        } else {
            add_scaled(static_cast<double>(difference));
        }
    }

    [[nodiscard]] double finish(double count) const {
        if (metric_.norm <= 4U) {
            const double mean = sum_ / count;
            return metric_.norm == 1U
                ? mean : std::pow(mean, 1.0 / static_cast<double>(metric_.norm));
        }
        if (scale_ == 0.0) {
            return 0.0;
        }
        return scale_ * std::pow(scaled_sum_ / count,
                                 1.0 / static_cast<double>(metric_.norm));
    }

    [[nodiscard]] bool is_norm1() const noexcept { return metric_.norm == 1U; }
    [[nodiscard]] float threshold() const noexcept { return metric_.threshold; }
    [[nodiscard]] double norm1_sum() const noexcept { return sum_; }
    void set_norm1_sum(double sum) noexcept { sum_ = sum; }

private:
    void add_scaled(double difference) {
        const double exponent = static_cast<double>(metric_.norm);
        if (difference > scale_) {
            if (scale_ != 0.0) {
                scaled_sum_ *= std::pow(scale_ / difference, exponent);
            }
            scale_ = difference;
            scaled_sum_ += 1.0;
        } else if (difference == scale_) {
            scaled_sum_ += 1.0;
        } else {
            scaled_sum_ += std::pow(difference / scale_, exponent);
        }
    }

    const MetricSpec &metric_;
    double sum_ = 0.0;
    double scale_ = 0.0;
    double scaled_sum_ = 0.0;
};

void add_absolute_difference_row(
    const float *source, const float *reconstruction,
    std::int32_t x_begin, std::int32_t x_end,
    MetricAccumulator &accumulator, detail::ColumnDispatchPolicy policy,
    const detail::AnalysisRowDispatch &dispatch) {
    (void)policy;
    std::int32_t x = x_begin;
    if (accumulator.is_norm1()
        && dispatch.absolute_difference_norm1 != nullptr) {
        const std::int32_t vector_end = x
            + (x_end - x) / dispatch.lanes * dispatch.lanes;
        if (x < vector_end) {
            accumulator.set_norm1_sum(dispatch.absolute_difference_norm1(
                source, reconstruction, x, vector_end, accumulator.threshold(),
                accumulator.norm1_sum()));
            x = vector_end;
        }
    } else if (dispatch.absolute_difference != nullptr) {
        alignas(64) float differences[16];
        for (; x <= x_end - dispatch.lanes; x += dispatch.lanes) {
            dispatch.absolute_difference(source + x, reconstruction + x, differences);
            for (std::int32_t lane = 0; lane < dispatch.lanes; ++lane) {
                accumulator.add(differences[lane]);
            }
        }
    }
    for (; x < x_end; ++x) {
        accumulator.add(std::abs(source[x] - reconstruction[x]));
    }
}

void add_vertical_reconstruction_row(
    const AxisPlan &plan, std::uint32_t begin, std::int32_t left,
    const float *source, const float *native, std::ptrdiff_t native_stride,
    std::int32_t x_begin, std::int32_t x_end,
    MetricAccumulator &accumulator, detail::ColumnDispatchPolicy policy,
    const detail::AnalysisRowDispatch &dispatch) {
    (void)policy;
    std::int32_t x = x_begin;
    if (accumulator.is_norm1()
        && dispatch.vertical_reconstruction_norm1 != nullptr) {
        const std::int32_t vector_end = x
            + (x_end - x) / dispatch.lanes * dispatch.lanes;
        if (x < vector_end) {
            accumulator.set_norm1_sum(dispatch.vertical_reconstruction_norm1(
                plan, begin, left, source, native, native_stride,
                x, vector_end, accumulator.threshold(), accumulator.norm1_sum()));
            x = vector_end;
        }
    } else if (dispatch.vertical_reconstruction != nullptr) {
        alignas(64) float differences[16];
        for (; x <= x_end - dispatch.lanes; x += dispatch.lanes) {
            dispatch.vertical_reconstruction(
                plan, begin, left, source, native, native_stride, x, differences);
            for (std::int32_t lane = 0; lane < dispatch.lanes; ++lane) {
                accumulator.add(differences[lane]);
            }
        }
    }
    for (; x < x_end; ++x) {
        float reconstructed = 0.0F;
        for (std::int32_t tap = 0; tap < plan.forward_width; ++tap) {
            reconstructed = std::fma(
                plan.forward_weights[begin + static_cast<std::uint32_t>(tap)],
                native[static_cast<std::ptrdiff_t>(left + tap) * native_stride + x],
                reconstructed);
        }
        accumulator.add(std::abs(source[x] - reconstructed));
    }
}

} // namespace

CpuWorkspace::CpuWorkspace(std::size_t maximum_elements)
    : maximum_elements_(maximum_elements) {}

void CpuWorkspace::reserve(std::int32_t source_width, std::int32_t source_height,
                           std::int32_t native_width, std::int32_t native_height,
                           AnalysisAxes axes) {
    (void)checked_area(source_width, source_height);
    (void)checked_area(native_width, native_height);
    const std::size_t inverse_intermediate = axes == AnalysisAxes::vertical
        ? 0U : axes == AnalysisAxes::horizontal
            ? checked_area(native_width, 4)
            : checked_area(native_width, source_height);
    const std::size_t horizontal_first_intermediate = axes == AnalysisAxes::both
        ? checked_area(source_width, native_height) : 0U;
    const std::size_t intermediate_count =
        std::max(inverse_intermediate, horizontal_first_intermediate);
    const std::size_t native_count = axes == AnalysisAxes::horizontal
        ? 0U : checked_area(axes == AnalysisAxes::vertical ? source_width : native_width, native_height);
    const std::size_t row_count = axes == AnalysisAxes::vertical
        ? 0U : axes == AnalysisAxes::horizontal
            ? checked_area(source_width, 4)
            : static_cast<std::size_t>(source_width);
    if (intermediate_count > std::numeric_limits<std::size_t>::max() - native_count
        || intermediate_count + native_count
            > std::numeric_limits<std::size_t>::max() - row_count) {
        throw std::length_error("workspace size overflow");
    }
    const std::size_t total = intermediate_count + native_count + row_count;
    if (maximum_elements_ != 0 && total > maximum_elements_) {
        throw std::length_error("workspace element limit exceeded");
    }
    intermediate.resize(intermediate_count);
    native.resize(native_count);
    reconstruction_row.resize(row_count);
    peak_elements_ = std::max(peak_elements_, total);
}

std::size_t CpuWorkspace::maximum_elements() const noexcept { return maximum_elements_; }
std::size_t CpuWorkspace::current_elements() const noexcept {
    return intermediate.size() + native.size() + reconstruction_row.size();
}
std::size_t CpuWorkspace::peak_elements() const noexcept { return peak_elements_; }

ForwardOrder select_forward_order(const AxisPlan &horizontal,
                                  const AxisPlan &vertical) noexcept {
    const double x_scale = static_cast<double>(horizontal.source_size)
        / horizontal.active_length;
    const double y_scale = static_cast<double>(vertical.source_size)
        / vertical.active_length;
    const double horizontal_first_cost = std::max(x_scale, 1.0) * 2.0
        + x_scale * std::max(y_scale, 1.0);
    const double vertical_first_cost = std::max(y_scale, 1.0)
        + y_scale * std::max(x_scale, 1.0) * 2.0;
    return horizontal_first_cost < vertical_first_cost
        ? ForwardOrder::horizontal_first : ForwardOrder::vertical_first;
}

void descale_2d_f32(ConstImageView source, const AxisPlan &horizontal,
                    const AxisPlan &vertical, CpuWorkspace &workspace,
                    ImageView native_output) {
    validate(source);
    validate(native_output);
    if (horizontal.source_size != source.width || vertical.source_size != source.height
        || horizontal.destination_size != native_output.width
        || vertical.destination_size != native_output.height) {
        throw std::invalid_argument("descale dimensions do not match axis plans");
    }
    workspace.reserve(source.width, source.height, native_output.width, native_output.height,
                      AnalysisAxes::both);
    const std::ptrdiff_t intermediate_stride = native_output.width;
    detail::inverse_rows_f32(
        horizontal, source.data, source.stride,
        workspace.intermediate.data(), intermediate_stride, source.height);
    detail::inverse_columns_f32(
        vertical, workspace.intermediate.data(), intermediate_stride,
        native_output.data, native_output.stride, native_output.width);
}

void reconstruct_2d_f32(ConstImageView native_source, const AxisPlan &horizontal,
                        const AxisPlan &vertical, CpuWorkspace &workspace,
                        ImageView output) {
    validate(native_source);
    validate(output);
    if (horizontal.destination_size != native_source.width
        || vertical.destination_size != native_source.height
        || horizontal.source_size != output.width || vertical.source_size != output.height) {
        throw std::invalid_argument("reconstruction dimensions do not match axis plans");
    }
    workspace.reserve(output.width, output.height, native_source.width, native_source.height,
                      AnalysisAxes::both);
    if (select_forward_order(horizontal, vertical) == ForwardOrder::vertical_first) {
        const std::ptrdiff_t intermediate_stride = native_source.width;
        detail::forward_columns_f32(
            vertical, native_source.data, native_source.stride,
            workspace.intermediate.data(), intermediate_stride, native_source.width);
        detail::forward_rows_f32(
            horizontal, workspace.intermediate.data(), intermediate_stride,
            output.data, output.stride, output.height);
    } else {
        const std::ptrdiff_t intermediate_stride = output.width;
        detail::forward_rows_f32(
            horizontal, native_source.data, native_source.stride,
            workspace.intermediate.data(), intermediate_stride, native_source.height);
        detail::forward_columns_f32(
            vertical, workspace.intermediate.data(), intermediate_stride,
            output.data, output.stride, output.width);
    }
}

double thresholded_p_norm(ConstImageView source, ConstImageView reconstruction,
                          const MetricSpec &metric) {
    validate(source);
    validate(reconstruction);
    if (source.width != reconstruction.width || source.height != reconstruction.height) {
        throw std::invalid_argument("metric images must have matching dimensions");
    }
    const MetricBounds bounds = metric_bounds(source, metric);
    const detail::AnalysisRowDispatch row_dispatch =
        detail::analysis_row_dispatch(detail::ColumnDispatchPolicy::automatic);

    MetricAccumulator accumulator(metric);
    for (std::int32_t y = bounds.y_begin; y < bounds.y_end; ++y) {
        const float *source_row = source.data + static_cast<std::ptrdiff_t>(y) * source.stride;
        const float *reconstruction_row = reconstruction.data
            + static_cast<std::ptrdiff_t>(y) * reconstruction.stride;
        add_absolute_difference_row(
            source_row, reconstruction_row, bounds.x_begin, bounds.x_end, accumulator,
            detail::ColumnDispatchPolicy::automatic, row_dispatch);
    }
    return accumulator.finish(bounds.count);
}

namespace {

double analyze_candidate_impl(ConstImageView source, const AxisPlan &horizontal,
                              const AxisPlan &vertical, const MetricSpec &metric,
                              CpuWorkspace &workspace,
                              detail::ColumnDispatchPolicy column_policy) {
    detail::validate_column_dispatch_policy(column_policy);
    validate(source);
    if (horizontal.source_size != source.width || vertical.source_size != source.height) {
        throw std::invalid_argument("candidate plans do not match source dimensions");
    }
    const MetricBounds bounds = metric_bounds(source, metric);
    const detail::AnalysisRowDispatch row_dispatch =
        detail::analysis_row_dispatch(column_policy);
    workspace.reserve(source.width, source.height,
                      horizontal.destination_size, vertical.destination_size,
                      AnalysisAxes::both);
    const std::ptrdiff_t horizontal_stride = horizontal.destination_size;
    detail::inverse_rows_f32(
        horizontal, source.data, source.stride,
        workspace.intermediate.data(), horizontal_stride, source.height,
        column_policy);
    const std::ptrdiff_t native_stride = horizontal.destination_size;
    detail::inverse_columns_f32(
        vertical, workspace.intermediate.data(), horizontal_stride,
        workspace.native.data(), native_stride, horizontal.destination_size,
        column_policy);

    MetricAccumulator accumulator(metric);
    if (select_forward_order(horizontal, vertical) == ForwardOrder::vertical_first) {
        detail::forward_columns_f32(
            vertical, workspace.native.data(), native_stride,
            workspace.intermediate.data(), horizontal_stride,
            horizontal.destination_size, column_policy);
        for (std::int32_t y = bounds.y_begin; y < bounds.y_end; ++y) {
            forward_axis_f32(horizontal,
                             workspace.intermediate.data()
                                 + static_cast<std::ptrdiff_t>(y) * horizontal_stride,
                             1, workspace.reconstruction_row.data(), 1);
            const float *source_row = source.data
                + static_cast<std::ptrdiff_t>(y) * source.stride;
            add_absolute_difference_row(
                source_row, workspace.reconstruction_row.data(),
                bounds.x_begin, bounds.x_end, accumulator,
                column_policy, row_dispatch);
        }
    } else {
        const std::ptrdiff_t intermediate_stride = source.width;
        detail::forward_rows_f32(
            horizontal, workspace.native.data(), native_stride,
            workspace.intermediate.data(), intermediate_stride,
            vertical.destination_size, column_policy);
        for (std::int32_t y = bounds.y_begin; y < bounds.y_end; ++y) {
            const std::uint32_t begin = vertical.forward_offsets[static_cast<std::size_t>(y)];
            const std::int32_t left = vertical.forward_indices[begin];
            const float *source_row = source.data
                + static_cast<std::ptrdiff_t>(y) * source.stride;
            add_vertical_reconstruction_row(
                vertical, begin, left, source_row, workspace.intermediate.data(),
                intermediate_stride, bounds.x_begin, bounds.x_end, accumulator,
                column_policy, row_dispatch);
        }
    }
    return accumulator.finish(bounds.count);
}

double analyze_axis_candidate_impl(ConstImageView source, const AxisPlan &axis,
                                   AnalysisAxes axis_direction, const MetricSpec &metric,
                                   CpuWorkspace &workspace,
                                   detail::ColumnDispatchPolicy column_policy) {
    detail::validate_column_dispatch_policy(column_policy);
    validate(source);
    const MetricBounds bounds = metric_bounds(source, metric);
    const detail::AnalysisRowDispatch row_dispatch =
        detail::analysis_row_dispatch(column_policy);
    if (axis_direction == AnalysisAxes::both) {
        throw std::invalid_argument("single-axis analysis requires horizontal or vertical direction");
    }
    MetricAccumulator accumulator(metric);
    if (axis_direction == AnalysisAxes::horizontal) {
        if (axis.source_size != source.width) {
            throw std::invalid_argument("horizontal plan does not match source width");
        }
        workspace.reserve(source.width, source.height, axis.destination_size, source.height,
                          AnalysisAxes::horizontal);
        const std::ptrdiff_t native_stride = axis.destination_size;
        const std::ptrdiff_t reconstruction_stride = source.width;
        for (std::int32_t y = bounds.y_begin; y < bounds.y_end; ) {
            const std::int32_t count = std::min(4, bounds.y_end - y);
            detail::inverse_rows_f32(
                axis,
                source.data + static_cast<std::ptrdiff_t>(y) * source.stride,
                source.stride, workspace.intermediate.data(), native_stride,
                count, column_policy);
            detail::forward_rows_f32(
                axis, workspace.intermediate.data(), native_stride,
                workspace.reconstruction_row.data(), reconstruction_stride,
                count, column_policy);
            for (std::int32_t row = 0; row < count; ++row) {
                const float *source_row = source.data
                    + static_cast<std::ptrdiff_t>(y + row) * source.stride;
                add_absolute_difference_row(
                    source_row,
                    workspace.reconstruction_row.data()
                        + static_cast<std::ptrdiff_t>(row) * reconstruction_stride,
                    bounds.x_begin, bounds.x_end, accumulator,
                    column_policy, row_dispatch);
            }
            y += count;
        }
    } else {
        if (axis.source_size != source.height) {
            throw std::invalid_argument("vertical plan does not match source height");
        }
        workspace.reserve(source.width, source.height, source.width, axis.destination_size,
                          AnalysisAxes::vertical);
        const std::ptrdiff_t native_stride = source.width;
        detail::inverse_columns_f32(
            axis, source.data, source.stride, workspace.native.data(), native_stride,
            bounds.x_begin, bounds.x_end - bounds.x_begin, column_policy);
        for (std::int32_t y = bounds.y_begin; y < bounds.y_end; ++y) {
            const std::uint32_t begin = axis.forward_offsets[static_cast<std::size_t>(y)];
            const std::int32_t left = axis.forward_indices[begin];
            const float *source_row = source.data + static_cast<std::ptrdiff_t>(y) * source.stride;
            add_vertical_reconstruction_row(
                axis, begin, left, source_row, workspace.native.data(), native_stride,
                bounds.x_begin, bounds.x_end, accumulator,
                column_policy, row_dispatch);
        }
    }
    return accumulator.finish(bounds.count);
}

std::vector<CandidateResult> analyze_batch_impl(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::size_t worker_count,
    std::size_t workspace_limit_elements,
    detail::ColumnDispatchPolicy column_policy) {
    detail::validate_column_dispatch_policy(column_policy);
    validate(source);
    std::vector<CandidateResult> results(candidates.size());
    if (candidates.empty()) {
        return results;
    }
    if (worker_count == 0) {
        worker_count = std::max<std::size_t>(1U, std::thread::hardware_concurrency());
    }
    worker_count = std::min(worker_count, candidates.size());
    std::atomic_size_t cursor{0};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    std::vector<JoiningThread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            CpuWorkspace workspace(workspace_limit_elements);
            while (true) {
                const std::size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
                if (index >= candidates.size()) {
                    break;
                }
                try {
                    const CandidateAnalysis &candidate = candidates[index];
                    double error = 0.0;
                    if (candidate.axes == AnalysisAxes::both) {
                        if (!candidate.horizontal || !candidate.vertical) {
                            throw std::invalid_argument("two-axis candidate contains a null plan");
                        }
                        error = analyze_candidate_impl(
                            source, *candidate.horizontal, *candidate.vertical,
                            metric, workspace, column_policy);
                    } else {
                        const auto &plan = candidate.axes == AnalysisAxes::horizontal
                            ? candidate.horizontal : candidate.vertical;
                        if (!plan) {
                            throw std::invalid_argument(
                                "single-axis candidate contains a null plan");
                        }
                        error = analyze_axis_candidate_impl(
                            source, *plan, candidate.axes, metric, workspace,
                            column_policy);
                    }
                    results[index] = {candidate.id, error};
                } catch (...) {
                    const std::scoped_lock lock(failure_mutex);
                    if (!failure) {
                        failure = std::current_exception();
                    }
                    cursor.store(candidates.size(), std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    workers.clear(); // JoiningThread joins here, before failure is inspected.
    if (failure) {
        std::rethrow_exception(failure);
    }
    return results;
}

} // namespace

double analyze_candidate_f32(ConstImageView source, const AxisPlan &horizontal,
                             const AxisPlan &vertical, const MetricSpec &metric,
                             CpuWorkspace &workspace) {
    return analyze_candidate_impl(
        source, horizontal, vertical, metric, workspace,
        detail::ColumnDispatchPolicy::automatic);
}

double analyze_axis_candidate_f32(ConstImageView source, const AxisPlan &axis,
                                  AnalysisAxes axis_direction, const MetricSpec &metric,
                                  CpuWorkspace &workspace) {
    return analyze_axis_candidate_impl(
        source, axis, axis_direction, metric, workspace,
        detail::ColumnDispatchPolicy::automatic);
}

namespace detail {

double analyze_axis_candidate_with_column_policy_f32(
    ConstImageView source, const AxisPlan &axis, AnalysisAxes axis_direction,
    const MetricSpec &metric, CpuWorkspace &workspace, ColumnDispatchPolicy policy) {
    return analyze_axis_candidate_impl(
        source, axis, axis_direction, metric, workspace, policy);
}

double analyze_candidate_with_column_policy_f32(
    ConstImageView source, const AxisPlan &horizontal, const AxisPlan &vertical,
    const MetricSpec &metric, CpuWorkspace &workspace, ColumnDispatchPolicy policy) {
    return analyze_candidate_impl(
        source, horizontal, vertical, metric, workspace, policy);
}

std::vector<CandidateResult> analyze_batch_with_column_policy_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, ColumnDispatchPolicy policy,
    std::size_t worker_count, std::size_t workspace_limit_elements) {
    return analyze_batch_impl(
        source, candidates, metric, worker_count, workspace_limit_elements, policy);
}

} // namespace detail

std::vector<CandidateResult> analyze_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::size_t worker_count,
    std::size_t workspace_limit_elements) {
    return analyze_batch_impl(
        source, candidates, metric, worker_count, workspace_limit_elements,
        detail::ColumnDispatchPolicy::automatic);
}

} // namespace getnative

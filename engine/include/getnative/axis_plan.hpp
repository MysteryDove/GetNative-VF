#pragma once

#include "getnative/filter.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace getnative {

enum class BorderMode : std::uint8_t {
    zero,
    repeat,
    mirror,
};

struct AxisPlanRequest {
    std::int32_t source_size = 0;      // Observed/rescaled axis length.
    std::int32_t destination_size = 0; // Native axis length to solve for.
    double active_length = 0.0;        // Fractional native source extent.
    double shift = 0.0;
    Filter filter = Filter::bicubic();
    BorderMode border = BorderMode::mirror;
};

// Immutable, GPU-uploadable description of A and the LDLT factorization of A^T A.
// Factor bands use structure-of-arrays layout: band * destination_size + element.
struct AxisPlan {
    std::int32_t source_size = 0;
    std::int32_t destination_size = 0;
    std::int32_t support = 0;
    std::int32_t half_bandwidth = 0;
    std::int32_t forward_width = 0;
    double active_length = 0.0;
    double shift = 0.0;

    // Zimg-compatible forward rows. Every row has forward_width contiguous taps,
    // including residual-carry padding coefficients at image boundaries.
    std::vector<std::uint32_t> forward_offsets;
    std::vector<std::int32_t> forward_indices;
    std::vector<float> forward_weights;

    // Sparse A^T rows, ordered by source observation index.
    std::vector<std::uint32_t> transpose_offsets;
    std::vector<std::int32_t> transpose_indices;
    std::vector<float> transpose_weights;

    // lower_ld[d-1, i] = L(i, i-d) * D(i-d); upper_l[d-1, i] = L(i+d, i).
    std::vector<float> lower_ld;
    std::vector<float> upper_l;
    std::vector<float> inverse_diagonal;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t packed_factor_elements() const noexcept;
};

[[nodiscard]] AxisPlan build_axis_plan(const AxisPlanRequest &request);

class AxisPlanCache {
public:
    AxisPlanCache();
    ~AxisPlanCache();
    AxisPlanCache(const AxisPlanCache &) = delete;
    AxisPlanCache &operator=(const AxisPlanCache &) = delete;

    [[nodiscard]] std::shared_ptr<const AxisPlan> get_or_build(const AxisPlanRequest &request);
    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// These hot-path functions perform no allocation. Output may not alias input.
void inverse_axis_f32(const AxisPlan &plan,
                      const float *input, std::ptrdiff_t input_stride,
                      float *output, std::ptrdiff_t output_stride);
void forward_axis_f32(const AxisPlan &plan,
                      const float *input, std::ptrdiff_t input_stride,
                      float *output, std::ptrdiff_t output_stride);

inline void inverse_axis_f32(const AxisPlan &plan,
                             std::span<const float> input,
                             std::span<float> output) {
    if (input.size() < static_cast<std::size_t>(plan.source_size)
        || output.size() < static_cast<std::size_t>(plan.destination_size)) {
        throw std::invalid_argument("inverse axis spans are too small");
    }
    inverse_axis_f32(plan, input.data(), 1, output.data(), 1);
}

inline void forward_axis_f32(const AxisPlan &plan,
                             std::span<const float> input,
                             std::span<float> output) {
    if (input.size() < static_cast<std::size_t>(plan.destination_size)
        || output.size() < static_cast<std::size_t>(plan.source_size)) {
        throw std::invalid_argument("forward axis spans are too small");
    }
    forward_axis_f32(plan, input.data(), 1, output.data(), 1);
}

} // namespace getnative

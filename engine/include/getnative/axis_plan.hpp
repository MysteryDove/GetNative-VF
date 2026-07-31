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
[[nodiscard]] std::size_t axis_plan_storage_bytes(const AxisPlan &plan) noexcept;

struct AxisPlanCacheLimits {
    std::size_t maximum_entries = 1024U;
    std::size_t maximum_resident_bytes = 256U * 1024U * 1024U;
};

struct AxisPlanCacheBatchResult {
    std::vector<std::shared_ptr<const AxisPlan>> plans;
    std::size_t unique_key_count = 0;
    // Counts input requests whose plan was resident when this call began.
    std::size_t ready_hit_count = 0;
    std::size_t physical_build_count = 0;
    std::size_t published_plan_count = 0;
    std::size_t peak_active_builds = 0;
    std::size_t effective_worker_count = 0;
    std::size_t resident_entry_count = 0;
    std::size_t resident_bytes = 0;
};

// Caller-owned cache for a bounded scan/video session. Batch misses use the
// bounded planner workers and publish only after the complete batch succeeds.
// Concurrent cold calls may duplicate construction; this is not single-flight.
// Limits use fixed admission: overflow plans are returned but not retained.
class AxisPlanCache {
public:
    explicit AxisPlanCache(AxisPlanCacheLimits limits = {});
    ~AxisPlanCache();
    AxisPlanCache(const AxisPlanCache &) = delete;
    AxisPlanCache &operator=(const AxisPlanCache &) = delete;

    [[nodiscard]] std::shared_ptr<const AxisPlan> get_or_build(const AxisPlanRequest &request);
    [[nodiscard]] AxisPlanCacheBatchResult get_or_build_batch(
        std::span<const AxisPlanRequest> requests,
        std::size_t worker_count = 0U);
    [[nodiscard]] AxisPlanCacheLimits limits() const noexcept;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t resident_bytes() const;
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

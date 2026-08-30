#pragma once

#include "getnative/axis_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace getnative {

struct ConstImageView {
    const float *data = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::ptrdiff_t stride = 0;
};

struct ImageView {
    float *data = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::ptrdiff_t stride = 0;
};

struct MetricSpec {
    std::int32_t crop_left = 10;
    std::int32_t crop_right = 10;
    std::int32_t crop_top = 10;
    std::int32_t crop_bottom = 10;
    float threshold = 0.015F;
    std::uint32_t norm = 1U;
};

// GPU stage timers (CUDA events, Vulkan timestamps, Metal GPUStartTime) stay
// compiled. Default off so verify/scan do not pay per-dispatch instrumentation.
// GETNATIVE_GPU_STAGE_PROFILE=1|true|on|stages turns them on for the process.
enum class GpuStageProfile : std::uint8_t {
    off,
    stages,
};

[[nodiscard]] inline GpuStageProfile gpu_stage_profile_from_environment() noexcept {
    const char *value = std::getenv("GETNATIVE_GPU_STAGE_PROFILE");
    if (value == nullptr || *value == '\0') return GpuStageProfile::off;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0
        || std::strcmp(value, "false") == 0 || std::strcmp(value, "no") == 0) {
        return GpuStageProfile::off;
    }
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0
        || std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0
        || std::strcmp(value, "stages") == 0) {
        return GpuStageProfile::stages;
    }
    return GpuStageProfile::off;
}

enum class AnalysisAxes : std::uint8_t {
    horizontal,
    vertical,
    both,
};

enum class ForwardOrder : std::uint8_t {
    horizontal_first,
    vertical_first,
};

[[nodiscard]] ForwardOrder select_forward_order(const AxisPlan &horizontal,
                                                const AxisPlan &vertical) noexcept;

struct CpuWorkspace {
    explicit CpuWorkspace(std::size_t maximum_elements = 0);

    void reserve(std::int32_t source_width, std::int32_t source_height,
                 std::int32_t native_width, std::int32_t native_height,
                 AnalysisAxes axes = AnalysisAxes::both);
    [[nodiscard]] std::size_t maximum_elements() const noexcept;
    [[nodiscard]] std::size_t current_elements() const noexcept;
    [[nodiscard]] std::size_t peak_elements() const noexcept;

    // Reused for inverse-H and the first forward pass; never stores a full
    // reconstructed source frame.
    std::vector<float> intermediate;
    std::vector<float> native;
    std::vector<float> reconstruction_row;

private:
    std::size_t maximum_elements_ = 0;
    std::size_t peak_elements_ = 0;
};

struct CandidateAnalysis {
    std::string id;
    std::shared_ptr<const AxisPlan> horizontal;
    std::shared_ptr<const AxisPlan> vertical;
    AnalysisAxes axes = AnalysisAxes::both;
};

struct CandidateResult {
    std::string id;
    double error = 0.0;
};

void descale_2d_f32(ConstImageView source,
                    const AxisPlan &horizontal, const AxisPlan &vertical,
                    CpuWorkspace &workspace, ImageView native_output);
void reconstruct_2d_f32(ConstImageView native_source,
                        const AxisPlan &horizontal, const AxisPlan &vertical,
                        CpuWorkspace &workspace, ImageView output);

[[nodiscard]] double thresholded_p_norm(ConstImageView source,
                                        ConstImageView reconstruction,
                                        const MetricSpec &metric);

[[nodiscard]] double analyze_candidate_f32(ConstImageView source,
                                           const AxisPlan &horizontal,
                                           const AxisPlan &vertical,
                                           const MetricSpec &metric,
                                           CpuWorkspace &workspace);

[[nodiscard]] double analyze_axis_candidate_f32(ConstImageView source,
                                                const AxisPlan &axis,
                                                AnalysisAxes axis_direction,
                                                const MetricSpec &metric,
                                                CpuWorkspace &workspace);

// Results retain input order. Every candidate reduction is performed in fixed raster order.
[[nodiscard]] std::vector<CandidateResult> analyze_batch_f32(
    ConstImageView source, std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric, std::size_t worker_count = 0,
    std::size_t workspace_limit_elements = 0);

} // namespace getnative

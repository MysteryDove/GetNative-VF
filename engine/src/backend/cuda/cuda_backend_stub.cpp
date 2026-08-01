#include "getnative/cuda_analysis.hpp"

#include <stdexcept>

namespace getnative {

struct CudaAnalysisEngine::Impl {};

CudaRuntimeProbe cuda_runtime_probe() noexcept {
    CudaRuntimeProbe result;
    result.reason = "CUDA backend was not compiled";
    return result;
}

bool cuda_backend_available() noexcept { return false; }

std::vector<CudaDeviceInfo> enumerate_cuda_devices() { return {}; }

CudaAnalysisEngine::CudaAnalysisEngine(CudaAnalysisOptions)
    : impl_(std::make_unique<Impl>()) {
    throw std::runtime_error("CUDA backend was not compiled");
}

CudaAnalysisEngine::~CudaAnalysisEngine() = default;
CudaAnalysisEngine::CudaAnalysisEngine(CudaAnalysisEngine &&) noexcept = default;
CudaAnalysisEngine &CudaAnalysisEngine::operator=(CudaAnalysisEngine &&) noexcept = default;

const CudaDeviceInfo &CudaAnalysisEngine::device_info() const noexcept {
    static const CudaDeviceInfo unavailable;
    return unavailable;
}

const CudaAnalysisOptions &CudaAnalysisEngine::options() const noexcept {
    static const CudaAnalysisOptions unavailable;
    return unavailable;
}

std::size_t CudaAnalysisEngine::peak_workspace_elements() const noexcept { return 0U; }
std::size_t CudaAnalysisEngine::peak_working_set_bytes() const noexcept { return 0U; }
CudaRuntimeTelemetry CudaAnalysisEngine::runtime_telemetry() const { return {}; }
void CudaAnalysisEngine::reset_analysis_telemetry() {}
void CudaAnalysisEngine::trim_working_buffers() {}

std::vector<CandidateResult> CudaAnalysisEngine::analyze_axis_batch_f32(
    ConstImageView, std::span<const CandidateAnalysis>, const MetricSpec &,
    std::stop_token) {
    throw std::runtime_error("CUDA backend was not compiled");
}

} // namespace getnative

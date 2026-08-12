#pragma once

#include <ostream>
#include <string_view>

#if defined(GETNATIVE_HAS_CUDA)
namespace getnative {
class CudaAnalysisEngine;
}
#endif
#if defined(GETNATIVE_HAS_VULKAN)
namespace getnative {
class VulkanAnalysisEngine;
}
#endif

namespace getnative::cli {

// Writes the schema_version=2 capability envelope. analysis_available is
// true inside a worker session (analyze exists there), false for the
// one-shot CLI transport.
void write_capabilities(std::ostream &output, bool analysis_available);

#if defined(GETNATIVE_HAS_CUDA) || defined(GETNATIVE_HAS_VULKAN)
// Worker sessions pass resident accelerator engines (or their initialization
// failures) so capability discovery and the first job share initialization.
void write_capabilities(std::ostream &output, bool analysis_available,
#if defined(GETNATIVE_HAS_CUDA)
                        const getnative::CudaAnalysisEngine *resident_cuda,
                        std::string_view resident_cuda_error
#endif
#if defined(GETNATIVE_HAS_VULKAN)
#if defined(GETNATIVE_HAS_CUDA)
                        ,
#endif
                        const getnative::VulkanAnalysisEngine *resident_vulkan,
                        std::string_view resident_vulkan_error
#endif
                        );
#endif

} // namespace getnative::cli

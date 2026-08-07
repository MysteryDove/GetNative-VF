#include "capabilities.hpp"

#include "getnative/cpu_features.hpp"
#include "getnative/profile.hpp"
#if defined(GETNATIVE_HAS_METAL)
#include "getnative/metal_analysis.hpp"
#endif
#if defined(GETNATIVE_HAS_CUDA)
#include "getnative/cuda_analysis.hpp"
#endif

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace getnative::cli {
namespace {

std::string json_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result.push_back('"');
    return result;
}

void write_isa_set(std::ostream &output, const getnative::CpuIsaSet &set) {
    output << '[';
    bool first = true;
    auto append = [&](getnative::CpuIsa isa, bool present) {
        if (!present) return;
        if (!first) output << ',';
        first = false;
        output << json_string(getnative::cpu_isa_name(isa));
    };
    append(getnative::CpuIsa::scalar, set.scalar);
    append(getnative::CpuIsa::sse2, set.sse2);
    append(getnative::CpuIsa::avx2, set.avx2);
    append(getnative::CpuIsa::avx512, set.avx512);
    output << ']';
}

} // namespace

void write_capabilities(std::ostream &output, bool analysis_available) {
    const char *available = analysis_available ? "true" : "false";
    const getnative::CpuDispatchInfo cpu = getnative::cpu_dispatch_info();
    output << "{\"schema_version\":2,\"engine\":\"getnative-engine\",\"version\":\"0.1.0\","
              "\"commands\":{\"capabilities\":true,\"geometry\":true,\"analyze\":"
           << available
           << "},\"kernels\":["
              "{\"id\":\"bilinear\",\"parameters\":{\"kind\":\"none\"}},"
              "{\"id\":\"bicubic\",\"parameters\":{\"kind\":\"bicubic_bc\",\"finite\":true}},"
              "{\"id\":\"lanczos\",\"parameters\":{\"kind\":\"integer_taps\","
              "\"gui_min\":1,\"gui_max\":8,\"core_min\":1,\"core_max\":15}},"
              "{\"id\":\"spline16\",\"parameters\":{\"kind\":\"none\"}},"
              "{\"id\":\"spline36\",\"parameters\":{\"kind\":\"none\"}},"
              "{\"id\":\"spline64\",\"parameters\":{\"kind\":\"none\"}}],"
              "\"unsupported_features\":[\"blur\",\"spline32\"],"
              "\"backends\":["
              "{\"id\":\"cpu\",\"compiled\":true,\"device_available\":true,"
              "\"analysis_command_available\":"
           << available
           << ",\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
              "\"p_norms\":{\"minimum\":1,\"maximum\":4294967295},"
              "\"max_half_bandwidth\":29,\"max_forward_width\":30,"
              "\"compiled_isa\":";
    write_isa_set(output, cpu.compiled);
    output << ",\"available_isa\":";
    write_isa_set(output, cpu.available);
    output << ",\"selected_isa\":" << json_string(getnative::cpu_isa_name(cpu.selected))
           << ",\"math_modes\":[\"production\"],\"selected_math_mode\":\"production\","
              "\"selection_reason\":"
           << json_string(cpu.selection_reason) << '}';
#if defined(GETNATIVE_HAS_METAL)
    try {
        const getnative::MetalAnalysisEngine metal;
        const auto &device = metal.device_info();
        output << ",{\"id\":\"metal\",\"compiled\":true,\"device_available\":true,"
                  "\"analysis_command_available\":false,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"device\":"
               << json_string(device.name)
               << ",\"registry_id\":" << device.registry_id
               << ",\"unified_memory\":" << (device.unified_memory ? "true" : "false")
               << '}';
    } catch (const std::exception &error) {
        output << ",{\"id\":\"metal\",\"compiled\":true,\"device_available\":false,"
                  "\"analysis_command_available\":false,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"reason\":"
               << json_string(error.what()) << '}';
    }
#else
    output << ",{\"id\":\"metal\",\"compiled\":false,\"device_available\":false,"
              "\"analysis_command_available\":false,\"axes\":[],\"p_norms\":null,"
              "\"max_half_bandwidth\":null,\"max_forward_width\":null,"
              "\"reason\":\"not compiled\"}";
#endif
#if defined(GETNATIVE_HAS_CUDA)
    try {
        const getnative::CudaRuntimeProbe probe = getnative::cuda_runtime_probe();
        const auto selected = std::find_if(
            probe.devices.begin(), probe.devices.end(),
            [](const getnative::CudaDeviceInfo &device) {
                return device.backend_compatible;
            });
        if (!probe.device_available || selected == probe.devices.end()) {
            throw std::runtime_error(
                probe.reason.empty()
                    ? "no compatible CUDA device is available"
                    : probe.reason);
        }
        getnative::CudaAnalysisOptions options;
        options.device_ordinal = selected->ordinal;
        const getnative::CudaAnalysisEngine cuda(options);
        const auto &device = cuda.device_info();
        const std::int32_t minimum_cuda = getnative::cuda_minimum_compute_capability();
        output << ",{\"id\":\"cuda\",\"compiled\":true,\"device_available\":true,"
                  "\"analysis_command_available\":"
               << available
               << ",\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"device\":"
               << json_string(device.name)
               << ",\"uuid\":" << json_string(device.uuid)
               << ",\"compute_capability\":"
               << json_string(std::to_string(device.compute_capability_major) + "."
                              + std::to_string(device.compute_capability_minor))
               << ",\"minimum_compute_capability\":"
               << json_string(std::to_string(minimum_cuda / 10) + "."
                              + std::to_string(minimum_cuda % 10))
               << ",\"artifact_target\":"
               << json_string(getnative::cuda_compiled_artifact_target())
               << ",\"driver_version\":" << device.driver_version
               << ",\"total_memory_bytes\":" << device.total_memory_bytes << '}';
    } catch (const std::exception &error) {
        const std::int32_t minimum_cuda = getnative::cuda_minimum_compute_capability();
        output << ",{\"id\":\"cuda\",\"compiled\":true,\"device_available\":false,"
                  "\"analysis_command_available\":false,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,"
                  "\"minimum_compute_capability\":"
               << json_string(std::to_string(minimum_cuda / 10) + "."
                              + std::to_string(minimum_cuda % 10))
               << ",\"artifact_target\":"
               << json_string(getnative::cuda_compiled_artifact_target())
               << ",\"reason\":" << json_string(error.what()) << '}';
    }
#else
    output << ",{\"id\":\"cuda\",\"compiled\":false,\"device_available\":false,"
              "\"analysis_command_available\":false,\"axes\":[],\"p_norms\":null,"
              "\"max_half_bandwidth\":null,\"max_forward_width\":null,"
              "\"reason\":\"not compiled\"}";
#endif
    output << "],\"profiles\":[";
    bool first = true;
    for (const auto &value : getnative::profiles()) {
        if (!first) output << ',';
        first = false;
        output << "{\"id\":\"" << value.name << "\",\"grid_semantics\":\""
               << getnative::grid_semantics_name(value.default_grid)
               << "\",\"default_crop\":" << value.default_crop << '}';
    }
    output << "],\"runtime_dependencies\":[]}\n";
}

} // namespace getnative::cli

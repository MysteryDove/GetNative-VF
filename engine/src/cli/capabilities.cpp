#include "capabilities.hpp"

#include "getnative/cpu_features.hpp"
#include "getnative/profile.hpp"
#if defined(GETNATIVE_HAS_MEDIA)
#include "getnative/media_decode.hpp"
#endif
#if defined(GETNATIVE_HAS_METAL)
#include "getnative/metal_analysis.hpp"
#endif
#if defined(GETNATIVE_HAS_CUDA)
#include "getnative/cuda_analysis.hpp"
#endif
#if defined(GETNATIVE_HAS_VULKAN)
#include "getnative/vulkan_analysis.hpp"
#endif

#include <algorithm>
#include <optional>
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

#if !defined(__ARM_NEON) && !defined(__ARM_NEON__)
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
#endif

} // namespace

void write_capabilities_impl(
    std::ostream &output, bool analysis_available
#if defined(GETNATIVE_HAS_CUDA)
    , const getnative::CudaAnalysisEngine *resident_cuda,
    std::string_view resident_cuda_error, bool use_resident_cuda
#endif
#if defined(GETNATIVE_HAS_VULKAN)
    , const getnative::VulkanAnalysisEngine *resident_vulkan,
    std::string_view resident_vulkan_error, bool use_resident_vulkan
#endif
) {
    const char *available = analysis_available ? "true" : "false";
#if !defined(__ARM_NEON) && !defined(__ARM_NEON__)
    const getnative::CpuDispatchInfo cpu = getnative::cpu_dispatch_info();
#endif
#if defined(GETNATIVE_HAS_MEDIA) && defined(GETNATIVE_HAS_CUDA)
    const bool nvdec_available =
        getnative::media::backend_runtime_available(
            getnative::media::DecoderOptions::Backend::cuda)
        && getnative::cuda_backend_available();
#endif
#if defined(GETNATIVE_HAS_MEDIA) && defined(GETNATIVE_HAS_VULKAN)
    bool vulkan_video_available = false;
    std::vector<std::string> vulkan_video_codecs;
    std::string vulkan_video_reason;
    try {
        std::optional<getnative::VulkanAnalysisEngine> temporary_vulkan_video;
        const getnative::VulkanAnalysisEngine *vulkan_video = resident_vulkan;
        if (use_resident_vulkan) {
            if (vulkan_video == nullptr) {
                throw std::runtime_error(
                    resident_vulkan_error.empty()
                        ? "no compatible Vulkan device is available"
                        : std::string{resident_vulkan_error});
            }
        } else {
            temporary_vulkan_video.emplace();
            vulkan_video = &*temporary_vulkan_video;
        }
        const auto &device = vulkan_video->device_info();
        vulkan_video_codecs = device.video_decode_codecs;
        if (!getnative::media::backend_runtime_available(
                getnative::media::DecoderOptions::Backend::vulkan_video)) {
            vulkan_video_reason = "FFmpeg Vulkan hardware decode is unavailable";
        } else if (!device.video_decode_available) {
            vulkan_video_reason = device.video_decode_reason;
        } else {
            vulkan_video_available = true;
        }
    } catch (const std::exception &error) {
        vulkan_video_reason = error.what();
    }
#endif
#if defined(GETNATIVE_HAS_MEDIA) && defined(__APPLE__)
    const bool videotoolbox_runtime = getnative::media::backend_runtime_available(
        getnative::media::DecoderOptions::Backend::videotoolbox);
    const auto videotoolbox_codecs = getnative::media::hardware_codecs(
        getnative::media::DecoderOptions::Backend::videotoolbox);
#endif
    output << "{\"schema_version\":2,\"engine\":\"getnative-engine\",\"version\":\"0.2.1\","
              "\"commands\":{\"capabilities\":true,\"geometry\":true,\"analyze\":"
           << available;
#if defined(GETNATIVE_HAS_MEDIA)
    output << ",\"media_index_begin\":true,\"media_frame_window\":true,"
              "\"media_preview_begin\":true,\"media_asset_batch_begin\":true";
#else
    output << ",\"media_index_begin\":false,\"media_frame_window\":false,"
              "\"media_preview_begin\":false,\"media_asset_batch_begin\":false";
#endif
    output << "},\"kernels\":["
              "{\"id\":\"bilinear\",\"parameters\":{\"kind\":\"none\"}},"
              "{\"id\":\"bicubic\",\"parameters\":{\"kind\":\"bicubic_bc\",\"finite\":true}},"
              "{\"id\":\"lanczos\",\"parameters\":{\"kind\":\"integer_taps\","
              "\"gui_min\":1,\"gui_max\":8,\"core_min\":1,\"core_max\":15}},"
              "{\"id\":\"spline16\",\"parameters\":{\"kind\":\"none\"}},"
              "{\"id\":\"spline36\",\"parameters\":{\"kind\":\"none\"}},"
              "{\"id\":\"spline64\",\"parameters\":{\"kind\":\"none\"}}],"
              "\"unsupported_features\":[\"spline32\"],"
              "\"features\":{\"verify_frame_ring\":true,\"media_frame_batch\":false,"
              "\"verify_engine_decode\":";
#if defined(GETNATIVE_HAS_MEDIA)
    output << "true,\"verify_metal_zero_copy\":false"
           << ",\"media_verify_concurrency\":{\"min\":1,\"max\":8,\"default\":2}},\"decode_backends\":["
              "{\"id\":\"software\",\"compiled\":true,\"runtime_device\":true,"
              "\"codecs\":[\"*\"],\"zero_copy\":false},";
#if defined(GETNATIVE_HAS_CUDA)
    output << "{\"id\":\"nvdec\",\"compiled\":true,\"runtime_device\":"
           << (nvdec_available ? "true" : "false")
           << ","
              "\"codecs\":[\"h264\",\"hevc\",\"av1\",\"vp9\",\"mpeg2video\",\"vc1\"],"
              "\"zero_copy\":"
           << (nvdec_available ? "true" : "false")
           << "},";
#else
    output << "{\"id\":\"nvdec\",\"compiled\":false,\"runtime_device\":false,"
              "\"codecs\":[],\"zero_copy\":false,\"reason\":\"not compiled\"},";
#endif
#if defined(GETNATIVE_HAS_VULKAN)
    output << "{\"id\":\"vulkan_video\",\"compiled\":true,\"runtime_device\":"
           << (vulkan_video_available ? "true" : "false")
           << ",\"codecs\":[";
    for (std::size_t index = 0U; index < vulkan_video_codecs.size(); ++index) {
        if (index != 0U) output << ',';
        output << json_string(vulkan_video_codecs[index]);
    }
    output << "],\"zero_copy\":"
           << (vulkan_video_available ? "true" : "false");
    if (!vulkan_video_available) {
        output << ",\"reason\":" << json_string(
            vulkan_video_reason.empty() ? "unavailable" : vulkan_video_reason);
    }
    output << "},";
#else
    output << "{\"id\":\"vulkan_video\",\"compiled\":false,\"runtime_device\":false,"
              "\"codecs\":[],\"zero_copy\":false,\"reason\":\"not compiled\"},";
#endif
#if defined(__APPLE__)
    output << "{\"id\":\"videotoolbox\",\"compiled\":true,\"runtime_device\":"
           << (videotoolbox_runtime ? "true" : "false") << ",\"codecs\":[";
    for (std::size_t index = 0U; index < videotoolbox_codecs.size(); ++index) {
        if (index != 0U) output << ',';
        output << json_string(videotoolbox_codecs[index]);
    }
    output << "],\"surface_formats\":[\"420v\",\"420f\",\"x420\",\"xf20\"],\"zero_copy\":"
           << "false,\"reason\":\"Metal zero-copy path is implemented but multi-slot/oracle validation is incomplete\"}],";
#else
    output << "{\"id\":\"videotoolbox\",\"compiled\":false,\"runtime_device\":false,\"codecs\":[],\"surface_formats\":[\"420v\",\"420f\",\"x420\",\"xf20\"],\"zero_copy\":false,\"reason\":\"not compiled\"}],";
#endif
#else
    output << "false,\"verify_metal_zero_copy\":false,\"media_verify_concurrency\":{\"min\":1,\"max\":8,\"default\":2}},\"decode_backends\":["
              "{\"id\":\"software\",\"compiled\":false,\"runtime_device\":false,"
              "\"codecs\":[],\"zero_copy\":false,\"reason\":\"not compiled\"},"
              "{\"id\":\"nvdec\",\"compiled\":false,\"runtime_device\":false,"
              "\"codecs\":[],\"zero_copy\":false,\"reason\":\"not compiled\"},"
              "{\"id\":\"vulkan_video\",\"compiled\":false,\"runtime_device\":false,"
              "\"codecs\":[],\"zero_copy\":false,\"reason\":\"not compiled\"},"
              "{\"id\":\"videotoolbox\",\"compiled\":false,\"runtime_device\":false,"
              "\"codecs\":[],\"surface_formats\":[\"420v\",\"420f\",\"x420\",\"xf20\"],"
              "\"zero_copy\":false,\"reason\":\"not compiled\"}],";
#endif
    output << "\"media\":{";
#if defined(GETNATIVE_HAS_MEDIA)
    output << "\"available\":true,\"ffmpeg_abi\":"
           << json_string(getnative::media::runtime_version())
           << ",\"index_version\":" << getnative::media::MediaIndex::format_version
           << ",\"index_format\":\"lwi/vf.lwi\"},";
#else
    output << "\"available\":false,\"ffmpeg_abi\":null,"
              "\"index_version\":null,\"index_format\":\"lwi/vf.lwi\"},";
#endif
    output << "\"backends\":["
              "{\"id\":\"cpu\",\"compiled\":true,\"device_available\":true,"
              "\"analysis_command_available\":"
           << available
           << ",\"auto_priority\":100,\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
              "\"p_norms\":{\"minimum\":1,\"maximum\":4294967295},"
              "\"max_half_bandwidth\":29,\"max_forward_width\":30,"
              "\"compiled_isa\":";
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    // The x86 ISA evaluator has no tiers to report on AArch64, where NEON is
    // architectural baseline and the column/metric kernels always use it.
    // Report the truth instead of a misleading scalar-only set.
    output << "[\"scalar\",\"neon\"],\"available_isa\":[\"scalar\",\"neon\"],"
              "\"selected_isa\":\"neon\",\"math_modes\":[\"production\"],"
              "\"selected_math_mode\":\"production\",\"selection_reason\":"
              "\"AArch64 baseline NEON\"}";
#else
    write_isa_set(output, cpu.compiled);
    output << ",\"available_isa\":";
    write_isa_set(output, cpu.available);
    output << ",\"selected_isa\":" << json_string(getnative::cpu_isa_name(cpu.selected))
           << ",\"math_modes\":[\"production\"],\"selected_math_mode\":\"production\","
              "\"selection_reason\":"
           << json_string(cpu.selection_reason) << '}';
#endif
#if defined(GETNATIVE_HAS_METAL)
    try {
        const getnative::MetalAnalysisEngine metal;
        const auto &device = metal.device_info();
        output << ",{\"id\":\"metal\",\"compiled\":true,\"device_available\":true,"
                  "\"analysis_command_available\":"
               << available
               << ",\"auto_priority\":null,"
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
                  "\"auto_priority\":null,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"reason\":"
               << json_string(error.what()) << '}';
    }
#else
    output << ",{\"id\":\"metal\",\"compiled\":false,\"device_available\":false,"
              "\"analysis_command_available\":false,\"auto_priority\":null,"
              "\"axes\":[],\"p_norms\":null,"
              "\"max_half_bandwidth\":null,\"max_forward_width\":null,"
              "\"reason\":\"not compiled\"}";
#endif
#if defined(GETNATIVE_HAS_CUDA)
    try {
        std::optional<getnative::CudaAnalysisEngine> temporary_cuda;
        const getnative::CudaAnalysisEngine *cuda = resident_cuda;
        if (use_resident_cuda) {
            if (cuda == nullptr) {
                throw std::runtime_error(
                    resident_cuda_error.empty()
                        ? "no compatible CUDA device is available"
                        : std::string{resident_cuda_error});
            }
        } else {
            // CUDA ordinal 0 is authoritative after CUDA_VISIBLE_DEVICES has
            // established runtime order. Do not guess among multiple devices.
            temporary_cuda.emplace();
            cuda = &*temporary_cuda;
        }
        const auto &device = cuda->device_info();
        const std::int32_t minimum_cuda = getnative::cuda_minimum_compute_capability();
        output << ",{\"id\":\"cuda\",\"compiled\":true,\"device_available\":true,"
                  "\"analysis_command_available\":"
               << available
               << ",\"auto_priority\":10,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":"
               << getnative::cuda_minimum_p_norm << ",\"maximum\":"
               << getnative::cuda_maximum_p_norm << "},"
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
                  "\"auto_priority\":null,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":"
               << getnative::cuda_minimum_p_norm << ",\"maximum\":"
               << getnative::cuda_maximum_p_norm << "},"
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
              "\"analysis_command_available\":false,\"auto_priority\":null,"
              "\"axes\":[],\"p_norms\":null,"
              "\"max_half_bandwidth\":null,\"max_forward_width\":null,"
              "\"reason\":\"not compiled\"}";
#endif
#if defined(GETNATIVE_HAS_VULKAN)
    try {
        std::optional<getnative::VulkanAnalysisEngine> temporary_vulkan;
        const getnative::VulkanAnalysisEngine *vulkan = resident_vulkan;
        if (use_resident_vulkan) {
            if (vulkan == nullptr) {
                throw std::runtime_error(
                    resident_vulkan_error.empty()
                        ? "no compatible Vulkan device is available"
                        : std::string{resident_vulkan_error});
            }
        } else {
            temporary_vulkan.emplace();
            vulkan = &*temporary_vulkan;
        }
        const auto &device = vulkan->device_info();
        output << ",{\"id\":\"vulkan\",\"compiled\":true,\"device_available\":true,"
                  "\"analysis_command_available\":"
               << available
               << ",\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":"
               << getnative::vulkan_minimum_p_norm << ",\"maximum\":"
               << getnative::vulkan_maximum_p_norm << "},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"device\":"
               << json_string(device.name)
               << ",\"device_type\":"
               << json_string(getnative::vulkan_device_type_name(device.device_type))
               << ",\"auto_priority\":"
               << (device.device_type == getnative::VulkanDeviceType::discrete_gpu
                       ? "20" : "null")
               << ",\"uuid\":" << json_string(device.uuid)
               << ",\"api_version\":" << device.api_version
               << ",\"driver_version\":" << device.driver_version
               << ",\"total_memory_bytes\":" << device.device_local_memory_bytes
               << ",\"max_storage_buffer_bytes\":"
               << device.maximum_storage_buffer_bytes << '}';
    } catch (const std::exception &error) {
        output << ",{\"id\":\"vulkan\",\"compiled\":true,\"device_available\":false,"
                  "\"analysis_command_available\":false,"
                  "\"auto_priority\":null,"
                  "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                  "\"p_norms\":{\"minimum\":"
               << getnative::vulkan_minimum_p_norm << ",\"maximum\":"
               << getnative::vulkan_maximum_p_norm << "},"
                  "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"reason\":"
               << json_string(error.what()) << '}';
    }
#else
    output << ",{\"id\":\"vulkan\",\"compiled\":false,\"device_available\":false,"
              "\"analysis_command_available\":false,\"auto_priority\":null,"
              "\"axes\":[],\"p_norms\":null,"
              "\"max_half_bandwidth\":null,\"max_forward_width\":null,"
              "\"reason\":\"not compiled\"}";
#endif
    output << "],\"profiles\":[";
    bool first = true;
    for (const auto &value : getnative::profiles()) {
        if (!first) output << ',';
        first = false;
        const char *endpoint = value.default_endpoint == getnative::EndpointRule::inclusive
            ? "inclusive" : "exclusive_stop";
        const char *axis = value.default_axis == getnative::DefaultAxisMode::height_plus_width
            ? "h_plus_w" : "h_only";
        output << "{\"id\":\"" << value.name << "\",\"grid_semantics\":\""
               << getnative::grid_semantics_name(value.default_grid)
               << "\",\"default_grid\":{\"start\":" << json_string(value.default_start)
               << ",\"stop\":" << json_string(value.default_stop)
               << ",\"step\":" << json_string(value.default_step)
               << ",\"endpoint_rule\":" << json_string(endpoint) << "}"
               << ",\"default_axis_mode\":" << json_string(axis)
               << ",\"default_crop\":" << value.default_crop
               << ",\"default_threshold\":" << value.default_threshold
               << ",\"threshold_comparison\":\"strict_greater_than\""
               << ",\"default_kernel\":{\"id\":" << json_string(value.default_kernel)
               << ",\"b\":" << value.default_b << ",\"c\":" << value.default_c
               << ",\"taps\":" << value.default_taps << "}}";
    }
    output << "],\"runtime_dependencies\":[]}\n";
}

void write_capabilities(std::ostream &output, bool analysis_available) {
    write_capabilities_impl(
        output, analysis_available
#if defined(GETNATIVE_HAS_CUDA)
        , nullptr, {}, false
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        , nullptr, {}, false
#endif
    );
}

#if defined(GETNATIVE_HAS_CUDA) || defined(GETNATIVE_HAS_VULKAN)
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
                        ) {
    write_capabilities_impl(
        output, analysis_available
#if defined(GETNATIVE_HAS_CUDA)
        , resident_cuda, resident_cuda_error, true
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        , resident_vulkan, resident_vulkan_error, true
#endif
        );
}
#endif

} // namespace getnative::cli

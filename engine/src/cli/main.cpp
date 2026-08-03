#include "getnative/crop_geometry.hpp"
#include "getnative/cpu_features.hpp"
#include "getnative/profile.hpp"
#if defined(GETNATIVE_HAS_METAL)
#include "getnative/metal_analysis.hpp"
#endif
#if defined(GETNATIVE_HAS_CUDA)
#include "getnative/cuda_analysis.hpp"
#endif
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::map<std::string, std::string, std::less<>> parse_options(int argc, char** argv, int first) {
    std::map<std::string, std::string, std::less<>> options;
    for (int index = first; index < argc; index += 2) {
        const std::string_view key = argv[index];
        if (!key.starts_with("--") || index + 1 >= argc) {
            throw std::invalid_argument("options must be --name value pairs");
        }
        options.emplace(std::string{key.substr(2)}, argv[index + 1]);
    }
    return options;
}

double required_double(const auto& options, std::string_view key) {
    const auto iterator = options.find(key);
    if (iterator == options.end()) {
        throw std::invalid_argument("missing --" + std::string{key});
    }
    double value = 0.0;
    const auto& text = iterator->second;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid --" + std::string{key});
    }
    return value;
}

std::int64_t required_integer(const auto& options, std::string_view key) {
    const double value = required_double(options, key);
    if (std::trunc(value) != value) {
        throw std::invalid_argument("--" + std::string{key} + " must be an integer");
    }
    return getnative::python_int(value);
}

std::optional<std::int64_t> optional_integer(const auto& options, std::string_view key) {
    return options.contains(key) ? std::optional{required_integer(options, key)} : std::nullopt;
}

std::string number(double value) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

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

void print_isa_set(const getnative::CpuIsaSet &set) {
    std::cout << '[';
    bool first = true;
    auto append = [&](getnative::CpuIsa isa, bool present) {
        if (!present) return;
        if (!first) std::cout << ',';
        first = false;
        std::cout << json_string(getnative::cpu_isa_name(isa));
    };
    append(getnative::CpuIsa::scalar, set.scalar);
    append(getnative::CpuIsa::sse2, set.sse2);
    append(getnative::CpuIsa::avx2, set.avx2);
    append(getnative::CpuIsa::avx512, set.avx512);
    std::cout << ']';
}

void print_capabilities() {
    const getnative::CpuDispatchInfo cpu = getnative::cpu_dispatch_info();
    std::cout << "{\"schema_version\":2,\"engine\":\"getnative-engine\",\"version\":\"0.1.0\","
                 "\"commands\":{\"capabilities\":true,\"geometry\":true,\"analyze\":false},"
                 "\"kernels\":["
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
                 "\"analysis_command_available\":false,"
                 "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                 "\"p_norms\":{\"minimum\":1,\"maximum\":4294967295},"
                 "\"max_half_bandwidth\":29,\"max_forward_width\":30,"
                 "\"compiled_isa\":";
    print_isa_set(cpu.compiled);
    std::cout << ",\"available_isa\":";
    print_isa_set(cpu.available);
    std::cout << ",\"selected_isa\":" << json_string(getnative::cpu_isa_name(cpu.selected))
              << ",\"math_modes\":[\"production\"],\"selected_math_mode\":\"production\","
                 "\"selection_reason\":" << json_string(cpu.selection_reason) << '}';
#if defined(GETNATIVE_HAS_METAL)
    try {
        const getnative::MetalAnalysisEngine metal;
        const auto& device = metal.device_info();
        std::cout << ",{\"id\":\"metal\",\"compiled\":true,\"device_available\":true,"
                     "\"analysis_command_available\":false,"
                     "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                     "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                     "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"device\":"
                  << json_string(device.name)
                  << ",\"registry_id\":" << device.registry_id
                  << ",\"unified_memory\":" << (device.unified_memory ? "true" : "false")
                  << '}';
    } catch (const std::exception& error) {
        std::cout << ",{\"id\":\"metal\",\"compiled\":true,\"device_available\":false,"
                     "\"analysis_command_available\":false,"
                     "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                     "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                     "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"reason\":"
                  << json_string(error.what()) << '}';
    }
#else
    std::cout << ",{\"id\":\"metal\",\"compiled\":false,\"device_available\":false,"
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
            throw std::runtime_error(probe.reason.empty()
                ? "no compatible CUDA device is available" : probe.reason);
        }
        getnative::CudaAnalysisOptions options;
        options.device_ordinal = selected->ordinal;
        const getnative::CudaAnalysisEngine cuda(options);
        const auto &device = cuda.device_info();
        const std::int32_t minimum_cuda =
            getnative::cuda_minimum_compute_capability();
        std::cout << ",{\"id\":\"cuda\",\"compiled\":true,\"device_available\":true,"
                     "\"analysis_command_available\":false,"
                     "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                     "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                     "\"max_half_bandwidth\":15,\"max_forward_width\":16,\"device\":"
                  << json_string(device.name)
                  << ",\"uuid\":" << json_string(device.uuid)
                  << ",\"compute_capability\":"
                  << json_string(std::to_string(device.compute_capability_major) + "."
                                 + std::to_string(device.compute_capability_minor))
                  << ",\"minimum_compute_capability\":"
                  << json_string(
                         std::to_string(minimum_cuda / 10) + "."
                         + std::to_string(minimum_cuda % 10))
                  << ",\"artifact_target\":"
                  << json_string(getnative::cuda_compiled_artifact_target())
                  << ",\"driver_version\":" << device.driver_version
                  << ",\"total_memory_bytes\":" << device.total_memory_bytes << '}';
    } catch (const std::exception &error) {
        const std::int32_t minimum_cuda =
            getnative::cuda_minimum_compute_capability();
        std::cout << ",{\"id\":\"cuda\",\"compiled\":true,\"device_available\":false,"
                     "\"analysis_command_available\":false,"
                     "\"axes\":[\"horizontal\",\"vertical\",\"both\"],"
                     "\"p_norms\":{\"minimum\":1,\"maximum\":1},"
                     "\"max_half_bandwidth\":15,\"max_forward_width\":16,"
                     "\"minimum_compute_capability\":"
                  << json_string(
                         std::to_string(minimum_cuda / 10) + "."
                         + std::to_string(minimum_cuda % 10))
                  << ",\"artifact_target\":"
                  << json_string(getnative::cuda_compiled_artifact_target())
                  << ",\"reason\":"
                  << json_string(error.what()) << '}';
    }
#else
    std::cout << ",{\"id\":\"cuda\",\"compiled\":false,\"device_available\":false,"
                 "\"analysis_command_available\":false,\"axes\":[],\"p_norms\":null,"
                 "\"max_half_bandwidth\":null,\"max_forward_width\":null,"
                 "\"reason\":\"not compiled\"}";
#endif
    std::cout << "],\"profiles\":[";
    bool first = true;
    for (const auto& value : getnative::profiles()) {
        if (!first) {
            std::cout << ',';
        }
        first = false;
        std::cout << "{\"id\":\"" << value.name << "\",\"grid_semantics\":\""
                  << getnative::grid_semantics_name(value.default_grid)
                  << "\",\"default_crop\":" << value.default_crop << '}';
    }
    std::cout << "],\"runtime_dependencies\":[]}\n";
}

void print_geometry(int argc, char** argv) {
    const auto options = parse_options(argc, argv, 2);
    const auto profile_name = options.contains("profile")
        ? std::string_view{options.at("profile")}
        : std::string_view{"muf-d278cd3"};
    if (!getnative::parse_profile(profile_name)) {
        throw std::invalid_argument("unknown profile: " + std::string{profile_name});
    }

    const double active_width = required_double(options, "active-width");
    const double active_height = required_double(options, "active-height");
    const std::string_view mode = options.contains("mode")
        ? std::string_view{options.at("mode")}
        : std::string_view{"standard"};
    if (mode != "standard" && mode != "pro") {
        throw std::invalid_argument("unknown --mode: " + std::string{mode});
    }
    const bool pro = mode == "pro";
    getnative::Geometry geometry{};
    if (pro) {
        geometry = getnative::descale_geometry_pro(
            active_width,
            active_height,
            optional_integer(options, "base-height"),
            optional_integer(options, "base-width"));
    } else {
        geometry = getnative::descale_geometry(
            required_integer(options, "source-width"),
            required_integer(options, "source-height"),
            active_width,
            active_height,
            optional_integer(options, "base-height"));
    }

    std::cout << "{\"schema_version\":1,\"profile\":\"" << profile_name
              << "\",\"mode\":\"" << (pro ? "pro" : "standard")
              << "\",\"geometry\":{\"width\":" << geometry.width
              << ",\"height\":" << geometry.height
              << ",\"src_left\":" << number(geometry.src_left)
              << ",\"src_top\":" << number(geometry.src_top)
              << ",\"src_width\":" << number(geometry.src_width)
              << ",\"src_height\":" << number(geometry.src_height) << "}}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            throw std::invalid_argument("usage: getnative-engine <capabilities|geometry> [options]");
        }
        const std::string_view command = argv[1];
        if (command == "capabilities") {
            print_capabilities();
            return EXIT_SUCCESS;
        }
        if (command == "geometry") {
            print_geometry(argc, argv);
            return EXIT_SUCCESS;
        }
        throw std::invalid_argument("unknown command: " + std::string{command});
    } catch (const std::exception& error) {
        std::cerr << "getnative-engine: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "getnative/metal_analysis.hpp"

#include "getnative/filter.hpp"
#include "inverse_columns.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/stdio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef GETNATIVE_METALLIB_PATH
#error "GETNATIVE_METALLIB_PATH must name the generated benchmark metallib"
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t absolute_working_set_limit = 2ULL * 1024ULL * 1024ULL * 1024ULL;

struct Configuration {
    std::filesystem::path matrix_path;
    std::filesystem::path fixture_path;
    std::optional<std::filesystem::path> json_path;
    std::optional<std::filesystem::path> artifact_root;
    std::optional<std::string> selected_case;
    std::size_t samples = 21;
    bool controls_only = true;
    bool sustained = false;
    bool required_specialized = false;
    bool cpu_columns = false;
    bool required_simd = false;
    bool assert_gates = false;
};

struct FilterSpec {
    std::string id;
    std::string type;
    double b = 0.0;
    double c = 0.5;
    std::int32_t taps = 0;
    bool control = false;
};

struct FractionalScan {
    std::string id;
    std::int32_t native_start = 0;
    std::int32_t native_end = 0;
    double active_start = 0.0;
    double active_end = 0.0;
};

struct Matrix {
    std::int32_t source_width = 0;
    std::int32_t source_height = 0;
    std::size_t candidates = 0;
    std::size_t tile_size = 0;
    std::size_t reduction_groups = 0;
    std::size_t inverse_threads = 0;
    std::int32_t primary_native_height = 0;
    std::string cpu_primary_filter_id;
    std::vector<std::int32_t> native_heights;
    getnative::MetricSpec metric;
    std::vector<FilterSpec> filters;
    FractionalScan fractional_scan;
    std::vector<std::string> sustained_filter_ids;
};

struct BenchmarkCase {
    std::string id;
    FilterSpec filter;
    std::int32_t native_height = 0;
    bool fractional_scan = false;
};

struct DecodedImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<float> luma;
    std::filesystem::path checksum_path;
    std::string expected_fixture_sha256;
    std::string fixture_sha256;
    std::string luma_sha256;
};

struct IdentitySnapshot {
    std::filesystem::path binary_path;
    std::filesystem::path metallib_path;
    std::uintmax_t binary_bytes = 0;
    std::uintmax_t metallib_bytes = 0;
    std::string binary_sha256;
    std::string metallib_sha256;
    std::string matrix_sha256;
    std::string fixture_sha256;
    std::string fixture_checksum_sha256;
};

struct Measurement {
    double wall_ms = 0.0;
    getnative::MetalRuntimeTelemetry telemetry;
    std::vector<getnative::CandidateResult> results;
};

struct HostObservation {
    std::string thermal_state;
    std::array<double, 3> load_average{};
    std::vector<std::string> concurrent_processes;
};

struct PairSample {
    std::string first_mode;
    Measurement generic;
    Measurement comparison;
    double delta = 0.0;
    HostObservation host_before;
    HostObservation host_after;
};

struct Accuracy {
    double maximum_error = 0.0;
    std::size_t reference_minimum_index = 0;
    std::size_t actual_minimum_index = 0;
    std::size_t valley_distance = 0;
    bool within_tolerance = true;
};

struct CaseReport {
    BenchmarkCase benchmark_case;
    std::string shape;
    double cpu_oracle_ms = 0.0;
    std::vector<getnative::CandidateResult> cpu_results;
    std::vector<PairSample> samples;
    double generic_median_ms = 0.0;
    double comparison_median_ms = 0.0;
    double generic_p95_ms = 0.0;
    double comparison_p95_ms = 0.0;
    double paired_delta_median = 0.0;
    double paired_delta_mad = 0.0;
    double first_decile_delta_median = 0.0;
    double last_decile_delta_median = 0.0;
    Accuracy generic_accuracy;
    Accuracy comparison_accuracy;
    double maximum_path_difference = 0.0;
    std::size_t generic_peak_working_set = 0;
    std::size_t comparison_peak_working_set = 0;
    getnative::MetalRuntimeTelemetry generic_creation;
    getnative::MetalRuntimeTelemetry comparison_creation;
    HostObservation host_before;
    HostObservation host_after;
    bool thermal_stable = true;
    bool concurrent_process_detected = false;
    bool gate_passed = false;
};

struct CpuMeasurement {
    double inverse_ms = 0.0;
    double candidate_ms = 0.0;
    double error = 0.0;
};

struct CpuPairSample {
    std::string first_mode;
    CpuMeasurement scalar;
    CpuMeasurement comparison;
    double inverse_delta = 0.0;
    double candidate_delta = 0.0;
    HostObservation host_before;
    HostObservation host_after;
};

struct CpuCaseReport {
    BenchmarkCase benchmark_case;
    std::string shape;
    std::size_t measured_candidate_index = 0;
    std::int32_t measured_native_height = 0;
    double measured_active_height = 0.0;
    std::string comparison_isa;
    std::vector<CpuPairSample> samples;
    double scalar_inverse_median_ms = 0.0;
    double comparison_inverse_median_ms = 0.0;
    double scalar_candidate_median_ms = 0.0;
    double comparison_candidate_median_ms = 0.0;
    double inverse_delta_median = 0.0;
    double inverse_delta_mad = 0.0;
    double candidate_delta_median = 0.0;
    double candidate_delta_mad = 0.0;
    bool inverse_bit_identical = true;
    bool workspace_bit_identical = true;
    bool metric_bit_identical = true;
    std::string scalar_inverse_sha256;
    std::string comparison_inverse_sha256;
    std::string scalar_workspace_sha256;
    std::string comparison_workspace_sha256;
    HostObservation host_before;
    HostObservation host_after;
    bool thermal_stable = true;
    bool concurrent_process_detected = false;
    bool gate_passed = false;
};

[[nodiscard]] std::size_t parse_size(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw std::invalid_argument("numeric option must be a positive integer");
    }
    return value;
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--matrix" && index + 1 < argc) {
            result.matrix_path = argv[++index];
        } else if (argument == "--fixture" && index + 1 < argc) {
            result.fixture_path = argv[++index];
        } else if (argument == "--json-out" && index + 1 < argc) {
            result.json_path = std::filesystem::path{argv[++index]};
        } else if (argument == "--artifact-root" && index + 1 < argc) {
            result.artifact_root = std::filesystem::path{argv[++index]};
        } else if (argument == "--samples" && index + 1 < argc) {
            result.samples = parse_size(argv[++index]);
        } else if (argument == "--case" && index + 1 < argc) {
            result.selected_case = argv[++index];
        } else if (argument == "--full-matrix") {
            result.controls_only = false;
        } else if (argument == "--controls-only") {
            result.controls_only = true;
        } else if (argument == "--sustained") {
            result.sustained = true;
            result.controls_only = false;
        } else if (argument == "--required-specialized") {
            result.required_specialized = true;
        } else if (argument == "--cpu-columns") {
            result.cpu_columns = true;
        } else if (argument == "--required-simd") {
            result.required_simd = true;
        } else if (argument == "--assert") {
            result.assert_gates = true;
        } else {
            throw std::invalid_argument(
                "usage: getnative_metal_kernel_benchmark --matrix PATH --fixture PATH "
                "(--json-out PATH|--artifact-root PATH) [--samples N] "
                "[--controls-only|--full-matrix|--sustained] [--case ID] "
                "[--required-specialized] [--cpu-columns [--required-simd]] [--assert]");
        }
    }
    if (result.matrix_path.empty() || result.fixture_path.empty()) {
        throw std::invalid_argument("--matrix and --fixture are required");
    }
    if (result.json_path.has_value() == result.artifact_root.has_value()) {
        throw std::invalid_argument("choose exactly one of --json-out or --artifact-root");
    }
    if (result.assert_gates && result.samples < 21) {
        throw std::invalid_argument("--assert requires at least 21 paired samples");
    }
    if (result.required_simd && !result.cpu_columns) {
        throw std::invalid_argument("--required-simd requires --cpu-columns");
    }
    if (result.cpu_columns && result.required_specialized) {
        throw std::invalid_argument(
            "--required-specialized is incompatible with --cpu-columns");
    }
    if (result.cpu_columns && result.sustained) {
        throw std::invalid_argument("--sustained is not a CPU column benchmark mode");
    }
    return result;
}

[[nodiscard]] std::string from_ns_string(NSString *value) {
    const char *text = value.UTF8String;
    if (text == nullptr) throw std::runtime_error("Foundation string is not UTF-8");
    return text;
}

[[nodiscard]] NSString *path_string(const std::filesystem::path &path) {
    NSString *result = [NSString stringWithUTF8String:path.string().c_str()];
    if (result == nil) throw std::invalid_argument("path is not valid UTF-8");
    return result;
}

[[nodiscard]] NSDictionary *require_dictionary(id value, std::string_view name) {
    if (![value isKindOfClass:[NSDictionary class]]) {
        throw std::invalid_argument(std::string{name} + " must be an object");
    }
    return static_cast<NSDictionary *>(value);
}

[[nodiscard]] NSArray *require_array(id value, std::string_view name) {
    if (![value isKindOfClass:[NSArray class]]) {
        throw std::invalid_argument(std::string{name} + " must be an array");
    }
    return static_cast<NSArray *>(value);
}

[[nodiscard]] NSNumber *require_number(id value, std::string_view name) {
    if (![value isKindOfClass:[NSNumber class]]) {
        throw std::invalid_argument(std::string{name} + " must be a number");
    }
    return static_cast<NSNumber *>(value);
}

[[nodiscard]] NSString *require_string(id value, std::string_view name) {
    if (![value isKindOfClass:[NSString class]]) {
        throw std::invalid_argument(std::string{name} + " must be a string");
    }
    return static_cast<NSString *>(value);
}

[[nodiscard]] std::int32_t checked_i32(NSNumber *number, std::string_view name) {
    const long long value = number.longLongValue;
    if (value < std::numeric_limits<std::int32_t>::min()
        || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::out_of_range(std::string{name} + " exceeds int32");
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] std::size_t checked_size(NSNumber *number, std::string_view name) {
    const long long value = number.longLongValue;
    if (value <= 0) throw std::invalid_argument(std::string{name} + " must be positive");
    return static_cast<std::size_t>(value);
}

[[nodiscard]] Matrix load_matrix(const std::filesystem::path &path) {
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfFile:path_string(path) options:0 error:&error];
    if (data == nil) {
        throw std::runtime_error("could not read matrix JSON: " + from_ns_string(error.localizedDescription));
    }
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (object == nil) {
        throw std::runtime_error("could not parse matrix JSON: " + from_ns_string(error.localizedDescription));
    }
    NSDictionary *root = require_dictionary(object, "matrix root");
    if (require_number(root[@"schema_version"], "schema_version").integerValue != 1) {
        throw std::invalid_argument("unsupported matrix schema_version");
    }
    NSDictionary *source = require_dictionary(root[@"source"], "source");
    NSDictionary *metric = require_dictionary(root[@"metric"], "metric");
    Matrix result;
    result.source_width = checked_i32(require_number(source[@"width"], "source.width"), "source.width");
    result.source_height = checked_i32(require_number(source[@"height"], "source.height"), "source.height");
    result.candidates = checked_size(require_number(root[@"candidates"], "candidates"), "candidates");
    result.tile_size = checked_size(require_number(root[@"tile_size"], "tile_size"), "tile_size");
    result.reduction_groups = checked_size(
        require_number(root[@"reduction_groups"], "reduction_groups"), "reduction_groups");
    result.inverse_threads = checked_size(
        require_number(root[@"inverse_threads"], "inverse_threads"), "inverse_threads");
    result.primary_native_height = checked_i32(
        require_number(root[@"primary_native_height"], "primary_native_height"),
        "primary_native_height");
    result.cpu_primary_filter_id = from_ns_string(
        require_string(root[@"cpu_primary_filter_id"], "cpu_primary_filter_id"));
    result.metric = {
        checked_i32(require_number(metric[@"crop_left"], "metric.crop_left"), "metric.crop_left"),
        checked_i32(require_number(metric[@"crop_right"], "metric.crop_right"), "metric.crop_right"),
        checked_i32(require_number(metric[@"crop_top"], "metric.crop_top"), "metric.crop_top"),
        checked_i32(require_number(metric[@"crop_bottom"], "metric.crop_bottom"), "metric.crop_bottom"),
        require_number(metric[@"threshold"], "metric.threshold").floatValue,
        1U,
    };
    for (id value in require_array(root[@"native_heights"], "native_heights")) {
        result.native_heights.push_back(checked_i32(
            require_number(value, "native height"), "native height"));
    }
    for (id value in require_array(root[@"filters"], "filters")) {
        NSDictionary *entry = require_dictionary(value, "filter");
        FilterSpec filter;
        filter.id = from_ns_string(require_string(entry[@"id"], "filter.id"));
        filter.type = from_ns_string(require_string(entry[@"type"], "filter.type"));
        if (entry[@"b"] != nil) filter.b = require_number(entry[@"b"], "filter.b").doubleValue;
        if (entry[@"c"] != nil) filter.c = require_number(entry[@"c"], "filter.c").doubleValue;
        if (entry[@"taps"] != nil) {
            filter.taps = checked_i32(require_number(entry[@"taps"], "filter.taps"), "filter.taps");
        }
        if (entry[@"control"] != nil) {
            filter.control = require_number(entry[@"control"], "filter.control").boolValue;
        }
        result.filters.push_back(std::move(filter));
    }
    NSDictionary *scan = require_dictionary(root[@"fractional_scan"], "fractional_scan");
    result.fractional_scan = {
        from_ns_string(require_string(scan[@"id"], "fractional_scan.id")),
        checked_i32(require_number(scan[@"native_start"], "fractional_scan.native_start"),
                    "fractional_scan.native_start"),
        checked_i32(require_number(scan[@"native_end"], "fractional_scan.native_end"),
                    "fractional_scan.native_end"),
        require_number(scan[@"active_start"], "fractional_scan.active_start").doubleValue,
        require_number(scan[@"active_end"], "fractional_scan.active_end").doubleValue,
    };
    for (id value in require_array(root[@"sustained_filter_ids"], "sustained_filter_ids")) {
        result.sustained_filter_ids.push_back(
            from_ns_string(require_string(value, "sustained filter id")));
    }
    if (result.native_heights.empty() || result.filters.empty()) {
        throw std::invalid_argument("matrix must contain native heights and filters");
    }
    if (std::none_of(result.filters.begin(), result.filters.end(), [&](const FilterSpec &filter) {
            return filter.id == result.cpu_primary_filter_id;
        })) {
        throw std::invalid_argument("cpu_primary_filter_id is not present in filters");
    }
    return result;
}

[[nodiscard]] getnative::Filter make_filter(const FilterSpec &spec) {
    if (spec.type == "bilinear") return getnative::Filter::bilinear();
    if (spec.type == "bicubic") return getnative::Filter::bicubic(spec.b, spec.c);
    if (spec.type == "spline16") return getnative::Filter::spline16();
    if (spec.type == "spline36") return getnative::Filter::spline36();
    if (spec.type == "spline64") return getnative::Filter::spline64();
    if (spec.type == "lanczos" && spec.taps >= 1 && spec.taps <= 8) {
        return getnative::Filter::lanczos(spec.taps);
    }
    throw std::invalid_argument("unsupported matrix filter " + spec.id);
}

[[nodiscard]] std::string sha256_bytes(const void *data, std::size_t size) {
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    const auto *bytes = static_cast<const unsigned char *>(data);
    while (size > 0) {
        const std::size_t chunk = std::min<std::size_t>(
            size, static_cast<std::size_t>(std::numeric_limits<CC_LONG>::max()));
        CC_SHA256_Update(&context, bytes, static_cast<CC_LONG>(chunk));
        bytes += chunk;
        size -= chunk;
    }
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &context);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char value : digest) output << std::setw(2) << static_cast<int>(value);
    return output.str();
}

[[nodiscard]] std::string sha256_file(const std::filesystem::path &path) {
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfFile:path_string(path) options:NSDataReadingMappedIfSafe
                                           error:&error];
    if (data == nil) {
        throw std::runtime_error("could not hash file: " + from_ns_string(error.localizedDescription));
    }
    return sha256_bytes(data.bytes, data.length);
}

[[nodiscard]] std::string expected_fixture_sha256(const std::filesystem::path &fixture_path,
                                                  std::filesystem::path &checksum_path) {
    checksum_path = fixture_path;
    checksum_path += ".sha256";
    std::ifstream input(checksum_path);
    std::string expected;
    if (!(input >> expected)) {
        throw std::runtime_error("could not read fixture checksum " + checksum_path.string());
    }
    const auto is_hex_digit = [](const char value) {
        return (value >= '0' && value <= '9')
            || (value >= 'a' && value <= 'f')
            || (value >= 'A' && value <= 'F');
    };
    if (expected.size() != 64U
        || !std::all_of(expected.begin(), expected.end(), is_hex_digit)) {
        throw std::runtime_error("fixture checksum must begin with one SHA-256 digest");
    }
    std::transform(expected.begin(), expected.end(), expected.begin(), [](const char value) {
        return value >= 'A' && value <= 'F' ? static_cast<char>(value - 'A' + 'a') : value;
    });
    return expected;
}

[[nodiscard]] DecodedImage decode_luma(const std::filesystem::path &path) {
    std::filesystem::path checksum_path;
    const std::string expected_hash = expected_fixture_sha256(path, checksum_path);
    const std::string fixture_hash = sha256_file(path);
    if (fixture_hash != expected_hash) {
        throw std::runtime_error("fixture SHA-256 does not match " + checksum_path.string());
    }
    NSURL *url = [NSURL fileURLWithPath:path_string(path)];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (source == nullptr) throw std::runtime_error("ImageIO could not open the fixture");
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (image == nullptr) throw std::runtime_error("ImageIO could not decode the fixture");
    const std::size_t width = CGImageGetWidth(image);
    const std::size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0
        || width > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())
        || height > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())
        || width > std::numeric_limits<std::size_t>::max() / height
        || width * height > std::numeric_limits<std::size_t>::max() / 4U) {
        CFRelease(image);
        throw std::length_error("decoded fixture dimensions are unsupported");
    }
    std::vector<std::uint8_t> rgba(width * height * 4U);
    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        rgba.data(), width, height, 8, width * 4U, color_space,
        static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast)
            | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big)));
    CGColorSpaceRelease(color_space);
    if (context == nullptr) {
        CFRelease(image);
        throw std::runtime_error("CoreGraphics could not allocate the decode surface");
    }
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(height));
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(context, CGRectMake(0.0, 0.0, static_cast<CGFloat>(width),
                                           static_cast<CGFloat>(height)), image);
    CGContextRelease(context);
    CFRelease(image);

    DecodedImage result;
    result.width = static_cast<std::int32_t>(width);
    result.height = static_cast<std::int32_t>(height);
    result.checksum_path = std::move(checksum_path);
    result.expected_fixture_sha256 = expected_hash;
    result.fixture_sha256 = fixture_hash;
    result.luma.resize(width * height);
    for (std::size_t index = 0; index < result.luma.size(); ++index) {
        const float red = static_cast<float>(rgba[index * 4U]);
        const float green = static_cast<float>(rgba[index * 4U + 1U]);
        const float blue = static_cast<float>(rgba[index * 4U + 2U]);
        result.luma[index] = (0.2126F * red + 0.7152F * green + 0.0722F * blue) / 255.0F;
    }
    result.luma_sha256 = sha256_bytes(
        result.luma.data(), result.luma.size() * sizeof(float));
    return result;
}

[[nodiscard]] std::string thermal_state() {
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal: return "nominal";
    case NSProcessInfoThermalStateFair: return "fair";
    case NSProcessInfoThermalStateSerious: return "serious";
    case NSProcessInfoThermalStateCritical: return "critical";
    }
    return "unknown";
}

[[nodiscard]] bool is_benchmark_process(std::string_view process_name) {
    std::string lowercase{process_name};
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(),
                   [](const unsigned char value) {
                       if (value >= 'A' && value <= 'Z') {
                           return static_cast<char>(value - 'A' + 'a');
                       }
                       return static_cast<char>(value);
                   });
    return (lowercase.starts_with("getnative_")
            && lowercase.find("benchmark") != std::string::npos)
        || lowercase == "xctrace"
        || lowercase.find("instruments") != std::string::npos;
}

[[nodiscard]] std::vector<std::string> concurrent_benchmark_processes() {
    std::vector<pid_t> process_ids(4096);
    int listed_bytes = 0;
    while (true) {
        const std::size_t byte_capacity = process_ids.size() * sizeof(pid_t);
        if (byte_capacity > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::length_error("process list exceeds libproc capacity");
        }
        listed_bytes = proc_listpids(PROC_ALL_PIDS, 0, process_ids.data(),
                                    static_cast<int>(byte_capacity));
        if (listed_bytes <= 0) {
            throw std::runtime_error("could not enumerate processes for benchmark isolation");
        }
        if (static_cast<std::size_t>(listed_bytes) < byte_capacity) break;
        process_ids.resize(process_ids.size() * 2U);
    }
    process_ids.resize(static_cast<std::size_t>(listed_bytes) / sizeof(pid_t));

    std::vector<std::string> result;
    const pid_t self = getpid();
    for (const pid_t process_id : process_ids) {
        if (process_id <= 0 || process_id == self) continue;

        std::array<char, PROC_PIDPATHINFO_MAXSIZE> path_buffer{};
        const int path_length = proc_pidpath(
            process_id, path_buffer.data(), static_cast<std::uint32_t>(path_buffer.size()));
        std::array<char, PROC_PIDPATHINFO_MAXSIZE> name_buffer{};
        const int name_length = proc_name(
            process_id, name_buffer.data(), static_cast<std::uint32_t>(name_buffer.size()));

        std::string process_path;
        std::string process_name;
        if (path_length > 0) {
            process_path = path_buffer.data();
            process_name = std::filesystem::path{process_path}.filename().string();
        }
        if (process_name.empty() && name_length > 0) process_name = name_buffer.data();
        if (!is_benchmark_process(process_name)) continue;

        std::string description = std::to_string(static_cast<long long>(process_id))
            + ":" + process_name;
        if (!process_path.empty()) description += ":" + process_path;
        result.push_back(std::move(description));
    }
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] HostObservation capture_host_observation() {
    HostObservation result;
    result.thermal_state = thermal_state();
    if (getloadavg(result.load_average.data(),
                   static_cast<int>(result.load_average.size()))
        != static_cast<int>(result.load_average.size())) {
        throw std::runtime_error("could not read host load average");
    }
    result.concurrent_processes = concurrent_benchmark_processes();
    return result;
}

[[nodiscard]] double median(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("median requires samples");
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if (values.size() % 2U != 0U) return values[middle];
    return (values[middle - 1U] + values[middle]) / 2.0;
}

[[nodiscard]] double percentile95(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("percentile requires samples");
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(values.size()))) - 1U;
    return values[std::min(index, values.size() - 1U)];
}

[[nodiscard]] double median_absolute_deviation(const std::vector<double> &values) {
    const double center = median(values);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values) deviations.push_back(std::abs(value - center));
    return median(std::move(deviations));
}

[[nodiscard]] const FilterSpec &find_filter(const Matrix &matrix, std::string_view id) {
    const auto found = std::find_if(matrix.filters.begin(), matrix.filters.end(),
                                    [&](const FilterSpec &value) { return value.id == id; });
    if (found == matrix.filters.end()) throw std::invalid_argument("unknown filter id " + std::string{id});
    return *found;
}

[[nodiscard]] std::vector<BenchmarkCase> select_cases(const Matrix &matrix,
                                                       const Configuration &config) {
    std::vector<BenchmarkCase> result;
    if (config.sustained) {
        std::size_t sequence = 0;
        for (const std::string &id : matrix.sustained_filter_ids) {
            const FilterSpec &filter = find_filter(matrix, id);
            result.push_back({"sustained-" + std::to_string(sequence++) + "-" + id,
                              filter, matrix.primary_native_height, false});
        }
    } else if (config.controls_only) {
        for (const FilterSpec &filter : matrix.filters) {
            if (filter.control) {
                result.push_back({filter.id + "@" + std::to_string(matrix.primary_native_height),
                                  filter, matrix.primary_native_height, false});
            }
        }
    } else {
        for (const FilterSpec &filter : matrix.filters) {
            for (const std::int32_t height : matrix.native_heights) {
                result.push_back({filter.id + "@" + std::to_string(height), filter, height, false});
            }
            result.push_back({filter.id + "@" + matrix.fractional_scan.id,
                              filter, matrix.fractional_scan.native_start, true});
        }
    }
    if (config.selected_case) {
        std::erase_if(result, [&](const BenchmarkCase &value) {
            return value.id != *config.selected_case;
        });
    }
    if (result.empty()) throw std::invalid_argument("case selection is empty");
    return result;
}

struct CandidatePoint {
    std::int32_t native_height = 0;
    double active_height = 0.0;
};

[[nodiscard]] CandidatePoint candidate_point(const Matrix &matrix,
                                             const BenchmarkCase &benchmark_case,
                                             std::size_t index) {
    if (index >= matrix.candidates) {
        throw std::out_of_range("candidate index exceeds matrix candidate count");
    }
    CandidatePoint result;
    result.native_height = benchmark_case.native_height;
    result.active_height = static_cast<double>(result.native_height)
        + static_cast<double>(index + 1U) / static_cast<double>(matrix.candidates + 1U);
    if (benchmark_case.fractional_scan) {
        const double position = matrix.candidates == 1
            ? 0.0 : static_cast<double>(index) / static_cast<double>(matrix.candidates - 1U);
        result.active_height = matrix.fractional_scan.active_start
            + position * (matrix.fractional_scan.active_end
                          - matrix.fractional_scan.active_start);
        result.native_height = static_cast<std::int32_t>(std::floor(result.active_height));
        result.native_height = std::clamp(
            result.native_height, matrix.fractional_scan.native_start,
            matrix.fractional_scan.native_end);
    }
    return result;
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> make_candidates(
    const Matrix &matrix, const BenchmarkCase &benchmark_case, getnative::AxisPlanCache &cache) {
    std::vector<getnative::CandidateAnalysis> result;
    result.reserve(matrix.candidates);
    const getnative::Filter filter = make_filter(benchmark_case.filter);
    for (std::size_t index = 0; index < matrix.candidates; ++index) {
        const CandidatePoint point = candidate_point(matrix, benchmark_case, index);
        result.push_back({
            std::to_string(index), nullptr,
            cache.get_or_build({matrix.source_height, point.native_height, point.active_height, 0.0,
                                filter, getnative::BorderMode::mirror}),
            getnative::AnalysisAxes::vertical,
        });
    }
    return result;
}

[[nodiscard]] Measurement measure(getnative::MetalAnalysisEngine &engine,
                                  getnative::ConstImageView source,
                                  std::span<const getnative::CandidateAnalysis> candidates,
                                  const getnative::MetricSpec &metric) {
    engine.reset_analysis_telemetry();
    const auto start = Clock::now();
    auto results = engine.analyze_axis_batch_f32(source, candidates, metric);
    const auto elapsed = Clock::now() - start;
    return {
        std::chrono::duration<double, std::milli>(elapsed).count(),
        engine.runtime_telemetry(),
        std::move(results),
    };
}

[[nodiscard]] Accuracy accuracy_against(
    std::span<const getnative::CandidateResult> oracle,
    std::span<const getnative::CandidateResult> actual) {
    if (oracle.size() != actual.size()) throw std::logic_error("result size mismatch");
    Accuracy result;
    for (std::size_t index = 0; index < oracle.size(); ++index) {
        if (oracle[index].id != actual[index].id) throw std::logic_error("result id mismatch");
        const double error = std::abs(oracle[index].error - actual[index].error);
        result.maximum_error = std::max(result.maximum_error, error);
        const double tolerance = std::max(1e-7, 5e-4 * std::abs(oracle[index].error));
        result.within_tolerance = result.within_tolerance && error <= tolerance;
    }
    const auto oracle_minimum = static_cast<std::size_t>(
        std::min_element(oracle.begin(), oracle.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.error < rhs.error;
        }) - oracle.begin());
    const auto actual_minimum = static_cast<std::size_t>(
        std::min_element(actual.begin(), actual.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.error < rhs.error;
        }) - actual.begin());
    result.reference_minimum_index = oracle_minimum;
    result.actual_minimum_index = actual_minimum;
    result.valley_distance = oracle_minimum > actual_minimum
        ? oracle_minimum - actual_minimum : actual_minimum - oracle_minimum;
    return result;
}

[[nodiscard]] double maximum_difference(
    std::span<const getnative::CandidateResult> first,
    std::span<const getnative::CandidateResult> second) {
    if (first.size() != second.size()) throw std::logic_error("comparison size mismatch");
    double result = 0.0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        result = std::max(result, std::abs(first[index].error - second[index].error));
    }
    return result;
}

[[nodiscard]] std::string shape_name(const getnative::AxisPlan &plan) {
    return "B" + std::to_string(2 * plan.half_bandwidth + 1)
        + "/F" + std::to_string(plan.forward_width);
}

[[nodiscard]] CaseReport run_case(const Matrix &matrix, const BenchmarkCase &benchmark_case,
                                  const DecodedImage &image, const Configuration &config) {
    HostObservation case_host_before = capture_host_observation();
    getnative::AxisPlanCache cache;
    const auto candidates = make_candidates(matrix, benchmark_case, cache);
    const getnative::ConstImageView source{
        image.luma.data(), image.width, image.height, image.width,
    };
    const auto cpu_start = Clock::now();
    auto cpu = getnative::analyze_batch_f32(source, candidates, matrix.metric);
    const double cpu_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - cpu_start).count();

    getnative::MetalAnalysisOptions generic_options{
        matrix.tile_size, matrix.reduction_groups, 0, false, matrix.inverse_threads,
        getnative::MetalKernelDispatchPolicy::generic_only,
    };
    getnative::MetalAnalysisOptions comparison_options = generic_options;
    comparison_options.kernel_dispatch = config.required_specialized
        ? getnative::MetalKernelDispatchPolicy::required_specialized
        : getnative::MetalKernelDispatchPolicy::automatic;
    getnative::MetalAnalysisEngine generic(generic_options);
    getnative::MetalAnalysisEngine comparison(comparison_options);
    const auto generic_creation = generic.runtime_telemetry();
    const auto comparison_creation = comparison.runtime_telemetry();

    (void)generic.analyze_axis_batch_f32(source, candidates, matrix.metric);
    (void)comparison.analyze_axis_batch_f32(source, candidates, matrix.metric);

    CaseReport report;
    report.benchmark_case = benchmark_case;
    report.shape = shape_name(*candidates.front().vertical);
    report.cpu_oracle_ms = cpu_ms;
    report.generic_creation = generic_creation;
    report.comparison_creation = comparison_creation;
    report.host_before = std::move(case_host_before);
    report.concurrent_process_detected = !report.host_before.concurrent_processes.empty();
    report.samples.reserve(config.samples);
    std::vector<double> generic_times;
    std::vector<double> comparison_times;
    std::vector<double> deltas;
    generic_times.reserve(config.samples);
    comparison_times.reserve(config.samples);
    deltas.reserve(config.samples);

    for (std::size_t sample = 0; sample < config.samples; ++sample) {
        PairSample pair;
        pair.host_before = capture_host_observation();
        if (sample % 2U == 0U) {
            pair.first_mode = "generic";
            pair.generic = measure(generic, source, candidates, matrix.metric);
            pair.comparison = measure(comparison, source, candidates, matrix.metric);
        } else {
            pair.first_mode = "comparison";
            pair.comparison = measure(comparison, source, candidates, matrix.metric);
            pair.generic = measure(generic, source, candidates, matrix.metric);
        }
        pair.host_after = capture_host_observation();
        pair.delta = (pair.comparison.wall_ms - pair.generic.wall_ms) / pair.generic.wall_ms;
        report.thermal_stable = report.thermal_stable
            && pair.host_before.thermal_state == report.host_before.thermal_state
            && pair.host_after.thermal_state == report.host_before.thermal_state;
        report.concurrent_process_detected = report.concurrent_process_detected
            || !pair.host_before.concurrent_processes.empty()
            || !pair.host_after.concurrent_processes.empty();
        generic_times.push_back(pair.generic.wall_ms);
        comparison_times.push_back(pair.comparison.wall_ms);
        deltas.push_back(pair.delta);
        report.samples.push_back(std::move(pair));
    }

    report.generic_median_ms = median(generic_times);
    report.comparison_median_ms = median(comparison_times);
    report.generic_p95_ms = percentile95(generic_times);
    report.comparison_p95_ms = percentile95(comparison_times);
    report.paired_delta_median = median(deltas);
    report.paired_delta_mad = median_absolute_deviation(deltas);
    const std::size_t decile_count = std::max<std::size_t>(1, deltas.size() / 10U);
    report.first_decile_delta_median = median(std::vector<double>(
        deltas.begin(), deltas.begin() + static_cast<std::ptrdiff_t>(decile_count)));
    report.last_decile_delta_median = median(std::vector<double>(
        deltas.end() - static_cast<std::ptrdiff_t>(decile_count), deltas.end()));
    report.generic_accuracy = accuracy_against(cpu, report.samples.front().generic.results);
    report.comparison_accuracy = accuracy_against(cpu, report.samples.front().comparison.results);
    report.maximum_path_difference = maximum_difference(
        report.samples.front().generic.results, report.samples.front().comparison.results);
    report.generic_peak_working_set = generic.peak_working_set_bytes();
    report.comparison_peak_working_set = comparison.peak_working_set_bytes();
    report.cpu_results = std::move(cpu);
    report.host_after = capture_host_observation();
    report.thermal_stable = report.thermal_stable
        && report.host_after.thermal_state == report.host_before.thermal_state;
    report.concurrent_process_detected = report.concurrent_process_detected
        || !report.host_after.concurrent_processes.empty();
    report.gate_passed = report.generic_accuracy.within_tolerance
        && report.comparison_accuracy.within_tolerance
        && report.generic_accuracy.valley_distance <= 1
        && report.comparison_accuracy.valley_distance <= 1
        && report.paired_delta_mad <= 0.02
        && report.thermal_stable
        && !report.concurrent_process_detected
        && report.generic_peak_working_set < absolute_working_set_limit
        && report.comparison_peak_working_set < absolute_working_set_limit
        && (!config.required_specialized || report.paired_delta_median <= -0.05)
        && (!config.required_specialized
            || report.samples.front().comparison.telemetry.specialized_tile_count > 0);
    return report;
}

[[nodiscard]] bool float_vectors_bit_identical(std::span<const float> first,
                                               std::span<const float> second) {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (std::bit_cast<std::uint32_t>(first[index])
            != std::bit_cast<std::uint32_t>(second[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool workspaces_bit_identical(const getnative::CpuWorkspace &first,
                                             const getnative::CpuWorkspace &second) {
    return float_vectors_bit_identical(first.intermediate, second.intermediate)
        && float_vectors_bit_identical(first.native, second.native)
        && float_vectors_bit_identical(first.reconstruction_row, second.reconstruction_row);
}

[[nodiscard]] std::string workspace_sha256(const getnative::CpuWorkspace &workspace) {
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    const auto update = [&](std::span<const float> values) {
        const std::uint64_t count = values.size();
        CC_SHA256_Update(&context, &count, static_cast<CC_LONG>(sizeof(count)));
        const auto *bytes = reinterpret_cast<const unsigned char *>(values.data());
        std::size_t size = values.size_bytes();
        while (size > 0) {
            const std::size_t chunk = std::min<std::size_t>(
                size, static_cast<std::size_t>(std::numeric_limits<CC_LONG>::max()));
            CC_SHA256_Update(&context, bytes, static_cast<CC_LONG>(chunk));
            bytes += chunk;
            size -= chunk;
        }
    };
    update(workspace.intermediate);
    update(workspace.native);
    update(workspace.reconstruction_row);
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &context);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char value : digest) {
        output << std::setw(2) << static_cast<int>(value);
    }
    return output.str();
}

[[nodiscard]] CpuMeasurement measure_cpu_columns(
    getnative::ConstImageView source, const getnative::AxisPlan &plan,
    const getnative::MetricSpec &metric, getnative::detail::ColumnDispatchPolicy policy,
    std::vector<float> &inverse_output, getnative::CpuWorkspace &candidate_workspace) {
    const auto inverse_start = Clock::now();
    getnative::detail::inverse_columns_f32(
        plan, source.data, source.stride, inverse_output.data(), source.width,
        source.width, policy);
    const double inverse_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - inverse_start).count();

    const auto candidate_start = Clock::now();
    const double error = getnative::detail::analyze_axis_candidate_with_column_policy_f32(
        source, plan, getnative::AnalysisAxes::vertical, metric, candidate_workspace,
        policy);
    const double candidate_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - candidate_start).count();
    return {inverse_ms, candidate_ms, error};
}

void update_cpu_correctness(CpuCaseReport &report,
                            const CpuMeasurement &scalar,
                            const CpuMeasurement &comparison,
                            std::span<const float> scalar_inverse,
                            std::span<const float> comparison_inverse,
                            const getnative::CpuWorkspace &scalar_workspace,
                            const getnative::CpuWorkspace &comparison_workspace) {
    report.inverse_bit_identical = report.inverse_bit_identical
        && float_vectors_bit_identical(scalar_inverse, comparison_inverse);
    report.workspace_bit_identical = report.workspace_bit_identical
        && workspaces_bit_identical(scalar_workspace, comparison_workspace);
    report.metric_bit_identical = report.metric_bit_identical
        && std::bit_cast<std::uint64_t>(scalar.error)
            == std::bit_cast<std::uint64_t>(comparison.error);
}

[[nodiscard]] CpuCaseReport run_cpu_case(
    const Matrix &matrix, const BenchmarkCase &benchmark_case,
    const DecodedImage &image, const Configuration &config) {
    CpuCaseReport report;
    report.benchmark_case = benchmark_case;
    report.measured_candidate_index = matrix.candidates / 2U;
    const CandidatePoint point = candidate_point(
        matrix, benchmark_case, report.measured_candidate_index);
    report.measured_native_height = point.native_height;
    report.measured_active_height = point.active_height;
    report.comparison_isa = std::string{getnative::detail::column_simd_name()};
    report.host_before = capture_host_observation();
    report.concurrent_process_detected = !report.host_before.concurrent_processes.empty();

    getnative::AxisPlanCache cache;
    const std::shared_ptr<const getnative::AxisPlan> plan = cache.get_or_build({
        matrix.source_height, point.native_height, point.active_height, 0.0,
        make_filter(benchmark_case.filter), getnative::BorderMode::mirror,
    });
    report.shape = shape_name(*plan);
    const getnative::ConstImageView source{
        image.luma.data(), image.width, image.height, image.width,
    };
    const std::size_t destination_rows = static_cast<std::size_t>(plan->destination_size);
    const std::size_t source_columns = static_cast<std::size_t>(source.width);
    if (destination_rows > std::numeric_limits<std::size_t>::max() / source_columns) {
        throw std::length_error("CPU inverse benchmark workspace size overflow");
    }
    std::vector<float> scalar_inverse(destination_rows * source_columns);
    std::vector<float> comparison_inverse(destination_rows * source_columns);
    getnative::CpuWorkspace scalar_workspace;
    getnative::CpuWorkspace comparison_workspace;
    const getnative::detail::ColumnDispatchPolicy comparison_policy = config.required_simd
        ? getnative::detail::ColumnDispatchPolicy::required_simd
        : getnative::detail::ColumnDispatchPolicy::automatic;

    const CpuMeasurement scalar_warm = measure_cpu_columns(
        source, *plan, matrix.metric, getnative::detail::ColumnDispatchPolicy::scalar_only,
        scalar_inverse, scalar_workspace);
    const CpuMeasurement comparison_warm = measure_cpu_columns(
        source, *plan, matrix.metric, comparison_policy,
        comparison_inverse, comparison_workspace);
    update_cpu_correctness(
        report, scalar_warm, comparison_warm, scalar_inverse, comparison_inverse,
        scalar_workspace, comparison_workspace);

    report.samples.reserve(config.samples);
    std::vector<double> scalar_inverse_times;
    std::vector<double> comparison_inverse_times;
    std::vector<double> scalar_candidate_times;
    std::vector<double> comparison_candidate_times;
    std::vector<double> inverse_deltas;
    std::vector<double> candidate_deltas;
    scalar_inverse_times.reserve(config.samples);
    comparison_inverse_times.reserve(config.samples);
    scalar_candidate_times.reserve(config.samples);
    comparison_candidate_times.reserve(config.samples);
    inverse_deltas.reserve(config.samples);
    candidate_deltas.reserve(config.samples);

    for (std::size_t sample = 0; sample < config.samples; ++sample) {
        CpuPairSample pair;
        pair.host_before = capture_host_observation();
        if (sample % 2U == 0U) {
            pair.first_mode = "scalar";
            pair.scalar = measure_cpu_columns(
                source, *plan, matrix.metric,
                getnative::detail::ColumnDispatchPolicy::scalar_only,
                scalar_inverse, scalar_workspace);
            pair.comparison = measure_cpu_columns(
                source, *plan, matrix.metric, comparison_policy,
                comparison_inverse, comparison_workspace);
        } else {
            pair.first_mode = "comparison";
            pair.comparison = measure_cpu_columns(
                source, *plan, matrix.metric, comparison_policy,
                comparison_inverse, comparison_workspace);
            pair.scalar = measure_cpu_columns(
                source, *plan, matrix.metric,
                getnative::detail::ColumnDispatchPolicy::scalar_only,
                scalar_inverse, scalar_workspace);
        }
        pair.host_after = capture_host_observation();
        if (!(pair.scalar.inverse_ms > 0.0) || !(pair.scalar.candidate_ms > 0.0)) {
            throw std::runtime_error("CPU benchmark scalar timing is not positive");
        }
        pair.inverse_delta = (pair.comparison.inverse_ms - pair.scalar.inverse_ms)
            / pair.scalar.inverse_ms;
        pair.candidate_delta = (pair.comparison.candidate_ms - pair.scalar.candidate_ms)
            / pair.scalar.candidate_ms;
        update_cpu_correctness(
            report, pair.scalar, pair.comparison, scalar_inverse, comparison_inverse,
            scalar_workspace, comparison_workspace);
        report.thermal_stable = report.thermal_stable
            && pair.host_before.thermal_state == report.host_before.thermal_state
            && pair.host_after.thermal_state == report.host_before.thermal_state;
        report.concurrent_process_detected = report.concurrent_process_detected
            || !pair.host_before.concurrent_processes.empty()
            || !pair.host_after.concurrent_processes.empty();
        scalar_inverse_times.push_back(pair.scalar.inverse_ms);
        comparison_inverse_times.push_back(pair.comparison.inverse_ms);
        scalar_candidate_times.push_back(pair.scalar.candidate_ms);
        comparison_candidate_times.push_back(pair.comparison.candidate_ms);
        inverse_deltas.push_back(pair.inverse_delta);
        candidate_deltas.push_back(pair.candidate_delta);
        report.samples.push_back(std::move(pair));
    }

    report.scalar_inverse_median_ms = median(scalar_inverse_times);
    report.comparison_inverse_median_ms = median(comparison_inverse_times);
    report.scalar_candidate_median_ms = median(scalar_candidate_times);
    report.comparison_candidate_median_ms = median(comparison_candidate_times);
    report.inverse_delta_median = median(inverse_deltas);
    report.inverse_delta_mad = median_absolute_deviation(inverse_deltas);
    report.candidate_delta_median = median(candidate_deltas);
    report.candidate_delta_mad = median_absolute_deviation(candidate_deltas);
    report.scalar_inverse_sha256 = sha256_bytes(
        scalar_inverse.data(), scalar_inverse.size() * sizeof(float));
    report.comparison_inverse_sha256 = sha256_bytes(
        comparison_inverse.data(), comparison_inverse.size() * sizeof(float));
    report.scalar_workspace_sha256 = workspace_sha256(scalar_workspace);
    report.comparison_workspace_sha256 = workspace_sha256(comparison_workspace);
    report.host_after = capture_host_observation();
    report.thermal_stable = report.thermal_stable
        && report.host_after.thermal_state == report.host_before.thermal_state;
    report.concurrent_process_detected = report.concurrent_process_detected
        || !report.host_after.concurrent_processes.empty();
    const bool primary_case = !benchmark_case.fractional_scan
        && benchmark_case.native_height == matrix.primary_native_height
        && benchmark_case.filter.id == matrix.cpu_primary_filter_id;
    const double maximum_allowed_delta = primary_case ? -0.05 : 0.03;
    report.gate_passed = report.inverse_bit_identical
        && report.workspace_bit_identical
        && report.metric_bit_identical
        && report.inverse_delta_mad <= 0.02
        && report.candidate_delta_mad <= 0.02
        && report.thermal_stable
        && !report.concurrent_process_detected
        && (!config.required_simd
            || (getnative::detail::column_simd_available()
                && report.candidate_delta_median <= maximum_allowed_delta));
    return report;
}

[[nodiscard]] NSMutableArray *strings_json(const std::vector<std::string> &values) {
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:values.size()];
    for (const std::string &value : values) [result addObject:@(value.c_str())];
    return result;
}

[[nodiscard]] NSDictionary *telemetry_json(const getnative::MetalRuntimeTelemetry &value) {
    return @{
        @"buffer_allocation_count": @(value.buffer_allocation_count),
        @"buffer_allocation_bytes": @(value.buffer_allocation_bytes),
        @"analyzed_tile_count": @(value.analyzed_tile_count),
        @"generic_tile_count": @(value.generic_tile_count),
        @"specialized_tile_count": @(value.specialized_tile_count),
        @"pipeline_creation_ms": @(value.pipeline_creation_ms),
        @"gpu_execution_ms": @(value.gpu_execution_ms),
        @"created_pipeline_names": strings_json(value.created_pipeline_names),
    };
}

[[nodiscard]] NSDictionary *accuracy_json(const Accuracy &value) {
    return @{
        @"maximum_error": @(value.maximum_error),
        @"reference_minimum_index": @(value.reference_minimum_index),
        @"actual_minimum_index": @(value.actual_minimum_index),
        @"valley_distance": @(value.valley_distance),
        @"within_tolerance": @(value.within_tolerance),
    };
}

[[nodiscard]] NSMutableArray *results_json(
    std::span<const getnative::CandidateResult> values) {
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:values.size()];
    for (const getnative::CandidateResult &value : values) {
        [result addObject:@{
            @"id": @(value.id.c_str()),
            @"error": @(value.error),
        }];
    }
    return result;
}

[[nodiscard]] NSDictionary *host_observation_json(const HostObservation &value) {
    return @{
        @"thermal_state": @(value.thermal_state.c_str()),
        @"load_average": @[
            @(value.load_average[0]),
            @(value.load_average[1]),
            @(value.load_average[2]),
        ],
        @"concurrent_processes": strings_json(value.concurrent_processes),
    };
}

[[nodiscard]] NSDictionary *identity_json(const IdentitySnapshot &value) {
    return @{
        @"binary_path": @(value.binary_path.string().c_str()),
        @"binary_bytes": @(value.binary_bytes),
        @"binary_sha256": @(value.binary_sha256.c_str()),
        @"metallib_path": @(value.metallib_path.string().c_str()),
        @"metallib_bytes": @(value.metallib_bytes),
        @"metallib_sha256": @(value.metallib_sha256.c_str()),
        @"matrix_sha256": @(value.matrix_sha256.c_str()),
        @"fixture_sha256": @(value.fixture_sha256.c_str()),
        @"fixture_checksum_sha256": @(value.fixture_checksum_sha256.c_str()),
    };
}

[[nodiscard]] NSDictionary *case_json(const CaseReport &report) {
    NSMutableArray *samples = [NSMutableArray arrayWithCapacity:report.samples.size()];
    for (std::size_t index = 0; index < report.samples.size(); ++index) {
        const PairSample &sample = report.samples[index];
        [samples addObject:@{
            @"index": @(index),
            @"first_mode": @(sample.first_mode.c_str()),
            @"generic_ms": @(sample.generic.wall_ms),
            @"comparison_ms": @(sample.comparison.wall_ms),
            @"paired_delta": @(sample.delta),
            @"generic_telemetry": telemetry_json(sample.generic.telemetry),
            @"comparison_telemetry": telemetry_json(sample.comparison.telemetry),
            @"host_before": host_observation_json(sample.host_before),
            @"host_after": host_observation_json(sample.host_after),
        }];
    }
    return @{
        @"id": @(report.benchmark_case.id.c_str()),
        @"filter_id": @(report.benchmark_case.filter.id.c_str()),
        @"shape": @(report.shape.c_str()),
        @"native_height": @(report.benchmark_case.native_height),
        @"fractional_scan": @(report.benchmark_case.fractional_scan),
        @"cpu_oracle_ms": @(report.cpu_oracle_ms),
        @"generic_median_ms": @(report.generic_median_ms),
        @"comparison_median_ms": @(report.comparison_median_ms),
        @"generic_p95_ms": @(report.generic_p95_ms),
        @"comparison_p95_ms": @(report.comparison_p95_ms),
        @"paired_delta_median": @(report.paired_delta_median),
        @"paired_delta_mad": @(report.paired_delta_mad),
        @"first_decile_delta_median": @(report.first_decile_delta_median),
        @"last_decile_delta_median": @(report.last_decile_delta_median),
        @"generic_accuracy": accuracy_json(report.generic_accuracy),
        @"comparison_accuracy": accuracy_json(report.comparison_accuracy),
        @"maximum_path_difference": @(report.maximum_path_difference),
        @"result_vectors": @{
            @"cpu_oracle": results_json(report.cpu_results),
            @"generic": results_json(report.samples.front().generic.results),
            @"comparison": results_json(report.samples.front().comparison.results),
        },
        @"generic_peak_working_set_bytes": @(report.generic_peak_working_set),
        @"comparison_peak_working_set_bytes": @(report.comparison_peak_working_set),
        @"generic_pipeline_creation": telemetry_json(report.generic_creation),
        @"comparison_pipeline_creation": telemetry_json(report.comparison_creation),
        @"host_before": host_observation_json(report.host_before),
        @"host_after": host_observation_json(report.host_after),
        @"thermal_stable": @(report.thermal_stable),
        @"concurrent_process_detected": @(report.concurrent_process_detected),
        @"gate_passed": @(report.gate_passed),
        @"samples": samples,
    };
}

[[nodiscard]] NSDictionary *cpu_case_json(const CpuCaseReport &report) {
    NSMutableArray *samples = [NSMutableArray arrayWithCapacity:report.samples.size()];
    for (std::size_t index = 0; index < report.samples.size(); ++index) {
        const CpuPairSample &sample = report.samples[index];
        [samples addObject:@{
            @"index": @(index),
            @"first_mode": @(sample.first_mode.c_str()),
            @"scalar_inverse_ms": @(sample.scalar.inverse_ms),
            @"comparison_inverse_ms": @(sample.comparison.inverse_ms),
            @"inverse_paired_delta": @(sample.inverse_delta),
            @"scalar_candidate_ms": @(sample.scalar.candidate_ms),
            @"comparison_candidate_ms": @(sample.comparison.candidate_ms),
            @"candidate_paired_delta": @(sample.candidate_delta),
            @"scalar_metric": @(sample.scalar.error),
            @"comparison_metric": @(sample.comparison.error),
            @"host_before": host_observation_json(sample.host_before),
            @"host_after": host_observation_json(sample.host_after),
        }];
    }
    return @{
        @"id": @(report.benchmark_case.id.c_str()),
        @"filter_id": @(report.benchmark_case.filter.id.c_str()),
        @"shape": @(report.shape.c_str()),
        @"matrix_native_height": @(report.benchmark_case.native_height),
        @"fractional_scan": @(report.benchmark_case.fractional_scan),
        @"measured_candidate_index": @(report.measured_candidate_index),
        @"measured_native_height": @(report.measured_native_height),
        @"measured_active_height": @(report.measured_active_height),
        @"comparison_isa": @(report.comparison_isa.c_str()),
        @"scalar_inverse_median_ms": @(report.scalar_inverse_median_ms),
        @"comparison_inverse_median_ms": @(report.comparison_inverse_median_ms),
        @"inverse_paired_delta_median": @(report.inverse_delta_median),
        @"inverse_paired_delta_mad": @(report.inverse_delta_mad),
        @"scalar_candidate_median_ms": @(report.scalar_candidate_median_ms),
        @"comparison_candidate_median_ms": @(report.comparison_candidate_median_ms),
        @"candidate_paired_delta_median": @(report.candidate_delta_median),
        @"candidate_paired_delta_mad": @(report.candidate_delta_mad),
        @"inverse_bit_identical": @(report.inverse_bit_identical),
        @"workspace_bit_identical": @(report.workspace_bit_identical),
        @"metric_bit_identical": @(report.metric_bit_identical),
        @"scalar_inverse_sha256": @(report.scalar_inverse_sha256.c_str()),
        @"comparison_inverse_sha256": @(report.comparison_inverse_sha256.c_str()),
        @"scalar_workspace_sha256": @(report.scalar_workspace_sha256.c_str()),
        @"comparison_workspace_sha256": @(report.comparison_workspace_sha256.c_str()),
        @"host_before": host_observation_json(report.host_before),
        @"host_after": host_observation_json(report.host_after),
        @"thermal_stable": @(report.thermal_stable),
        @"concurrent_process_detected": @(report.concurrent_process_detected),
        @"gate_passed": @(report.gate_passed),
        @"samples": samples,
    };
}

[[nodiscard]] std::string compiler_identity() {
#if defined(__clang__)
    return std::string{"clang "} + __clang_version__;
#elif defined(__GNUC__)
    return std::string{"gcc "} + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string target_architecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::filesystem::path executable_path() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("could not resolve benchmark executable path");
    }
    return std::filesystem::weakly_canonical(path.data());
}

[[nodiscard]] IdentitySnapshot capture_identity_snapshot(
    const Configuration &config, const std::filesystem::path &fixture_checksum_path) {
    IdentitySnapshot result;
    result.binary_path = executable_path();
    result.metallib_path = std::filesystem::weakly_canonical(GETNATIVE_METALLIB_PATH);
    result.binary_bytes = std::filesystem::file_size(result.binary_path);
    result.metallib_bytes = std::filesystem::file_size(result.metallib_path);
    result.binary_sha256 = sha256_file(result.binary_path);
    result.metallib_sha256 = sha256_file(result.metallib_path);
    result.matrix_sha256 = sha256_file(config.matrix_path);
    result.fixture_sha256 = sha256_file(config.fixture_path);
    result.fixture_checksum_sha256 = sha256_file(fixture_checksum_path);
    return result;
}

[[nodiscard]] bool identities_equal(const IdentitySnapshot &lhs,
                                    const IdentitySnapshot &rhs) {
    return lhs.binary_path == rhs.binary_path
        && lhs.metallib_path == rhs.metallib_path
        && lhs.binary_bytes == rhs.binary_bytes
        && lhs.metallib_bytes == rhs.metallib_bytes
        && lhs.binary_sha256 == rhs.binary_sha256
        && lhs.metallib_sha256 == rhs.metallib_sha256
        && lhs.matrix_sha256 == rhs.matrix_sha256
        && lhs.fixture_sha256 == rhs.fixture_sha256
        && lhs.fixture_checksum_sha256 == rhs.fixture_checksum_sha256;
}

[[nodiscard]] std::string utc_stamp() {
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    formatter.dateFormat = @"yyyyMMdd'T'HHmmssSSS'Z'";
    return from_ns_string([formatter stringFromDate:[NSDate date]]);
}

[[nodiscard]] std::filesystem::path output_path(const Configuration &config) {
    if (config.json_path) return *config.json_path;
    return *config.artifact_root
        / (utc_stamp() + "-pid" + std::to_string(static_cast<long long>(getpid())))
        / (config.cpu_columns ? "cpu-column-report.json" : "metal-kernel-report.json");
}

void write_json(NSDictionary *root, const std::filesystem::path &path) {
    NSFileManager *manager = NSFileManager.defaultManager;
    const std::filesystem::path parent_path = path.has_parent_path()
        ? path.parent_path() : std::filesystem::path{"."};
    NSString *parent = path_string(parent_path);
    NSError *error = nil;
    if (![manager createDirectoryAtPath:parent withIntermediateDirectories:YES
                             attributes:nil error:&error]) {
        throw std::runtime_error("could not create artifact directory: "
                                 + from_ns_string(error.localizedDescription));
    }
    NSData *data = [NSJSONSerialization dataWithJSONObject:root
                                                   options:(NSJSONWritingPrettyPrinted
                                                            | NSJSONWritingSortedKeys)
                                                     error:&error];
    if (data == nil) {
        throw std::runtime_error("could not serialize report JSON: "
                                 + from_ns_string(error.localizedDescription));
    }

    std::filesystem::path temporary_path;
    int descriptor = -1;
    try {
        for (std::size_t attempt = 0; attempt < 100U; ++attempt) {
            temporary_path = parent_path
                / ("." + path.filename().string() + ".tmp-pid"
                   + std::to_string(static_cast<long long>(getpid())) + "-"
                   + std::to_string(attempt));
            descriptor = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (descriptor >= 0) break;
            if (errno != EEXIST) {
                throw std::system_error(errno, std::generic_category(),
                                        "could not create report temporary file");
            }
        }
        if (descriptor < 0) {
            throw std::runtime_error("could not reserve a report temporary file");
        }

        const auto *bytes = static_cast<const std::uint8_t *>(data.bytes);
        std::size_t remaining = data.length;
        while (remaining > 0) {
            const std::size_t chunk = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t written = write(descriptor, bytes, chunk);
            if (written < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(),
                                        "could not write report temporary file");
            }
            if (written == 0) throw std::runtime_error("report write made no progress");
            bytes += static_cast<std::size_t>(written);
            remaining -= static_cast<std::size_t>(written);
        }
        if (fsync(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "could not flush report temporary file");
        }
        if (close(descriptor) != 0) {
            const int close_error = errno;
            descriptor = -1;
            throw std::system_error(close_error, std::generic_category(),
                                    "could not close report temporary file");
        }
        descriptor = -1;

        if (renamex_np(temporary_path.c_str(), path.c_str(), RENAME_EXCL) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "could not publish report without overwriting");
        }
        temporary_path.clear();
    } catch (...) {
        if (descriptor >= 0) (void)close(descriptor);
        if (!temporary_path.empty()) (void)unlink(temporary_path.c_str());
        throw;
    }
}

} // namespace

int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            const Configuration config = parse_arguments(argc, argv);
            const std::filesystem::path report_path = output_path(config);
            std::error_code output_error;
            if (std::filesystem::exists(report_path, output_error)) {
                throw std::runtime_error("report path already exists: " + report_path.string());
            }
            if (output_error) {
                throw std::system_error(output_error, "could not inspect report path");
            }
            std::filesystem::path fixture_checksum_path = config.fixture_path;
            fixture_checksum_path += ".sha256";
            const IdentitySnapshot identity_before = capture_identity_snapshot(
                config, fixture_checksum_path);
            const Matrix matrix = load_matrix(config.matrix_path);
            const DecodedImage image = decode_luma(config.fixture_path);
            const IdentitySnapshot identity_after_setup = capture_identity_snapshot(
                config, fixture_checksum_path);
            if (!identities_equal(identity_before, identity_after_setup)
                || image.fixture_sha256 != identity_before.fixture_sha256) {
                throw std::runtime_error("benchmark identities changed while loading inputs");
            }
            if (image.width != matrix.source_width || image.height != matrix.source_height) {
                throw std::runtime_error("decoded fixture dimensions do not match the matrix");
            }
            if (config.cpu_columns) {
                if (config.required_simd && !getnative::detail::column_simd_available()) {
                    throw std::runtime_error(
                        "required adjacent-column SIMD is unavailable on this host");
                }
                const auto cases = select_cases(matrix, config);
                const HostObservation run_host_before = capture_host_observation();
                if (!run_host_before.concurrent_processes.empty()) {
                    throw std::runtime_error(
                        "concurrent benchmark or capture process detected: "
                        + run_host_before.concurrent_processes.front());
                }
                std::vector<CpuCaseReport> reports;
                reports.reserve(cases.size());
                bool all_passed = true;
                for (const BenchmarkCase &benchmark_case : cases) {
                    std::cout << "running CPU columns " << benchmark_case.id << " ("
                              << config.samples << " paired samples)\n" << std::flush;
                    CpuCaseReport report = run_cpu_case(
                        matrix, benchmark_case, image, config);
                    std::cout << std::fixed << std::setprecision(3)
                              << "case=" << report.benchmark_case.id
                              << " shape=" << report.shape
                              << " isa=" << report.comparison_isa
                              << " inverse_improvement="
                              << -100.0 * report.inverse_delta_median << "%"
                              << " candidate_improvement="
                              << -100.0 * report.candidate_delta_median << "%"
                              << " inverse_mad=" << report.inverse_delta_mad
                              << " candidate_mad=" << report.candidate_delta_mad
                              << " gate=" << (report.gate_passed ? "pass" : "fail") << '\n';
                    all_passed = all_passed && report.gate_passed;
                    reports.push_back(std::move(report));
                }

                const HostObservation run_host_after = capture_host_observation();
                const IdentitySnapshot identity_after = capture_identity_snapshot(
                    config, fixture_checksum_path);
                const bool identity_stable = identities_equal(identity_before, identity_after);
                const bool run_thermal_stable =
                    run_host_before.thermal_state == run_host_after.thermal_state;
                const bool run_concurrent_process_detected =
                    !run_host_before.concurrent_processes.empty()
                    || !run_host_after.concurrent_processes.empty();
                all_passed = all_passed && identity_stable && run_thermal_stable
                    && !run_concurrent_process_detected;

                NSMutableArray *case_objects = [NSMutableArray arrayWithCapacity:reports.size()];
                for (const CpuCaseReport &report : reports) {
                    [case_objects addObject:cpu_case_json(report)];
                }
                const std::string architecture = target_architecture();
                const std::string compiler = compiler_identity();
                const std::string selected_isa{getnative::detail::column_simd_name()};
                NSDictionary *root = @{
                    @"schema_version": @1,
                    @"status": all_passed ? @"pass" : @"fail",
                    @"mode": @"cpu_columns",
                    @"case_selection": config.controls_only ? @"controls" : @"full_matrix",
                    @"comparison_policy": config.required_simd
                        ? @"required_simd" : @"automatic",
                    @"sample_count": @(config.samples),
                    @"host_before": host_observation_json(run_host_before),
                    @"host_after": host_observation_json(run_host_after),
                    @"run_thermal_stable": @(run_thermal_stable),
                    @"run_concurrent_process_detected": @(run_concurrent_process_detected),
                    @"cpu": @{
                        @"architecture": @(architecture.c_str()),
                        @"compiler": @(compiler.c_str()),
                        @"simd_available": @(getnative::detail::column_simd_available()),
                        @"selected_isa": @(selected_isa.c_str()),
                        @"primary_filter_id": @(matrix.cpu_primary_filter_id.c_str()),
                        @"primary_native_height": @(matrix.primary_native_height),
                    },
                    @"fixture": @{
                        @"path": @(config.fixture_path.string().c_str()),
                        @"checksum_path": @(image.checksum_path.string().c_str()),
                        @"expected_sha256": @(image.expected_fixture_sha256.c_str()),
                        @"sha256": @(image.fixture_sha256.c_str()),
                        @"decoded_luma_sha256": @(image.luma_sha256.c_str()),
                        @"width": @(image.width),
                        @"height": @(image.height),
                    },
                    @"identities": @{
                        @"stable": @(identity_stable),
                        @"before": identity_json(identity_before),
                        @"after": identity_json(identity_after),
                        @"binary_sha256": @(identity_before.binary_sha256.c_str()),
                        @"matrix_path": @(config.matrix_path.string().c_str()),
                        @"matrix_sha256": @(identity_before.matrix_sha256.c_str()),
                    },
                    @"cases": case_objects,
                };
                write_json(root, report_path);
                std::cout << "report=" << report_path << '\n';
                if (config.assert_gates && !all_passed) return EXIT_FAILURE;
                return EXIT_SUCCESS;
            }
            if (!getnative::metal_backend_available()) {
                throw std::runtime_error("no Metal device is available");
            }
            const auto cases = select_cases(matrix, config);
            const HostObservation run_host_before = capture_host_observation();
            if (!run_host_before.concurrent_processes.empty()) {
                throw std::runtime_error(
                    "concurrent benchmark or capture process detected: "
                    + run_host_before.concurrent_processes.front());
            }
            std::vector<CaseReport> reports;
            reports.reserve(cases.size());
            bool all_passed = true;
            for (const BenchmarkCase &benchmark_case : cases) {
                std::cout << "running " << benchmark_case.id << " (" << config.samples
                          << " paired samples)\n" << std::flush;
                CaseReport report = run_case(matrix, benchmark_case, image, config);
                std::cout << std::fixed << std::setprecision(3)
                          << "case=" << report.benchmark_case.id
                          << " shape=" << report.shape
                          << " generic_ms=" << report.generic_median_ms
                          << " comparison_ms=" << report.comparison_median_ms
                          << " improvement=" << -100.0 * report.paired_delta_median << "%"
                          << " paired_mad=" << report.paired_delta_mad
                          << " gate=" << (report.gate_passed ? "pass" : "fail") << '\n';
                all_passed = all_passed && report.gate_passed;
                reports.push_back(std::move(report));
            }

            const HostObservation run_host_after = capture_host_observation();
            const IdentitySnapshot identity_after = capture_identity_snapshot(
                config, fixture_checksum_path);
            const bool identity_stable = identities_equal(identity_before, identity_after);
            const bool run_thermal_stable =
                run_host_before.thermal_state == run_host_after.thermal_state;
            const bool run_concurrent_process_detected =
                !run_host_before.concurrent_processes.empty()
                || !run_host_after.concurrent_processes.empty();
            all_passed = all_passed && identity_stable && run_thermal_stable
                && !run_concurrent_process_detected;

            getnative::MetalAnalysisEngine device_probe;
            NSMutableArray *case_objects = [NSMutableArray arrayWithCapacity:reports.size()];
            for (const CaseReport &report : reports) [case_objects addObject:case_json(report)];
            const std::filesystem::path binary = executable_path();
            const std::filesystem::path metallib = GETNATIVE_METALLIB_PATH;
            NSDictionary *root = @{
                @"schema_version": @1,
                @"status": all_passed ? @"pass" : @"fail",
                @"mode": config.sustained ? @"sustained"
                    : (config.controls_only ? @"controls" : @"full_matrix"),
                @"comparison_policy": config.required_specialized
                    ? @"required_specialized" : @"automatic",
                @"sample_count": @(config.samples),
                @"host_before": host_observation_json(run_host_before),
                @"host_after": host_observation_json(run_host_after),
                @"run_thermal_stable": @(run_thermal_stable),
                @"run_concurrent_process_detected": @(run_concurrent_process_detected),
                @"device": @{
                    @"name": @(device_probe.device_info().name.c_str()),
                    @"registry_id": @(device_probe.device_info().registry_id),
                    @"maximum_buffer_bytes": @(device_probe.device_info().maximum_buffer_bytes),
                    @"unified_memory": @(device_probe.device_info().unified_memory),
                },
                @"fixture": @{
                    @"path": @(config.fixture_path.string().c_str()),
                    @"checksum_path": @(image.checksum_path.string().c_str()),
                    @"expected_sha256": @(image.expected_fixture_sha256.c_str()),
                    @"sha256": @(image.fixture_sha256.c_str()),
                    @"decoded_luma_sha256": @(image.luma_sha256.c_str()),
                    @"width": @(image.width),
                    @"height": @(image.height),
                },
                @"identities": @{
                    @"stable": @(identity_stable),
                    @"before": identity_json(identity_before),
                    @"after": identity_json(identity_after),
                    @"binary_path": @(binary.string().c_str()),
                    @"binary_sha256": @(identity_before.binary_sha256.c_str()),
                    @"metallib_path": @(metallib.string().c_str()),
                    @"metallib_sha256": @(identity_before.metallib_sha256.c_str()),
                    @"matrix_path": @(config.matrix_path.string().c_str()),
                    @"matrix_sha256": @(identity_before.matrix_sha256.c_str()),
                },
                @"cases": case_objects,
            };
            write_json(root, report_path);
            std::cout << "report=" << report_path << '\n';
            if (config.assert_gates && !all_passed) return EXIT_FAILURE;
            return EXIT_SUCCESS;
        } catch (const std::exception &error) {
            std::cerr << "Backend hotpath benchmark failure: " << error.what() << '\n';
            return EXIT_FAILURE;
        }
    }
}

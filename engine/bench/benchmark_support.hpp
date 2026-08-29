#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/stdio.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef GETNATIVE_BENCHMARK_SOURCE_ID
#define GETNATIVE_BENCHMARK_SOURCE_ID "unknown"
#endif

#ifndef GETNATIVE_BENCHMARK_BUILD_TYPE
#define GETNATIVE_BENCHMARK_BUILD_TYPE "unknown"
#endif

#ifndef GETNATIVE_BENCHMARK_CXX_FLAGS
#define GETNATIVE_BENCHMARK_CXX_FLAGS "unknown"
#endif

#ifndef GETNATIVE_BENCHMARK_SYSTEM_NAME
#define GETNATIVE_BENCHMARK_SYSTEM_NAME "unknown"
#endif

#ifndef GETNATIVE_BENCHMARK_SYSTEM_PROCESSOR
#define GETNATIVE_BENCHMARK_SYSTEM_PROCESSOR "unknown"
#endif

namespace getnative::benchmark {

struct Summary {
    std::vector<double> raw;
    double median = 0.0;
    double mad = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
};

[[nodiscard]] inline double median(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("cannot summarize an empty sample set");
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if ((values.size() & 1U) != 0U) return values[middle];
    return (values[middle - 1U] + values[middle]) / 2.0;
}

[[nodiscard]] inline Summary summarize(std::vector<double> values) {
    for (const double value : values) {
        if (!std::isfinite(value)) throw std::runtime_error("benchmark sample is nonfinite");
    }
    const double sample_median = median(values);
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    const double sample_minimum = *minimum;
    const double sample_maximum = *maximum;
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values) deviations.push_back(std::abs(value - sample_median));
    return {std::move(values), sample_median, median(std::move(deviations)),
            sample_minimum, sample_maximum};
}

[[nodiscard]] inline std::string json_string(std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << hex[character >> 4U] << hex[character & 0x0FU];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

inline void append_double_array(std::ostream &output, const std::vector<double> &values) {
    output << '[' << std::setprecision(17);
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) output << ',';
        output << values[index];
    }
    output << ']';
}

inline void append_summary(std::ostream &output, const Summary &summary) {
    output << "{\"raw\":";
    append_double_array(output, summary.raw);
    output << ",\"median\":" << std::setprecision(17) << summary.median
           << ",\"mad\":" << summary.mad
           << ",\"minimum\":" << summary.minimum
           << ",\"maximum\":" << summary.maximum << '}';
}

[[nodiscard]] inline std::string utc_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) != 0) throw std::runtime_error("failed to format UTC time");
#else
    if (gmtime_r(&now, &utc) == nullptr) throw std::runtime_error("failed to format UTC time");
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] inline std::string compiler_identity() {
#if defined(__clang__)
    return std::string{"Clang "} + __clang_version__;
#elif defined(__GNUC__)
    return std::string{"GCC "} + __VERSION__;
#elif defined(_MSC_VER)
    return std::string{"MSVC "} + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

[[nodiscard]] inline std::filesystem::path executable_path(std::string_view argument_zero) {
    std::error_code error;
    std::filesystem::path path{argument_zero};
    if (path.is_relative()) path = std::filesystem::absolute(path, error);
    if (error) throw std::runtime_error("failed to resolve benchmark executable path");
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

[[nodiscard]] inline std::string fnv1a64_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open benchmark executable for identity");
    std::uint64_t hash = 1469598103934665603ULL;
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    if (!input.eof()) throw std::runtime_error("failed while hashing benchmark executable");
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

inline void append_common_metadata(
    std::ostream &output,
    std::string_view benchmark_name,
    std::string_view planner_mode,
    std::string_view fixture_identity,
    int argc,
    char **argv) {
    const auto executable = executable_path(argv[0]);
    output << "\"schema_version\":1"
           << ",\"benchmark\":" << json_string(benchmark_name)
           << ",\"timestamp_utc\":" << json_string(utc_timestamp())
           << ",\"arguments\":[";
    for (int index = 0; index < argc; ++index) {
        if (index != 0) output << ',';
        output << json_string(argv[index]);
    }
    output << "]"
           << ",\"planner_mode\":" << json_string(planner_mode)
           << ",\"fixture_identity\":" << json_string(fixture_identity)
           << ",\"source_identity_sha256\":" << json_string(GETNATIVE_BENCHMARK_SOURCE_ID)
           << ",\"executable\":{\"path\":" << json_string(executable.string())
           << ",\"fnv1a64\":" << json_string(fnv1a64_file(executable)) << '}'
           << ",\"host\":{\"system\":" << json_string(GETNATIVE_BENCHMARK_SYSTEM_NAME)
           << ",\"processor\":" << json_string(GETNATIVE_BENCHMARK_SYSTEM_PROCESSOR)
           << ",\"logical_cpus\":" << std::thread::hardware_concurrency() << '}'
           << ",\"build\":{\"type\":" << json_string(GETNATIVE_BENCHMARK_BUILD_TYPE)
           << ",\"compiler\":" << json_string(compiler_identity())
           << ",\"cxx_standard\":23"
           << ",\"flags\":" << json_string(GETNATIVE_BENCHMARK_CXX_FLAGS) << '}';
}

inline void validate_json_output_path(const std::filesystem::path &final_path) {
    if (final_path.empty()) throw std::invalid_argument("JSON output path must not be empty");
    std::error_code error;
    if (std::filesystem::exists(final_path, error)) {
        throw std::runtime_error("JSON output path already exists");
    }
    if (error) throw std::runtime_error("failed to inspect JSON output path");
    const auto parent = final_path.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, error)) {
        throw std::runtime_error("JSON output parent directory does not exist");
    }
    if (error) throw std::runtime_error("failed to inspect JSON output parent directory");
}

inline void publish_json_no_replace(
    const std::filesystem::path &temporary_path,
    const std::filesystem::path &final_path) {
#if defined(_WIN32)
    // MoveFileExW without MOVEFILE_REPLACE_EXISTING is atomic on the same
    // volume and preserves the no-overwrite contract under publication races.
    if (::MoveFileExW(
            temporary_path.c_str(),
            final_path.c_str(),
            MOVEFILE_WRITE_THROUGH) == 0) {
        throw std::system_error(
            static_cast<int>(::GetLastError()),
            std::system_category(),
            "failed to atomically publish JSON output");
    }
#elif defined(__APPLE__)
    if (::renamex_np(temporary_path.c_str(), final_path.c_str(), RENAME_EXCL) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "failed to atomically publish JSON output");
    }
#else
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "failed to atomically publish JSON output");
    }
    if (::unlink(temporary_path.c_str()) != 0) {
        const int unlink_error = errno;
        (void)::unlink(final_path.c_str());
        throw std::system_error(unlink_error, std::generic_category(),
                                "failed to remove temporary JSON output after publication");
    }
#endif
}

inline void atomic_write_json(
    const std::filesystem::path &final_path,
    std::string_view contents) {
    validate_json_output_path(final_path);
    std::filesystem::path temporary_path = final_path;
    temporary_path += ".tmp";

#if defined(_WIN32)
    const int descriptor = _open(
        temporary_path.string().c_str(),
        _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
        _S_IREAD | _S_IWRITE);
#else
    const int descriptor = ::open(
        temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
#endif
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "failed to create temporary JSON output");
    }

    const auto close_descriptor = [descriptor]() noexcept {
#if defined(_WIN32)
        return _close(descriptor);
#else
        return ::close(descriptor);
#endif
    };

    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const std::size_t remaining = contents.size() - offset;
#if defined(_WIN32)
            const auto chunk = static_cast<unsigned int>(
                std::min<std::size_t>(remaining, std::numeric_limits<unsigned int>::max()));
            const int written = _write(descriptor, contents.data() + offset, chunk);
#else
            const auto written = ::write(descriptor, contents.data() + offset, remaining);
#endif
            if (written < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "failed to write temporary JSON output");
            }
            offset += static_cast<std::size_t>(written);
        }
#if defined(_WIN32)
        if (_commit(descriptor) != 0) {
#else
        if (::fsync(descriptor) != 0) {
#endif
            throw std::system_error(errno, std::generic_category(),
                                    "failed to flush temporary JSON output");
        }
        if (close_descriptor() != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to close temporary JSON output");
        }
    } catch (...) {
        (void)close_descriptor();
        std::error_code ignored;
        (void)std::filesystem::remove(temporary_path, ignored);
        throw;
    }

    try {
        publish_json_no_replace(temporary_path, final_path);
    } catch (...) {
        std::error_code ignored;
        (void)std::filesystem::remove(temporary_path, ignored);
        throw;
    }
}

} // namespace getnative::benchmark

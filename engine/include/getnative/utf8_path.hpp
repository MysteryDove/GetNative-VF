#pragma once

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace getnative {

// Paths crossing the worker protocol are UTF-8. On Windows, constructing a
// filesystem::path from std::string uses the active ANSI code page instead.
[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded(value.size(), u8'\0');
    std::memcpy(encoded.data(), value.data(), value.size());
    return std::filesystem::path{encoded};
}

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path &value) {
    const std::u8string encoded = value.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

} // namespace getnative

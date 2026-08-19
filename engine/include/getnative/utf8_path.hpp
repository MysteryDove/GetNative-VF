#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
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

// Windows stores the process environment as UTF-16. The narrow CRT view uses
// the active code page and can corrupt paths that contain CJK characters.
[[nodiscard]] inline std::optional<std::filesystem::path> path_from_environment(
    const char *name) {
    if (name == nullptr) return std::nullopt;
#if defined(_WIN32)
    std::wstring wide_name;
    for (const unsigned char value : std::string_view{name}) {
        if (value > 0x7fU) return std::nullopt;
        wide_name.push_back(static_cast<wchar_t>(value));
    }
    wchar_t *value = nullptr;
    std::size_t length = 0U;
    const errno_t error = ::_wdupenv_s(&value, &length, wide_name.c_str());
    if (error != 0 || value == nullptr) {
        std::free(value);
        return std::nullopt;
    }
    std::filesystem::path result{value};
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::filesystem::path{value};
#endif
}

} // namespace getnative

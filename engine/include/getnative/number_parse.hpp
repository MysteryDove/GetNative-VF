#pragma once

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace getnative {

// libc++ versions used by older Xcode releases do not provide floating-point
// std::from_chars. strtod gives the same full-token validation contract here.
inline bool parse_finite_double(std::string_view text, double &value) {
    std::string copy{text};
    char *end = nullptr;
    errno = 0;
    value = std::strtod(copy.c_str(), &end);
    return errno != ERANGE && end == copy.c_str() + copy.size()
        && std::isfinite(value);
}

} // namespace getnative

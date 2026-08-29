#pragma once

#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace getnative {

// libc++ versions used by older Xcode releases do not provide floating-point
// std::from_chars. Parse with the classic locale so worker JSON numbers stay
// independent of LC_NUMERIC (comma-decimal locales would reject "1.25").
inline bool parse_finite_double(std::string_view text, double &value) {
    if (text.empty()) return false;
    std::istringstream stream{std::string{text}};
    stream.imbue(std::locale::classic());
    double parsed = 0.0;
    if (!(stream >> parsed) || !std::isfinite(parsed)) return false;
    stream >> std::ws;
    if (!stream.eof()) return false;
    value = parsed;
    return true;
}

} // namespace getnative

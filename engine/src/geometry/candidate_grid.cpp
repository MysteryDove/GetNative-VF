#include "getnative/candidate_grid.hpp"
#include "getnative/number_parse.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace getnative {
namespace {

struct FixedDecimal {
    std::int64_t units;
    std::uint32_t scale;
};

std::int64_t pow10(std::uint32_t scale) {
    std::int64_t result = 1;
    for (std::uint32_t index = 0; index < scale; ++index) {
        if (result > std::numeric_limits<std::int64_t>::max() / 10) {
            throw std::out_of_range("decimal has too many fractional digits");
        }
        result *= 10;
    }
    return result;
}

FixedDecimal parse_decimal(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("decimal must not be empty");
    }
    bool negative = false;
    std::size_t cursor = 0;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        cursor = 1;
    }
    if (cursor == text.size()) {
        throw std::invalid_argument("invalid decimal");
    }

    std::int64_t units = 0;
    std::uint32_t scale = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    for (; cursor < text.size(); ++cursor) {
        const char ch = text[cursor];
        if (ch == '.' && !seen_dot) {
            seen_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9') {
            throw std::invalid_argument("invalid decimal: " + std::string{text});
        }
        seen_digit = true;
        if (units > (std::numeric_limits<std::int64_t>::max() - 9) / 10) {
            throw std::out_of_range("decimal is outside int64 fixed-point range");
        }
        units = units * 10 + static_cast<std::int64_t>(ch - '0');
        if (seen_dot) {
            ++scale;
        }
    }
    if (!seen_digit) {
        throw std::invalid_argument("invalid decimal");
    }
    return {negative ? -units : units, scale};
}

FixedDecimal rescale(FixedDecimal value, std::uint32_t target_scale) {
    if (value.scale == target_scale) {
        return value;
    }
    const auto multiplier = pow10(target_scale - value.scale);
    if (value.units > std::numeric_limits<std::int64_t>::max() / multiplier ||
        value.units < std::numeric_limits<std::int64_t>::min() / multiplier) {
        throw std::out_of_range("decimal rescaling overflow");
    }
    return {value.units * multiplier, target_scale};
}

std::string format_fixed(std::int64_t units, std::uint32_t scale) {
    const bool negative = units < 0;
    const auto magnitude = negative
        ? static_cast<std::uint64_t>(-(units + 1)) + 1
        : static_cast<std::uint64_t>(units);
    std::string digits = std::to_string(magnitude);
    if (scale > 0) {
        if (digits.size() <= scale) {
            digits.insert(0, static_cast<std::size_t>(scale) + 1 - digits.size(), '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
        while (digits.back() == '0') {
            digits.pop_back();
        }
        if (digits.back() == '.') {
            digits.pop_back();
        }
    }
    if (negative && magnitude != 0) {
        digits.insert(digits.begin(), '-');
    }
    return digits;
}

double parse_double(std::string_view text) {
    double value = 0.0;
    if (!getnative::parse_finite_double(text, value)) {
        throw std::invalid_argument("invalid floating-point decimal: " + std::string{text});
    }
    return value;
}

std::string format_double(double value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

} // namespace

std::vector<Candidate> generate_candidates(const CandidateGridSpec& spec, GridSemantics semantics) {
    std::vector<Candidate> result;
    result.reserve(spec.count);

    if (semantics == GridSemantics::decimal_fixed_point) {
        auto start = parse_decimal(spec.start);
        auto step = parse_decimal(spec.step);
        const auto scale = std::max(start.scale, step.scale);
        start = rescale(start, scale);
        step = rescale(step, scale);
        for (std::size_t index = 0; index < spec.count; ++index) {
            if (index > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::out_of_range("candidate index overflow");
            }
            const auto signed_index = static_cast<std::int64_t>(index);
            if (step.units > 0 && signed_index > std::numeric_limits<std::int64_t>::max() / step.units) {
                throw std::out_of_range("candidate grid multiplication overflow");
            }
            if (step.units < 0) {
                if (step.units == std::numeric_limits<std::int64_t>::min()) {
                    if (signed_index > 1) {
                        throw std::out_of_range("candidate grid multiplication overflow");
                    }
                } else if (signed_index > std::numeric_limits<std::int64_t>::max() / -step.units) {
                    throw std::out_of_range("candidate grid multiplication overflow");
                }
            }
            const auto increment = step.units * signed_index;
            if ((increment > 0 && start.units > std::numeric_limits<std::int64_t>::max() - increment) ||
                (increment < 0 && start.units < std::numeric_limits<std::int64_t>::min() - increment)) {
                throw std::out_of_range("candidate grid overflow");
            }
            const auto units = start.units + increment;
            const auto decimal = format_fixed(units, scale);
            result.push_back({decimal, parse_double(decimal)});
        }
        return result;
    }

    const double start = parse_double(spec.start);
    const double step = parse_double(spec.step);
    double current = start;
    for (std::size_t index = 0; index < spec.count; ++index) {
        const double value = semantics == GridSemantics::repeated_addition
            ? current
            : start + static_cast<double>(index) * step;
        if (!std::isfinite(value)) {
            throw std::out_of_range("candidate grid is outside the finite double range");
        }
        result.push_back({format_double(value), value});
        if (semantics == GridSemantics::repeated_addition && index + 1U < spec.count) {
            current += step;
        }
    }
    return result;
}

std::vector<Candidate> generate_candidate_range(
    const CandidateRangeSpec& spec, GridSemantics semantics) {
    if (spec.maximum_count == 0U) {
        throw std::invalid_argument("candidate maximum_count must be positive");
    }

    std::vector<Candidate> result;
    result.reserve(std::min<std::size_t>(spec.maximum_count, 4096U));

    if (semantics == GridSemantics::decimal_fixed_point) {
        auto start = parse_decimal(spec.start);
        auto stop = parse_decimal(spec.stop);
        auto step = parse_decimal(spec.step);
        const auto scale = std::max({start.scale, stop.scale, step.scale});
        start = rescale(start, scale);
        stop = rescale(stop, scale);
        step = rescale(step, scale);
        if (step.units <= 0) throw std::invalid_argument("candidate step must be positive");
        if (stop.units < start.units) throw std::invalid_argument("candidate stop precedes start");
        for (std::int64_t units = start.units;;) {
            const bool within = spec.endpoint == EndpointRule::inclusive
                ? units <= stop.units : units < stop.units;
            if (!within) break;
            if (result.size() >= spec.maximum_count) {
                throw std::out_of_range("candidate range exceeds maximum_count");
            }
            const std::string decimal = format_fixed(units, scale);
            result.push_back({decimal, parse_double(decimal)});
            if (units > std::numeric_limits<std::int64_t>::max() - step.units) {
                throw std::out_of_range("candidate grid overflow");
            }
            units += step.units;
        }
    } else {
        const double start = parse_double(spec.start);
        const double stop = parse_double(spec.stop);
        const double step = parse_double(spec.step);
        if (!(step > 0.0)) throw std::invalid_argument("candidate step must be positive");
        if (stop < start) throw std::invalid_argument("candidate stop precedes start");
        double current = start;
        for (std::size_t index = 0;; ++index) {
            const double value = semantics == GridSemantics::repeated_addition
                ? current : start + static_cast<double>(index) * step;
            if (!std::isfinite(value)) {
                throw std::out_of_range("candidate grid is outside the finite double range");
            }
            const bool within = spec.endpoint == EndpointRule::inclusive
                ? value <= stop : value < stop;
            if (!within) break;
            if (result.size() >= spec.maximum_count) {
                throw std::out_of_range("candidate range exceeds maximum_count");
            }
            result.push_back({format_double(value), value});
            if (semantics == GridSemantics::repeated_addition) current += step;
        }
    }

    if (result.empty()) throw std::invalid_argument("candidate range is empty");
    return result;
}

} // namespace getnative

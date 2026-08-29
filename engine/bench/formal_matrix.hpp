#pragma once

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace getnative::benchmark::formal {

inline constexpr std::size_t matrix_case_count = 128U;
inline constexpr std::string_view primary_case_id = "bicubic-catrom@810";
inline constexpr std::string_view candidate_contract_id =
    "metal-kernel-matrix-vertical-mirror-v1";

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] JsonValue parse() {
        JsonValue result = parse_value();
        skip_space();
        if (position_ != input_.size()) fail("trailing JSON content");
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::invalid_argument(
            "matrix JSON at byte " + std::to_string(position_) + ": "
            + std::string{message});
    }

    void skip_space() noexcept {
        while (position_ < input_.size()) {
            const char current = input_[position_];
            if (current != ' ' && current != '\t'
                && current != '\r' && current != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        skip_space();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail("unexpected token");
    }

    [[nodiscard]] JsonValue parse_value() {
        skip_space();
        if (position_ >= input_.size()) fail("unexpected end of input");
        switch (input_[position_]) {
        case '{': return JsonValue{parse_object()};
        case '[': return JsonValue{parse_array()};
        case '"': return JsonValue{parse_string()};
        case 't': parse_literal("true"); return JsonValue{true};
        case 'f': parse_literal("false"); return JsonValue{false};
        case 'n': parse_literal("null"); return JsonValue{nullptr};
        default:
            if (input_[position_] == '-'
                || (input_[position_] >= '0' && input_[position_] <= '9')) {
                return JsonValue{parse_number()};
            }
            fail("invalid value");
        }
    }

    [[nodiscard]] JsonValue::Object parse_object() {
        expect('{');
        JsonValue::Object result;
        if (consume('}')) return result;
        while (true) {
            skip_space();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parse_string();
            expect(':');
            if (!result.emplace(std::move(key), parse_value()).second) {
                fail("duplicate object key");
            }
            if (consume('}')) return result;
            expect(',');
        }
    }

    [[nodiscard]] JsonValue::Array parse_array() {
        expect('[');
        JsonValue::Array result;
        if (consume(']')) return result;
        while (true) {
            result.push_back(parse_value());
            if (consume(']')) return result;
            expect(',');
        }
    }

    [[nodiscard]] static unsigned hex_digit(char value) {
        if (value >= '0' && value <= '9') {
            return static_cast<unsigned>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return 10U + static_cast<unsigned>(value - 'a');
        }
        if (value >= 'A' && value <= 'F') {
            return 10U + static_cast<unsigned>(value - 'A');
        }
        throw std::invalid_argument("invalid JSON unicode escape");
    }

    static void append_utf8(std::string &output, unsigned codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(
                0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | (codepoint & 0x3FU)));
        }
    }

    [[nodiscard]] std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const char current = input_[position_++];
            if (current == '"') return result;
            if (static_cast<unsigned char>(current) < 0x20U) {
                fail("control in string");
            }
            if (current != '\\') {
                result.push_back(current);
                continue;
            }
            if (position_ >= input_.size()) fail("unterminated escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (position_ + 4U > input_.size()) {
                    fail("short unicode escape");
                }
                unsigned codepoint = 0U;
                for (int digit = 0; digit < 4; ++digit) {
                    codepoint = codepoint * 16U
                        + hex_digit(input_[position_++]);
                }
                append_utf8(result, codepoint);
                break;
            }
            default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }

    [[nodiscard]] double parse_number() {
        skip_space();
        const std::size_t begin = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) fail("short number");
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                fail("invalid number");
            }
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction_begin) fail("empty fraction");
        }
        if (position_ < input_.size()
            && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent_begin) fail("empty exponent");
        }
        const std::string text{input_.substr(begin, position_ - begin)};
        std::size_t used = 0U;
        const double result = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(result)) {
            fail("non-finite number");
        }
        return result;
    }

    void parse_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
    }

    std::string_view input_;
    std::size_t position_ = 0U;
};

[[nodiscard]] inline const JsonValue::Object &as_object(
    const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<JsonValue::Object>(&value.value);
    if (result == nullptr) {
        throw std::invalid_argument(std::string{name} + " must be an object");
    }
    return *result;
}

[[nodiscard]] inline const JsonValue::Array &as_array(
    const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<JsonValue::Array>(&value.value);
    if (result == nullptr) {
        throw std::invalid_argument(std::string{name} + " must be an array");
    }
    return *result;
}

[[nodiscard]] inline const JsonValue &member(
    const JsonValue::Object &object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end()) {
        throw std::invalid_argument(
            "missing matrix field: " + std::string{name});
    }
    return found->second;
}

[[nodiscard]] inline double as_number(
    const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<double>(&value.value);
    if (result == nullptr) {
        throw std::invalid_argument(std::string{name} + " must be a number");
    }
    return *result;
}

[[nodiscard]] inline std::int32_t as_i32(
    const JsonValue &value, std::string_view name) {
    const double number = as_number(value, name);
    if (number != std::trunc(number)
        || number < static_cast<double>(
            std::numeric_limits<std::int32_t>::min())
        || number > static_cast<double>(
            std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument(std::string{name} + " must fit int32");
    }
    return static_cast<std::int32_t>(number);
}

[[nodiscard]] inline std::string as_string(
    const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<std::string>(&value.value);
    if (result == nullptr) {
        throw std::invalid_argument(std::string{name} + " must be a string");
    }
    return *result;
}

struct NamedFilter {
    std::string id;
    Filter filter;
};

struct Matrix {
    std::int32_t source_width = 0;
    std::int32_t source_height = 0;
    std::int32_t candidate_count = 0;
    std::int32_t tile_size = 0;
    std::int32_t reduction_groups = 0;
    std::int32_t inverse_threads = 0;
    std::int32_t primary_native_height = 0;
    std::string primary_filter_id;
    MetricSpec metric{};
    std::vector<std::int32_t> native_heights;
    std::vector<NamedFilter> filters;
    bool has_fractional_scan = false;
    std::string scan_id;
    std::int32_t scan_native_start = 0;
    std::int32_t scan_native_end = 0;
    double scan_active_start = 0.0;
    double scan_active_end = 0.0;
};

[[nodiscard]] inline Filter parse_filter(const JsonValue::Object &object) {
    const std::string type = as_string(member(object, "type"), "filter.type");
    const auto blur_member = object.find("blur");
    const double blur = blur_member == object.end()
        ? 1.0
        : as_number(blur_member->second, "filter.blur");
    if (type == "bilinear") return Filter::bilinear(blur);
    if (type == "bicubic") {
        return Filter::bicubic(
            as_number(member(object, "b"), "filter.b"),
            as_number(member(object, "c"), "filter.c"), blur);
    }
    if (type == "spline16") return Filter::spline16(blur);
    if (type == "spline36") return Filter::spline36(blur);
    if (type == "spline64") return Filter::spline64(blur);
    if (type == "lanczos") {
        return Filter::lanczos(
            as_i32(member(object, "taps"), "filter.taps"), blur);
    }
    throw std::invalid_argument("unknown matrix filter type: " + type);
}

[[nodiscard]] inline Matrix load_matrix(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open matrix: " + path.string());
    }
    const std::string text{std::istreambuf_iterator<char>{input}, {}};
    const auto root = as_object(JsonParser{text}.parse(), "matrix");
    if (as_i32(member(root, "schema_version"), "schema_version") != 1) {
        throw std::invalid_argument("unsupported matrix schema version");
    }

    Matrix result{};
    const auto &source = as_object(member(root, "source"), "source");
    result.source_width = as_i32(member(source, "width"), "source.width");
    result.source_height = as_i32(member(source, "height"), "source.height");
    result.candidate_count = as_i32(member(root, "candidates"), "candidates");
    result.tile_size = as_i32(member(root, "tile_size"), "tile_size");
    result.reduction_groups = as_i32(
        member(root, "reduction_groups"), "reduction_groups");
    result.inverse_threads = as_i32(
        member(root, "inverse_threads"), "inverse_threads");
    result.primary_native_height = as_i32(
        member(root, "primary_native_height"), "primary_native_height");
    result.primary_filter_id = as_string(
        member(root, "cpu_primary_filter_id"), "cpu_primary_filter_id");

    for (const JsonValue &entry : as_array(
             member(root, "native_heights"), "native_heights")) {
        result.native_heights.push_back(as_i32(entry, "native_height"));
    }

    const auto &metric = as_object(member(root, "metric"), "metric");
    result.metric = {
        as_i32(member(metric, "crop_left"), "metric.crop_left"),
        as_i32(member(metric, "crop_right"), "metric.crop_right"),
        as_i32(member(metric, "crop_top"), "metric.crop_top"),
        as_i32(member(metric, "crop_bottom"), "metric.crop_bottom"),
        static_cast<float>(
            as_number(member(metric, "threshold"), "metric.threshold")),
        1U,
    };

    for (const JsonValue &entry : as_array(
             member(root, "filters"), "filters")) {
        const auto &object = as_object(entry, "filter");
        result.filters.push_back({
            as_string(member(object, "id"), "filter.id"),
            parse_filter(object),
        });
    }

    const auto scan_found = root.find("fractional_scan");
    if (scan_found != root.end()) {
        const auto &scan = as_object(scan_found->second, "fractional_scan");
        result.has_fractional_scan = true;
        result.scan_id = as_string(
            member(scan, "id"), "fractional_scan.id");
        result.scan_native_start = as_i32(
            member(scan, "native_start"), "fractional_scan.native_start");
        result.scan_native_end = as_i32(
            member(scan, "native_end"), "fractional_scan.native_end");
        result.scan_active_start = as_number(
            member(scan, "active_start"), "fractional_scan.active_start");
        result.scan_active_end = as_number(
            member(scan, "active_end"), "fractional_scan.active_end");
    }

    if (result.source_width <= 0 || result.source_height <= 0
        || result.candidate_count <= 0 || result.tile_size <= 0
        || result.reduction_groups <= 0 || result.inverse_threads <= 0
        || result.primary_native_height <= 0
        || result.native_heights.empty() || result.filters.empty()) {
        throw std::invalid_argument("matrix contains invalid dimensions or ranges");
    }
    if (result.has_fractional_scan
        && (result.scan_id.empty() || result.scan_native_start <= 0
            || result.scan_native_end < result.scan_native_start
            || result.scan_active_end < result.scan_active_start)) {
        throw std::invalid_argument("matrix fractional_scan is invalid");
    }
    if (std::any_of(
            result.native_heights.begin(), result.native_heights.end(),
            [](std::int32_t value) { return value <= 0; })) {
        throw std::invalid_argument("matrix native heights must be positive");
    }
    if (std::none_of(
            result.filters.begin(), result.filters.end(),
            [&](const NamedFilter &value) {
                return value.id == result.primary_filter_id;
            })) {
        throw std::invalid_argument("primary filter is absent from matrix");
    }
    return result;
}

struct Case {
    std::string id;
    std::string filter_id;
    Filter filter;
    std::int32_t native_height = 0;
    bool fractional_scan = false;
    bool primary = false;
};

[[nodiscard]] inline std::vector<Case> make_cases(
    const Matrix &matrix,
    const std::optional<std::string> &selected_case = std::nullopt) {
    std::vector<Case> result;
    const std::size_t cases_per_filter = matrix.native_heights.size()
        + (matrix.has_fractional_scan ? 1U : 0U);
    result.reserve(matrix.filters.size() * cases_per_filter);
    for (const NamedFilter &filter : matrix.filters) {
        for (const std::int32_t native_height : matrix.native_heights) {
            result.push_back({
                filter.id + "@" + std::to_string(native_height),
                filter.id,
                filter.filter,
                native_height,
                false,
                filter.id == matrix.primary_filter_id
                    && native_height == matrix.primary_native_height,
            });
        }
        if (matrix.has_fractional_scan) {
            result.push_back({
                filter.id + "@" + matrix.scan_id,
                filter.id,
                filter.filter,
                matrix.scan_native_start,
                true,
                false,
            });
        }
    }

    std::map<std::string, std::size_t, std::less<>> seen;
    for (std::size_t index = 0U; index < result.size(); ++index) {
        if (!seen.emplace(result[index].id, index).second) {
            throw std::invalid_argument(
                "matrix produces duplicate case id: " + result[index].id);
        }
    }
    if (std::count_if(
            result.begin(), result.end(),
            [](const Case &value) { return value.primary; }) != 1) {
        throw std::invalid_argument("matrix must produce exactly one primary case");
    }
    if (selected_case) {
        std::erase_if(result, [&](const Case &value) {
            return value.id != *selected_case;
        });
        if (result.empty()) {
            throw std::invalid_argument(
                "selected matrix case is absent: " + *selected_case);
        }
    }
    return result;
}

[[nodiscard]] inline bool same_filter(
    const Filter &left, const Filter &right) noexcept {
    return left.type == right.type && left.b == right.b
        && left.c == right.c && left.taps == right.taps
        && left.blur == right.blur;
}

[[nodiscard]] inline bool is_formal_matrix(
    const Matrix &matrix, const std::vector<Case> &cases) {
    const std::vector<std::int32_t> expected_heights{
        362, 540, 720, 810, 846, 864, 900,
    };
    const std::vector<std::pair<std::string_view, Filter>> expected_filters{
        {"bilinear", Filter::bilinear()},
        {"bicubic-catrom", Filter::bicubic(0.0, 0.5)},
        {"bicubic-mitchell", Filter::bicubic(1.0 / 3.0, 1.0 / 3.0)},
        {"spline16", Filter::spline16()},
        {"spline36", Filter::spline36()},
        {"spline64", Filter::spline64()},
        {"spline64-blur125", Filter::spline64(1.25)},
        {"spline64-blur150", Filter::spline64(1.5)},
        {"lanczos1", Filter::lanczos(1)},
        {"lanczos2", Filter::lanczos(2)},
        {"lanczos3", Filter::lanczos(3)},
        {"lanczos4", Filter::lanczos(4)},
        {"lanczos5", Filter::lanczos(5)},
        {"lanczos6", Filter::lanczos(6)},
        {"lanczos7", Filter::lanczos(7)},
        {"lanczos8", Filter::lanczos(8)},
    };
    if (matrix.source_width != 1920 || matrix.source_height != 1080
        || matrix.candidate_count != 1000 || matrix.tile_size != 32
        || matrix.reduction_groups != 8 || matrix.inverse_threads != 32
        || matrix.primary_filter_id != "bicubic-catrom"
        || matrix.primary_native_height != 810
        || matrix.native_heights != expected_heights
        || matrix.filters.size() != expected_filters.size()
        || matrix.scan_id != "800-899.9"
        || matrix.scan_native_start != 800 || matrix.scan_native_end != 899
        || matrix.scan_active_start != 800.0 || matrix.scan_active_end != 899.9
        || matrix.metric.crop_left != 5 || matrix.metric.crop_right != 5
        || matrix.metric.crop_top != 5 || matrix.metric.crop_bottom != 5
        || std::bit_cast<std::uint32_t>(matrix.metric.threshold)
            != std::bit_cast<std::uint32_t>(0.015F)
        || matrix.metric.norm != 1U || cases.size() != matrix_case_count) {
        return false;
    }
    for (std::size_t index = 0U; index < expected_filters.size(); ++index) {
        if (matrix.filters[index].id != expected_filters[index].first
            || !same_filter(
                matrix.filters[index].filter, expected_filters[index].second)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline float source_value(
    std::int32_t row, std::int32_t column) noexcept {
    return static_cast<float>(
        0.41
        + 0.19 * std::sin(0.013 * static_cast<double>(row))
        + 0.23 * std::cos(0.017 * static_cast<double>(column))
        + 0.07 * std::sin(0.007 * static_cast<double>(row + column)));
}

[[nodiscard]] inline std::vector<float> make_source(const Matrix &matrix) {
    std::vector<float> result(
        static_cast<std::size_t>(matrix.source_width)
        * static_cast<std::size_t>(matrix.source_height));
    for (std::int32_t row = 0; row < matrix.source_height; ++row) {
        for (std::int32_t column = 0; column < matrix.source_width; ++column) {
            result[static_cast<std::size_t>(row * matrix.source_width + column)] =
                source_value(row, column);
        }
    }
    return result;
}

struct CandidatePoint {
    std::int32_t native_height = 0;
    double active_height = 0.0;
};

[[nodiscard]] inline CandidatePoint candidate_point(
    const Matrix &matrix, const Case &benchmark_case, std::size_t index) {
    CandidatePoint result{
        benchmark_case.native_height,
        static_cast<double>(benchmark_case.native_height)
            + static_cast<double>(index + 1U)
                / (static_cast<double>(matrix.candidate_count) + 1.0),
    };
    if (benchmark_case.fractional_scan) {
        if (!matrix.has_fractional_scan) {
            throw std::invalid_argument(
                "fractional scan case requires matrix.fractional_scan");
        }
        const double position = matrix.candidate_count == 1
            ? 0.0
            : static_cast<double>(index)
                / static_cast<double>(matrix.candidate_count - 1);
        result.active_height = matrix.scan_active_start
            + position * (matrix.scan_active_end - matrix.scan_active_start);
        result.native_height = std::clamp(
            static_cast<std::int32_t>(std::floor(result.active_height)),
            matrix.scan_native_start, matrix.scan_native_end);
    }
    return result;
}

[[nodiscard]] inline std::vector<AxisPlanRequest> make_requests(
    const Matrix &matrix, const Case &benchmark_case) {
    std::vector<AxisPlanRequest> result;
    result.reserve(static_cast<std::size_t>(matrix.candidate_count));
    for (std::size_t index = 0U;
         index < static_cast<std::size_t>(matrix.candidate_count); ++index) {
        const CandidatePoint point = candidate_point(matrix, benchmark_case, index);
        result.push_back({
            matrix.source_height,
            point.native_height,
            point.active_height,
            0.0,
            benchmark_case.filter,
            BorderMode::mirror,
        });
    }
    return result;
}

[[nodiscard]] inline std::vector<CandidateAnalysis> make_candidates(
    const std::vector<std::shared_ptr<const AxisPlan>> &plans) {
    std::vector<CandidateAnalysis> result;
    result.reserve(plans.size());
    for (std::size_t index = 0U; index < plans.size(); ++index) {
        result.push_back({
            "candidate-" + std::to_string(index),
            nullptr,
            plans[index],
            AnalysisAxes::vertical,
        });
    }
    return result;
}

struct PreparedCase {
    std::vector<AxisPlanRequest> requests;
    std::vector<std::shared_ptr<const AxisPlan>> plans;
    std::vector<CandidateAnalysis> candidates;
};

[[nodiscard]] inline PreparedCase prepare_case(
    const Matrix &matrix, const Case &benchmark_case) {
    PreparedCase result{};
    result.requests = make_requests(matrix, benchmark_case);
    AxisPlanCache cache;
    result.plans = cache.get_or_build_batch(result.requests).plans;
    result.candidates = make_candidates(result.plans);
    return result;
}

inline void fnv1a64_append(
    std::uint64_t &hash, std::uint64_t value, unsigned bytes) noexcept {
    for (unsigned index = 0U; index < bytes; ++index) {
        hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
        hash *= 1099511628211ULL;
    }
}

inline void fnv1a64_append(
    std::uint64_t &hash, std::string_view value) noexcept {
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    fnv1a64_append(hash, 0U, 1U);
}

[[nodiscard]] inline std::string hexadecimal_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

[[nodiscard]] inline std::string source_f32_fnv1a64(
    const std::vector<float> &values) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float value : values) {
        fnv1a64_append(hash, std::bit_cast<std::uint32_t>(value), 4U);
    }
    return hexadecimal_u64(hash);
}

[[nodiscard]] inline std::string candidate_contract_fingerprint(
    const Matrix &matrix, const Case &benchmark_case) {
    std::uint64_t hash = 1469598103934665603ULL;
    fnv1a64_append(hash, candidate_contract_id);
    fnv1a64_append(hash, benchmark_case.id);
    fnv1a64_append(hash, benchmark_case.filter_id);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.source_width), 4U);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.source_height), 4U);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.candidate_count), 4U);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.metric.crop_left), 4U);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.metric.crop_right), 4U);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.metric.crop_top), 4U);
    fnv1a64_append(hash, static_cast<std::uint32_t>(matrix.metric.crop_bottom), 4U);
    fnv1a64_append(
        hash, std::bit_cast<std::uint32_t>(matrix.metric.threshold), 4U);
    fnv1a64_append(hash, matrix.metric.norm, 4U);
    fnv1a64_append(hash, benchmark_case.fractional_scan ? 1U : 0U, 1U);
    for (std::size_t index = 0U;
         index < static_cast<std::size_t>(matrix.candidate_count); ++index) {
        const CandidatePoint point = candidate_point(matrix, benchmark_case, index);
        fnv1a64_append(
            hash, static_cast<std::uint32_t>(point.native_height), 4U);
        fnv1a64_append(
            hash, std::bit_cast<std::uint64_t>(point.active_height), 8U);
    }
    return hexadecimal_u64(hash);
}

} // namespace getnative::benchmark::formal

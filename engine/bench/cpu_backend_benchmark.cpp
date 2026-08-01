#include "benchmark_support.hpp"
#include "inverse_columns.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/cpu_features.hpp"
#include "getnative/filter.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
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

#ifndef GETNATIVE_BENCHMARK_PLANNER_FP_MODE
#define GETNATIVE_BENCHMARK_PLANNER_FP_MODE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t formal_matrix_case_count = 112U;
constexpr std::string_view formal_primary_case_id = "bicubic-catrom@810";
constexpr std::string_view candidate_contract_id =
    "metal-kernel-matrix-vertical-mirror-v1";
constexpr std::string_view plan_content_contract_id =
    "getnative-axis-plan-binary-v1";

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
            if (current != ' ' && current != '\t' && current != '\r' && current != '\n') {
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
            if (input_[position_] == '-' || (input_[position_] >= '0'
                                              && input_[position_] <= '9')) {
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
        if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f') return 10U + static_cast<unsigned>(value - 'a');
        if (value >= 'A' && value <= 'F') return 10U + static_cast<unsigned>(value - 'A');
        throw std::invalid_argument("invalid JSON unicode escape");
    }

    void append_utf8(std::string &output, unsigned codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    [[nodiscard]] std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const char current = input_[position_++];
            if (current == '"') return result;
            if (static_cast<unsigned char>(current) < 0x20U) fail("control in string");
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
                if (position_ + 4U > input_.size()) fail("short unicode escape");
                unsigned codepoint = 0;
                for (int digit = 0; digit < 4; ++digit) {
                    codepoint = codepoint * 16U + hex_digit(input_[position_++]);
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
            if (input_[position_] < '1' || input_[position_] > '9') fail("invalid number");
            while (position_ < input_.size() && input_[position_] >= '0'
                   && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0'
                   && input_[position_] <= '9') ++position_;
            if (position_ == fraction_begin) fail("empty fraction");
        }
        if (position_ < input_.size()
            && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0'
                   && input_[position_] <= '9') ++position_;
            if (position_ == exponent_begin) fail("empty exponent");
        }
        const std::string text{input_.substr(begin, position_ - begin)};
        std::size_t used = 0;
        const double result = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(result)) fail("non-finite number");
        return result;
    }

    void parse_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) fail("invalid literal");
        position_ += literal.size();
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

[[nodiscard]] const JsonValue::Object &as_object(
    const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<JsonValue::Object>(&value.value);
    if (!result) throw std::invalid_argument(std::string{name} + " must be an object");
    return *result;
}

[[nodiscard]] const JsonValue::Array &as_array(
    const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<JsonValue::Array>(&value.value);
    if (!result) throw std::invalid_argument(std::string{name} + " must be an array");
    return *result;
}

[[nodiscard]] const JsonValue &member(
    const JsonValue::Object &object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end()) throw std::invalid_argument("missing matrix field: " + std::string{name});
    return found->second;
}

[[nodiscard]] double as_number(const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<double>(&value.value);
    if (!result) throw std::invalid_argument(std::string{name} + " must be a number");
    return *result;
}

[[nodiscard]] std::int32_t as_i32(const JsonValue &value, std::string_view name) {
    const double number = as_number(value, name);
    if (number != std::trunc(number)
        || number < static_cast<double>(std::numeric_limits<std::int32_t>::min())
        || number > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument(std::string{name} + " must fit int32");
    }
    return static_cast<std::int32_t>(number);
}

[[nodiscard]] std::string as_string(const JsonValue &value, std::string_view name) {
    const auto *result = std::get_if<std::string>(&value.value);
    if (!result) throw std::invalid_argument(std::string{name} + " must be a string");
    return *result;
}

struct NamedFilter {
    std::string id;
    getnative::Filter filter;
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
    getnative::MetricSpec metric{};
    std::vector<std::int32_t> native_heights;
    std::vector<NamedFilter> filters;
    bool has_fractional_scan = false;
    std::string scan_id;
    std::int32_t scan_native_start = 0;
    std::int32_t scan_native_end = 0;
    double scan_active_start = 0.0;
    double scan_active_end = 0.0;
};

[[nodiscard]] getnative::Filter parse_filter(const JsonValue::Object &object) {
    const std::string type = as_string(member(object, "type"), "filter.type");
    if (type == "bilinear") return getnative::Filter::bilinear();
    if (type == "bicubic") {
        return getnative::Filter::bicubic(
            as_number(member(object, "b"), "filter.b"),
            as_number(member(object, "c"), "filter.c"));
    }
    if (type == "spline16") return getnative::Filter::spline16();
    if (type == "spline36") return getnative::Filter::spline36();
    if (type == "spline64") return getnative::Filter::spline64();
    if (type == "lanczos") {
        return getnative::Filter::lanczos(as_i32(member(object, "taps"), "filter.taps"));
    }
    throw std::invalid_argument("unknown matrix filter type: " + type);
}

[[nodiscard]] Matrix load_matrix(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open matrix: " + path.string());
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
    for (const JsonValue &entry : as_array(member(root, "native_heights"), "native_heights")) {
        result.native_heights.push_back(as_i32(entry, "native_height"));
    }
    const auto &metric = as_object(member(root, "metric"), "metric");
    result.metric = {
        as_i32(member(metric, "crop_left"), "metric.crop_left"),
        as_i32(member(metric, "crop_right"), "metric.crop_right"),
        as_i32(member(metric, "crop_top"), "metric.crop_top"),
        as_i32(member(metric, "crop_bottom"), "metric.crop_bottom"),
        static_cast<float>(as_number(member(metric, "threshold"), "metric.threshold")),
        1U,
    };
    for (const JsonValue &entry : as_array(member(root, "filters"), "filters")) {
        const auto &object = as_object(entry, "filter");
        result.filters.push_back({
            as_string(member(object, "id"), "filter.id"), parse_filter(object),
        });
    }
    const auto scan_found = root.find("fractional_scan");
    if (scan_found != root.end()) {
        const auto &scan = as_object(scan_found->second, "fractional_scan");
        result.has_fractional_scan = true;
        result.scan_id = as_string(member(scan, "id"), "fractional_scan.id");
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
    if (std::any_of(result.native_heights.begin(), result.native_heights.end(),
                    [](std::int32_t height) { return height <= 0; })) {
        throw std::invalid_argument("matrix native heights must be positive");
    }
    if (std::none_of(result.filters.begin(), result.filters.end(), [&](const auto &filter) {
            return filter.id == result.primary_filter_id;
        })) {
        throw std::invalid_argument("primary CPU filter is absent from matrix");
    }
    return result;
}

struct Configuration {
    std::filesystem::path matrix_path;
    std::filesystem::path artifact_root;
    std::filesystem::path plan_dump_path;
    int samples = 21;
    bool assert_correctness = false;
    bool all_isa = false;
    bool list_cases = false;
    bool planner_only = false;
    getnative::CpuIsaRequest request = getnative::CpuIsaRequest::automatic;
    std::optional<std::string> selected_case;
};

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--matrix" && index + 1 < argc) {
            result.matrix_path = argv[++index];
        } else if (argument == "--artifact-root" && index + 1 < argc) {
            result.artifact_root = argv[++index];
        } else if (argument == "--plan-dump" && index + 1 < argc) {
            result.plan_dump_path = argv[++index];
        } else if (argument == "--samples" && index + 1 < argc) {
            result.samples = std::stoi(argv[++index]);
        } else if (argument == "--cpu-isa" && index + 1 < argc) {
            const std::string_view value{argv[++index]};
            if (value == "all") {
                result.all_isa = true;
            } else {
                const auto parsed = getnative::parse_cpu_isa_request(value);
                if (!parsed) throw std::invalid_argument("unknown --cpu-isa value");
                result.all_isa = false;
                result.request = *parsed;
            }
        } else if (argument == "--case" && index + 1 < argc) {
            result.selected_case = argv[++index];
        } else if (argument == "--list-cases") {
            result.list_cases = true;
        } else if (argument == "--planner-only") {
            result.planner_only = true;
        } else if (argument == "--assert") {
            result.assert_correctness = true;
        } else if (argument == "--help") {
            std::cout << "usage: getnative_cpu_backend_benchmark --matrix PATH "
                         "[--artifact-root PATH] "
                         "[--plan-dump PATH] "
                         "[--cpu-isa all|auto|scalar|sse2|avx2|avx512] "
                         "[--case ID] [--list-cases] [--planner-only] "
                         "[--samples N] [--assert]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + std::string{argument});
        }
    }
    if (result.matrix_path.empty()) {
        throw std::invalid_argument("--matrix is required");
    }
    if (!result.list_cases && result.artifact_root.empty()) {
        throw std::invalid_argument("--artifact-root is required for measurement");
    }
    if (!result.list_cases && result.samples < 21) {
        throw std::invalid_argument("CPU backend benchmark requires at least 21 samples");
    }
    return result;
}

[[nodiscard]] float source_value(std::int32_t row, std::int32_t column) noexcept {
    return static_cast<float>(
        0.41 + 0.19 * std::sin(0.013 * static_cast<double>(row))
        + 0.23 * std::cos(0.017 * static_cast<double>(column))
        + 0.07 * std::sin(0.007 * static_cast<double>(row + column)));
}

[[nodiscard]] std::vector<float> make_source(const Matrix &matrix) {
    std::vector<float> result(
        static_cast<std::size_t>(matrix.source_width)
        * static_cast<std::size_t>(matrix.source_height));
    for (std::int32_t y = 0; y < matrix.source_height; ++y) {
        for (std::int32_t x = 0; x < matrix.source_width; ++x) {
            result[static_cast<std::size_t>(y * matrix.source_width + x)] =
                source_value(y, x);
        }
    }
    return result;
}

struct MatrixCase {
    std::string id;
    std::string filter_id;
    getnative::Filter filter;
    std::int32_t native_height = 0;
    bool fractional_scan = false;
    bool primary = false;
};

[[nodiscard]] std::vector<MatrixCase> make_matrix_cases(
    const Matrix &matrix, const std::optional<std::string> &selected_case) {
    std::vector<MatrixCase> result;
    const std::size_t cases_per_filter = matrix.native_heights.size()
        + (matrix.has_fractional_scan ? 1U : 0U);
    result.reserve(matrix.filters.size() * cases_per_filter);
    for (const NamedFilter &filter : matrix.filters) {
        for (const std::int32_t native_height : matrix.native_heights) {
            const std::string id = filter.id + "@" + std::to_string(native_height);
            result.push_back({
                id,
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
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (!seen.emplace(result[index].id, index).second) {
            throw std::invalid_argument("matrix produces a duplicate case id: " + result[index].id);
        }
    }
    if (std::count_if(result.begin(), result.end(),
                      [](const MatrixCase &value) { return value.primary; }) != 1) {
        throw std::invalid_argument("matrix must produce exactly one primary CPU case");
    }
    if (selected_case) {
        std::erase_if(result, [&](const MatrixCase &value) {
            return value.id != *selected_case;
        });
        if (result.empty()) {
            throw std::invalid_argument("selected matrix case is absent: " + *selected_case);
        }
    }
    return result;
}

[[nodiscard]] bool is_formal_matrix(
    const Matrix &matrix, const std::vector<MatrixCase> &cases) {
    const std::vector<std::int32_t> expected_heights{
        362, 540, 720, 810, 846, 864, 900,
    };
    const std::vector<std::pair<std::string_view, getnative::Filter>> expected_filters{
        {"bilinear", getnative::Filter::bilinear()},
        {"bicubic-catrom", getnative::Filter::bicubic(0.0, 0.5)},
        {"bicubic-mitchell", getnative::Filter::bicubic(1.0 / 3.0, 1.0 / 3.0)},
        {"spline16", getnative::Filter::spline16()},
        {"spline36", getnative::Filter::spline36()},
        {"spline64", getnative::Filter::spline64()},
        {"lanczos1", getnative::Filter::lanczos(1)},
        {"lanczos2", getnative::Filter::lanczos(2)},
        {"lanczos3", getnative::Filter::lanczos(3)},
        {"lanczos4", getnative::Filter::lanczos(4)},
        {"lanczos5", getnative::Filter::lanczos(5)},
        {"lanczos6", getnative::Filter::lanczos(6)},
        {"lanczos7", getnative::Filter::lanczos(7)},
        {"lanczos8", getnative::Filter::lanczos(8)},
    };
    if (matrix.source_width != 1920 || matrix.source_height != 1080
        || matrix.candidate_count != 1000
        || matrix.tile_size != 32 || matrix.reduction_groups != 8
        || matrix.inverse_threads != 32
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
        || matrix.metric.norm != 1U || cases.size() != formal_matrix_case_count) {
        return false;
    }
    for (std::size_t index = 0; index < expected_filters.size(); ++index) {
        const NamedFilter &actual = matrix.filters[index];
        const auto &[expected_id, expected_filter] = expected_filters[index];
        if (actual.id != expected_id || actual.filter.type != expected_filter.type
            || actual.filter.b != expected_filter.b
            || actual.filter.c != expected_filter.c
            || actual.filter.taps != expected_filter.taps) {
            return false;
        }
    }
    return true;
}

struct CandidatePoint {
    std::int32_t native_height = 0;
    double active_height = 0.0;
};

[[nodiscard]] CandidatePoint candidate_point(
    const Matrix &matrix, const MatrixCase &benchmark_case, std::size_t index) {
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

[[nodiscard]] std::vector<getnative::AxisPlanRequest> make_requests(
    const Matrix &matrix, const MatrixCase &benchmark_case) {
    std::vector<getnative::AxisPlanRequest> result;
    result.reserve(static_cast<std::size_t>(matrix.candidate_count));
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(matrix.candidate_count); ++index) {
        const CandidatePoint point = candidate_point(matrix, benchmark_case, index);
        result.push_back({
            matrix.source_height,
            point.native_height,
            point.active_height,
            0.0,
            benchmark_case.filter,
            getnative::BorderMode::mirror,
        });
    }
    return result;
}

[[nodiscard]] std::string source_f32_fnv1a64(const std::vector<float> &values) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float value : values) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>(bits >> shift);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

void fnv1a64_append(std::uint64_t &hash, std::uint64_t value, unsigned bytes) noexcept {
    for (unsigned index = 0; index < bytes; ++index) {
        hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
        hash *= 1099511628211ULL;
    }
}

void fnv1a64_append(std::uint64_t &hash, std::string_view value) noexcept {
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    fnv1a64_append(hash, 0U, 1U);
}

[[nodiscard]] std::string hexadecimal_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

[[nodiscard]] std::string candidate_contract_fingerprint(
    const Matrix &matrix, const MatrixCase &benchmark_case) {
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
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(matrix.candidate_count); ++index) {
        const CandidatePoint point = candidate_point(matrix, benchmark_case, index);
        fnv1a64_append(hash, static_cast<std::uint32_t>(point.native_height), 4U);
        fnv1a64_append(hash, std::bit_cast<std::uint64_t>(point.active_height), 8U);
    }
    return hexadecimal_u64(hash);
}

void plan_bytes_append_u64(
    std::vector<std::uint8_t> &output, std::uint64_t value) {
    for (unsigned index = 0; index < 8U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void plan_bytes_append_u32(
    std::vector<std::uint8_t> &output, std::uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void plan_bytes_append_vector(
    std::vector<std::uint8_t> &output,
    const std::vector<std::uint32_t> &values) {
    plan_bytes_append_u64(output, static_cast<std::uint64_t>(values.size()));
    for (const std::uint32_t value : values) plan_bytes_append_u32(output, value);
}

void plan_bytes_append_vector(
    std::vector<std::uint8_t> &output,
    const std::vector<std::int32_t> &values) {
    plan_bytes_append_u64(output, static_cast<std::uint64_t>(values.size()));
    for (const std::int32_t value : values) {
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(value));
    }
}

void plan_bytes_append_vector(
    std::vector<std::uint8_t> &output,
    const std::vector<float> &values) {
    plan_bytes_append_u64(output, static_cast<std::uint64_t>(values.size()));
    for (const float value : values) {
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(value));
    }
}

[[nodiscard]] std::vector<std::uint8_t> serialize_axis_plans(
    const std::vector<std::shared_ptr<const getnative::AxisPlan>> &plans) {
    std::vector<std::uint8_t> output;
    for (const char character : plan_content_contract_id) {
        output.push_back(static_cast<std::uint8_t>(character));
    }
    output.push_back(0U);
    plan_bytes_append_u64(output, static_cast<std::uint64_t>(plans.size()));
    for (const auto &plan_pointer : plans) {
        if (!plan_pointer) throw std::runtime_error("cannot serialize a null AxisPlan");
        const getnative::AxisPlan &plan = *plan_pointer;
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(plan.source_size));
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(plan.destination_size));
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(plan.support));
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(plan.half_bandwidth));
        plan_bytes_append_u32(output, std::bit_cast<std::uint32_t>(plan.forward_width));
        plan_bytes_append_u64(output, std::bit_cast<std::uint64_t>(plan.active_length));
        plan_bytes_append_u64(output, std::bit_cast<std::uint64_t>(plan.shift));
        plan_bytes_append_vector(output, plan.forward_offsets);
        plan_bytes_append_vector(output, plan.forward_indices);
        plan_bytes_append_vector(output, plan.forward_weights);
        plan_bytes_append_vector(output, plan.transpose_offsets);
        plan_bytes_append_vector(output, plan.transpose_indices);
        plan_bytes_append_vector(output, plan.transpose_weights);
        plan_bytes_append_vector(output, plan.lower_ld);
        plan_bytes_append_vector(output, plan.upper_l);
        plan_bytes_append_vector(output, plan.inverse_diagonal);
    }
    return output;
}

[[nodiscard]] std::string plan_content_fingerprint(
    const std::vector<std::uint8_t> &content) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t value : content) fnv1a64_append(hash, value, 1U);
    return hexadecimal_u64(hash);
}

void write_plan_dump(
    const std::filesystem::path &path,
    const std::vector<std::uint8_t> &content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to open plan dump: " + path.string());
    output.write(
        reinterpret_cast<const char *>(content.data()),
        static_cast<std::streamsize>(content.size()));
    if (!output) throw std::runtime_error("failed to write plan dump: " + path.string());
}

[[nodiscard]] std::vector<getnative::CandidateAnalysis> make_candidates(
    const std::vector<std::shared_ptr<const getnative::AxisPlan>> &plans) {
    std::vector<getnative::CandidateAnalysis> result;
    result.reserve(plans.size());
    for (std::size_t index = 0; index < plans.size(); ++index) {
        result.push_back({
            "candidate-" + std::to_string(index), nullptr, plans[index],
            getnative::AnalysisAxes::vertical,
        });
    }
    return result;
}

std::atomic<std::uint64_t> benchmark_sink{0};

template <class Function>
[[nodiscard]] double elapsed_milliseconds(Function &&function) {
    const auto begin = Clock::now();
    const std::uint64_t checksum = static_cast<std::uint64_t>(function());
    const auto end = Clock::now();
    benchmark_sink.fetch_xor(checksum, std::memory_order_relaxed);
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class Function>
[[nodiscard]] getnative::benchmark::Summary measure(int samples, Function &&function) {
    (void)function();
    std::vector<double> raw;
    raw.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        raw.push_back(elapsed_milliseconds(function));
    }
    return getnative::benchmark::summarize(std::move(raw));
}

[[nodiscard]] std::uint64_t result_checksum(
    const std::vector<getnative::CandidateResult> &results) noexcept {
    std::uint64_t checksum = 0;
    for (const auto &result : results) checksum ^= std::bit_cast<std::uint64_t>(result.error);
    return checksum;
}

struct PreparedCase {
    std::vector<getnative::AxisPlanRequest> requests;
    std::vector<std::shared_ptr<const getnative::AxisPlan>> plans;
    std::vector<getnative::CandidateAnalysis> candidates;
    std::vector<std::uint8_t> plan_content;
};

[[nodiscard]] PreparedCase prepare_case(
    const Matrix &matrix, const MatrixCase &benchmark_case) {
    PreparedCase result{};
    result.requests = make_requests(matrix, benchmark_case);
    getnative::AxisPlanCache cache;
    result.plans = cache.get_or_build_batch(result.requests).plans;
    result.candidates = make_candidates(result.plans);
    result.plan_content = serialize_axis_plans(result.plans);
    return result;
}

struct P0Case {
    std::string id;
    getnative::AxisPlan plan;
    std::int32_t columns = 0;
    std::ptrdiff_t input_stride = 0;
    std::ptrdiff_t output_stride = 0;
    std::size_t base_offset = 0;
    std::vector<float> input;
    std::vector<float> output;
    std::vector<float> scalar;
};

[[nodiscard]] std::vector<P0Case> make_p0_cases() {
    const std::vector<std::pair<std::string_view, getnative::Filter>> shapes{
        {"b3", getnative::Filter::bilinear()},
        {"b7", getnative::Filter::bicubic()},
        {"b11", getnative::Filter::spline36()},
        {"b15", getnative::Filter::spline64()},
        {"generic-lanczos8", getnative::Filter::lanczos(8)},
        {"generic-lanczos15", getnative::Filter::lanczos(15)},
    };
    const std::vector<std::int32_t> widths{
        3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 1919, 1920, 1921,
    };
    std::vector<P0Case> result;
    for (const auto &[shape, filter] : shapes) {
        const auto plan = getnative::build_axis_plan({
            79, 43, 43.25, -0.1875, filter, getnative::BorderMode::mirror,
        });
        for (const std::int32_t columns : widths) {
            P0Case current{};
            current.id = std::string{shape} + "-columns-" + std::to_string(columns);
            current.plan = plan;
            current.columns = columns;
            current.input_stride = columns + 3;
            current.output_stride = columns + 5;
            current.base_offset = 1U;
            current.input.assign(
                static_cast<std::size_t>(79 * current.input_stride) + current.base_offset,
                -91.0F);
            current.output.assign(
                static_cast<std::size_t>(43 * current.output_stride) + current.base_offset,
                -93.0F);
            current.scalar = current.output;
            for (std::int32_t row = 0; row < 79; ++row) {
                for (std::int32_t column = 0; column < columns; ++column) {
                    current.input[current.base_offset
                                  + static_cast<std::size_t>(row * current.input_stride + column)] =
                        source_value(row, column);
                }
            }
            getnative::detail::inverse_columns_f32(
                current.plan, current.input.data() + current.base_offset,
                current.input_stride, current.scalar.data() + current.base_offset,
                current.output_stride, current.columns,
                getnative::detail::ColumnDispatchPolicy::scalar_only);
            result.push_back(std::move(current));
        }
    }
    return result;
}

struct P0Measurement {
    std::string id;
    getnative::benchmark::Summary milliseconds;
    bool within_tolerance = false;
    bool bit_identical = false;
};

enum class P1Kind : std::uint8_t {
    absolute_difference,
    vertical_reconstruction,
};

struct P1Case {
    std::string id;
    P1Kind kind = P1Kind::absolute_difference;
    getnative::AxisPlan plan;
    std::uint32_t begin = 0;
    std::int32_t left = 0;
    std::int32_t width = 0;
    std::ptrdiff_t native_stride = 0;
    std::vector<float> source;
    std::vector<float> reconstruction;
    std::vector<float> native;
    std::vector<float> differences;
    std::vector<float> scalar;
};

void run_p1_case(
    P1Case &benchmark_case, getnative::detail::ColumnDispatchPolicy policy) {
    getnative::detail::validate_column_dispatch_policy(policy);
    const getnative::detail::AnalysisRowDispatch dispatch =
        getnative::detail::analysis_row_dispatch(policy);
    std::int32_t x = 0;
    if (benchmark_case.kind == P1Kind::absolute_difference) {
        if (dispatch.absolute_difference != nullptr) {
            for (; x <= benchmark_case.width - dispatch.lanes; x += dispatch.lanes) {
                dispatch.absolute_difference(
                    benchmark_case.source.data() + x,
                    benchmark_case.reconstruction.data() + x,
                    benchmark_case.differences.data() + x);
            }
        }
        for (; x < benchmark_case.width; ++x) {
            benchmark_case.differences[static_cast<std::size_t>(x)] = std::abs(
                benchmark_case.source[static_cast<std::size_t>(x)]
                - benchmark_case.reconstruction[static_cast<std::size_t>(x)]);
        }
        return;
    }

    if (dispatch.vertical_reconstruction != nullptr) {
        for (; x <= benchmark_case.width - dispatch.lanes; x += dispatch.lanes) {
            dispatch.vertical_reconstruction(
                benchmark_case.plan, benchmark_case.begin, benchmark_case.left,
                benchmark_case.source.data(), benchmark_case.native.data(),
                benchmark_case.native_stride, x,
                benchmark_case.differences.data() + x);
        }
    }
    for (; x < benchmark_case.width; ++x) {
        float reconstructed = 0.0F;
        for (std::int32_t tap = 0; tap < benchmark_case.plan.forward_width; ++tap) {
            reconstructed += benchmark_case.plan.forward_weights[
                benchmark_case.begin + static_cast<std::uint32_t>(tap)]
                * benchmark_case.native[
                    static_cast<std::size_t>(benchmark_case.left + tap)
                        * static_cast<std::size_t>(benchmark_case.native_stride)
                    + static_cast<std::size_t>(x)];
        }
        benchmark_case.differences[static_cast<std::size_t>(x)] = std::abs(
            benchmark_case.source[static_cast<std::size_t>(x)] - reconstructed);
    }
}

[[nodiscard]] std::vector<P1Case> make_p1_cases() {
    const std::vector<std::int32_t> widths{
        3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 1919, 1920, 1921,
    };
    const std::vector<std::pair<std::string_view, getnative::Filter>> shapes{
        {"b3", getnative::Filter::bilinear()},
        {"b7", getnative::Filter::bicubic()},
        {"b11", getnative::Filter::spline36()},
        {"b15", getnative::Filter::spline64()},
        {"generic-lanczos8", getnative::Filter::lanczos(8)},
        {"generic-lanczos15", getnative::Filter::lanczos(15)},
    };
    std::vector<P1Case> result;
    for (const std::int32_t width : widths) {
        P1Case absolute{};
        absolute.id = "absolute-difference-width-" + std::to_string(width);
        absolute.kind = P1Kind::absolute_difference;
        absolute.width = width;
        absolute.source.resize(static_cast<std::size_t>(width));
        absolute.reconstruction.resize(static_cast<std::size_t>(width));
        absolute.differences.resize(static_cast<std::size_t>(width));
        for (std::int32_t x = 0; x < width; ++x) {
            absolute.source[static_cast<std::size_t>(x)] = source_value(17, x);
            absolute.reconstruction[static_cast<std::size_t>(x)] =
                source_value(19, x + 1);
        }
        run_p1_case(absolute, getnative::detail::ColumnDispatchPolicy::scalar_only);
        absolute.scalar = absolute.differences;
        result.push_back(std::move(absolute));

        for (const auto &[shape, filter] : shapes) {
            P1Case vertical{};
            vertical.id = std::string{"vertical-reconstruction-"} + std::string{shape}
                + "-width-" + std::to_string(width);
            vertical.kind = P1Kind::vertical_reconstruction;
            vertical.plan = getnative::build_axis_plan({
                79, 43, 43.25, -0.1875, filter, getnative::BorderMode::mirror,
            });
            const std::int32_t row = vertical.plan.source_size / 2;
            vertical.begin = vertical.plan.forward_offsets[static_cast<std::size_t>(row)];
            vertical.left = vertical.plan.forward_indices[vertical.begin];
            vertical.width = width;
            vertical.native_stride = width + 3;
            vertical.source.resize(static_cast<std::size_t>(width));
            vertical.native.resize(
                static_cast<std::size_t>(vertical.plan.destination_size)
                * static_cast<std::size_t>(vertical.native_stride), -17.0F);
            vertical.differences.resize(static_cast<std::size_t>(width));
            for (std::int32_t x = 0; x < width; ++x) {
                vertical.source[static_cast<std::size_t>(x)] = source_value(row, x);
            }
            for (std::int32_t y = 0; y < vertical.plan.destination_size; ++y) {
                for (std::int32_t x = 0; x < width; ++x) {
                    vertical.native[static_cast<std::size_t>(y)
                                        * static_cast<std::size_t>(vertical.native_stride)
                                    + static_cast<std::size_t>(x)] = source_value(y + 3, x);
                }
            }
            run_p1_case(vertical, getnative::detail::ColumnDispatchPolicy::scalar_only);
            vertical.scalar = vertical.differences;
            result.push_back(std::move(vertical));
        }
    }
    return result;
}

struct P1Measurement {
    std::string id;
    getnative::benchmark::Summary milliseconds;
    bool within_tolerance = false;
    bool bit_identical = false;
};

struct PlannerCaseMeasurement {
    std::string id;
    std::string filter_id;
    std::int32_t native_height = 0;
    bool fractional_scan = false;
    bool primary = false;
    std::string plan_content_fnv1a64;
    std::size_t plan_content_bytes = 0;
    getnative::benchmark::Summary cold_planner_ms;
    getnative::benchmark::Summary warm_planner_ms;
};

struct MatrixCaseMeasurement {
    std::string id;
    std::string filter_id;
    std::int32_t native_height = 0;
    bool fractional_scan = false;
    bool primary = false;
    getnative::benchmark::Summary execution_ms;
    getnative::benchmark::Summary end_to_end_ms;
    bool result_within_tolerance = false;
    bool result_bits_identical = false;
    double max_abs_metric_error = 0.0;
};

struct IsaMeasurement {
    getnative::CpuIsaRequest request = getnative::CpuIsaRequest::automatic;
    getnative::CpuDispatchInfo dispatch{};
    std::vector<MatrixCaseMeasurement> matrix_cases;
    std::vector<P0Measurement> p0;
    std::vector<P1Measurement> p1;
};

// Production CPU path: metrics use the shared tolerance
// max(1e-7, 5e-4 * |ref|). Bit-identical is reported but not required.
[[nodiscard]] bool close_metric(double actual, double expected) noexcept {
    if (std::bit_cast<std::uint64_t>(actual)
        == std::bit_cast<std::uint64_t>(expected)) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    const double scale = std::max(std::abs(actual), std::abs(expected));
    return std::abs(actual - expected) <= std::max(1e-7, 5e-4 * scale);
}

[[nodiscard]] bool close_float_buffer(float actual, float expected) noexcept {
    if (std::bit_cast<std::uint32_t>(actual)
        == std::bit_cast<std::uint32_t>(expected)) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    const float scale = std::max(std::abs(actual), std::abs(expected));
    return std::abs(actual - expected) <= std::max(1e-5F, 1e-3F * scale);
}

struct ResultCompare {
    bool within_tolerance = false;
    bool bits_identical = false;
    double max_abs_error = 0.0;
};

[[nodiscard]] ResultCompare compare_results(
    const std::vector<getnative::CandidateResult> &actual,
    const std::vector<getnative::CandidateResult> &reference) noexcept {
    ResultCompare result{};
    if (actual.size() != reference.size()) return result;
    result.within_tolerance = true;
    result.bits_identical = true;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index].id != reference[index].id) {
            result.within_tolerance = false;
            result.bits_identical = false;
            continue;
        }
        const double abs_error =
            std::abs(actual[index].error - reference[index].error);
        result.max_abs_error = std::max(result.max_abs_error, abs_error);
        if (std::bit_cast<std::uint64_t>(actual[index].error)
            != std::bit_cast<std::uint64_t>(reference[index].error)) {
            result.bits_identical = false;
        }
        if (!close_metric(actual[index].error, reference[index].error)) {
            result.within_tolerance = false;
        }
    }
    return result;
}

[[nodiscard]] bool buffer_within_tolerance(
    const std::vector<float> &actual, const std::vector<float> &reference) noexcept {
    if (actual.size() != reference.size()) return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (!close_float_buffer(actual[index], reference[index])) return false;
    }
    return true;
}

[[nodiscard]] bool buffer_bits_identical(
    const std::vector<float> &actual, const std::vector<float> &reference) noexcept {
    if (actual.size() != reference.size()) return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (std::bit_cast<std::uint32_t>(actual[index])
            != std::bit_cast<std::uint32_t>(reference[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] IsaMeasurement benchmark_isa_kernels(
    const Configuration &config,
    const std::vector<P0Case> &p0_cases,
    const std::vector<P1Case> &p1_cases,
    getnative::CpuIsaRequest request) {
    IsaMeasurement result{};
    result.request = request;
    result.dispatch = getnative::cpu_dispatch_info(request);
    if (!result.dispatch.request_available) (void)getnative::require_cpu_isa(request);
    const auto policy = getnative::detail::column_dispatch_policy(request);

    result.p0.reserve(p0_cases.size());
    for (const P0Case &source_case : p0_cases) {
        P0Case current = source_case;
        getnative::detail::inverse_columns_f32(
            current.plan, current.input.data() + current.base_offset,
            current.input_stride, current.output.data() + current.base_offset,
            current.output_stride, current.columns, policy);
        const bool within = buffer_within_tolerance(current.output, current.scalar);
        const bool identical = buffer_bits_identical(current.output, current.scalar);
        const auto samples = measure(config.samples, [&]() -> std::uint64_t {
            getnative::detail::inverse_columns_f32(
                current.plan, current.input.data() + current.base_offset,
                current.input_stride, current.output.data() + current.base_offset,
                current.output_stride, current.columns, policy);
            return std::bit_cast<std::uint32_t>(
                current.output[current.base_offset
                               + static_cast<std::size_t>(current.columns / 2)]);
        });
        result.p0.push_back({current.id, samples, within, identical});
    }

    result.p1.reserve(p1_cases.size());
    for (const P1Case &source_case : p1_cases) {
        P1Case current = source_case;
        run_p1_case(current, policy);
        const bool within =
            buffer_within_tolerance(current.differences, current.scalar);
        const bool identical =
            buffer_bits_identical(current.differences, current.scalar);
        const auto samples = measure(config.samples, [&]() -> std::uint64_t {
            run_p1_case(current, policy);
            return std::bit_cast<std::uint32_t>(
                current.differences[static_cast<std::size_t>(current.width / 2)]);
        });
        result.p1.push_back({current.id, samples, within, identical});
    }

    return result;
}

[[nodiscard]] PlannerCaseMeasurement benchmark_planner_case(
    const Configuration &config,
    const MatrixCase &benchmark_case,
    const PreparedCase &prepared) {
    PlannerCaseMeasurement result{
        benchmark_case.id,
        benchmark_case.filter_id,
        benchmark_case.native_height,
        benchmark_case.fractional_scan,
        benchmark_case.primary,
        plan_content_fingerprint(prepared.plan_content),
        prepared.plan_content.size(),
        {},
        {},
    };
    result.cold_planner_ms = measure(config.samples, [&]() -> std::uint64_t {
        getnative::AxisPlanCache cache;
        const auto built = cache.get_or_build_batch(prepared.requests);
        return static_cast<std::uint64_t>(built.plans.size())
            ^ (static_cast<std::uint64_t>(built.physical_build_count) << 32U);
    });

    getnative::AxisPlanCache warm_cache;
    (void)warm_cache.get_or_build_batch(prepared.requests);
    result.warm_planner_ms = measure(config.samples, [&]() -> std::uint64_t {
        const auto built = warm_cache.get_or_build_batch(prepared.requests);
        return static_cast<std::uint64_t>(built.ready_hit_count)
            ^ (static_cast<std::uint64_t>(built.physical_build_count) << 32U);
    });
    return result;
}

[[nodiscard]] MatrixCaseMeasurement benchmark_matrix_case(
    const Configuration &config,
    const Matrix &matrix,
    getnative::ConstImageView source,
    const MatrixCase &benchmark_case,
    const PreparedCase &prepared,
    const std::vector<getnative::CandidateResult> &scalar_results,
    const IsaMeasurement &isa) {
    const auto policy = getnative::detail::column_dispatch_policy(isa.request);
    const auto actual_results =
        getnative::detail::analyze_batch_with_column_policy_f32(
            source, prepared.candidates, matrix.metric, policy);
    const ResultCompare compared = compare_results(actual_results, scalar_results);
    MatrixCaseMeasurement result{
        benchmark_case.id,
        benchmark_case.filter_id,
        benchmark_case.native_height,
        benchmark_case.fractional_scan,
        benchmark_case.primary,
        {},
        {},
        compared.within_tolerance,
        compared.bits_identical,
        compared.max_abs_error,
    };
    result.execution_ms = measure(config.samples, [&]() -> std::uint64_t {
        return result_checksum(getnative::detail::analyze_batch_with_column_policy_f32(
            source, prepared.candidates, matrix.metric, policy));
    });

    result.end_to_end_ms = measure(config.samples, [&]() -> std::uint64_t {
        getnative::AxisPlanCache cache;
        const auto plans = cache.get_or_build_batch(prepared.requests).plans;
        const auto candidates = make_candidates(plans);
        return result_checksum(getnative::detail::analyze_batch_with_column_policy_f32(
            source, candidates, matrix.metric, policy));
    });
    return result;
}

[[nodiscard]] std::vector<getnative::CpuIsaRequest> requests_to_run(
    const Configuration &config) {
    if (!config.all_isa) return {config.request};
    std::vector<getnative::CpuIsaRequest> result{getnative::CpuIsaRequest::scalar};
    const getnative::CpuDispatchInfo automatic = getnative::cpu_dispatch_info();
    if (automatic.available.sse2) result.push_back(getnative::CpuIsaRequest::sse2);
    if (automatic.available.avx2) result.push_back(getnative::CpuIsaRequest::avx2);
    if (automatic.available.avx512) result.push_back(getnative::CpuIsaRequest::avx512);
    result.push_back(getnative::CpuIsaRequest::automatic);
    return result;
}

[[nodiscard]] std::optional<double> relative_percent(
    double numerator, double denominator) noexcept {
    if (!std::isfinite(numerator) || !std::isfinite(denominator)
        || numerator < 0.0 || denominator <= 0.0) {
        return std::nullopt;
    }
    const double result = (numerator / denominator - 1.0) * 100.0;
    return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

struct SelectionGate {
    getnative::CpuIsa lower = getnative::CpuIsa::scalar;
    getnative::CpuIsa higher = getnative::CpuIsa::scalar;
    std::optional<double> primary_gain_percent;
    std::optional<double> worst_matrix_regression_percent;
    std::optional<double> worst_kernel_regression_percent;
    std::size_t expected_matrix_case_count = 0;
    bool diagnostic_only = false;
    bool matrix_complete = false;
    bool formal_matrix_complete = false;
    bool matrix_correct = false;
    bool kernel_complete = false;
    bool timings_valid = true;
    bool passes = false;
};

[[nodiscard]] std::vector<SelectionGate> selection_gates(
    const std::vector<IsaMeasurement> &measurements,
    const std::vector<MatrixCase> &expected_cases,
    bool formal_matrix_selected) {
    const auto find = [&](getnative::CpuIsa isa) -> const IsaMeasurement * {
        const auto found = std::find_if(measurements.begin(), measurements.end(),
            [&](const auto &measurement) {
                return measurement.dispatch.forced
                    && measurement.dispatch.selected == isa;
            });
        return found == measurements.end() ? nullptr : &*found;
    };
    std::vector<SelectionGate> result;
    for (const auto [lower_isa, higher_isa] : {
             std::pair{getnative::CpuIsa::scalar, getnative::CpuIsa::sse2},
             std::pair{getnative::CpuIsa::sse2, getnative::CpuIsa::avx2},
             std::pair{getnative::CpuIsa::avx2, getnative::CpuIsa::avx512}}) {
        const IsaMeasurement *lower = find(lower_isa);
        const IsaMeasurement *higher = find(higher_isa);
        if (!lower || !higher) continue;
        const auto lower_primary = std::find_if(
            lower->matrix_cases.begin(), lower->matrix_cases.end(),
            [](const MatrixCaseMeasurement &value) {
                return value.primary && value.id == formal_primary_case_id;
            });
        const auto higher_primary = std::find_if(
            higher->matrix_cases.begin(), higher->matrix_cases.end(),
            [](const MatrixCaseMeasurement &value) {
                return value.primary && value.id == formal_primary_case_id;
            });
        if (lower_primary == lower->matrix_cases.end()
            || higher_primary == higher->matrix_cases.end()) {
            continue;
        }
        SelectionGate gate{};
        gate.lower = lower_isa;
        gate.higher = higher_isa;
        gate.expected_matrix_case_count = expected_cases.size();
        gate.diagnostic_only = !formal_matrix_selected;
        gate.primary_gain_percent = relative_percent(
            lower_primary->execution_ms.median, higher_primary->execution_ms.median);
        gate.timings_valid = gate.primary_gain_percent.has_value();
        gate.matrix_complete = lower->matrix_cases.size() == expected_cases.size()
            && higher->matrix_cases.size() == expected_cases.size();
        gate.matrix_correct = gate.matrix_complete;
        if (gate.matrix_complete) {
            for (std::size_t index = 0; index < expected_cases.size(); ++index) {
                const auto &lower_case = lower->matrix_cases[index];
                const auto &higher_case = higher->matrix_cases[index];
                const auto &expected = expected_cases[index];
                gate.matrix_complete = gate.matrix_complete
                    && lower_case.id == expected.id
                    && higher_case.id == expected.id
                    && lower_case.filter_id == expected.filter_id
                    && higher_case.filter_id == expected.filter_id
                    && lower_case.native_height == expected.native_height
                    && higher_case.native_height == expected.native_height
                    && lower_case.fractional_scan == expected.fractional_scan
                    && higher_case.fractional_scan == expected.fractional_scan
                    && lower_case.primary == expected.primary
                    && higher_case.primary == expected.primary;
                gate.matrix_correct = gate.matrix_correct
                    && lower_case.id == higher_case.id
                    && lower_case.result_within_tolerance
                    && higher_case.result_within_tolerance;
                const auto regression = relative_percent(
                    higher_case.execution_ms.median, lower_case.execution_ms.median);
                if (!regression) {
                    gate.timings_valid = false;
                } else if (!gate.worst_matrix_regression_percent
                           || *regression > *gate.worst_matrix_regression_percent) {
                    gate.worst_matrix_regression_percent = *regression;
                }
            }
        }
        gate.formal_matrix_complete = formal_matrix_selected
            && expected_cases.size() == formal_matrix_case_count
            && gate.matrix_complete;
        gate.kernel_complete = lower->p0.size() == higher->p0.size()
            && lower->p1.size() == higher->p1.size();
        const std::size_t count = std::min(lower->p0.size(), higher->p0.size());
        for (std::size_t index = 0; index < count; ++index) {
            gate.kernel_complete = gate.kernel_complete
                && lower->p0[index].id == higher->p0[index].id;
            const auto regression = relative_percent(
                higher->p0[index].milliseconds.median,
                lower->p0[index].milliseconds.median);
            if (!regression) {
                gate.timings_valid = false;
            } else if (!gate.worst_kernel_regression_percent
                       || *regression > *gate.worst_kernel_regression_percent) {
                gate.worst_kernel_regression_percent = *regression;
            }
        }
        const std::size_t p1_count = std::min(lower->p1.size(), higher->p1.size());
        for (std::size_t index = 0; index < p1_count; ++index) {
            gate.kernel_complete = gate.kernel_complete
                && lower->p1[index].id == higher->p1[index].id;
            const auto regression = relative_percent(
                higher->p1[index].milliseconds.median,
                lower->p1[index].milliseconds.median);
            if (!regression) {
                gate.timings_valid = false;
            } else if (!gate.worst_kernel_regression_percent
                       || *regression > *gate.worst_kernel_regression_percent) {
                gate.worst_kernel_regression_percent = *regression;
            }
        }
        gate.passes = gate.formal_matrix_complete
            && gate.matrix_correct
            && gate.kernel_complete
            && gate.timings_valid
            && gate.primary_gain_percent.has_value()
            && gate.worst_matrix_regression_percent.has_value()
            && gate.worst_kernel_regression_percent.has_value()
            && std::all_of(lower->p0.begin(), lower->p0.end(),
                           [](const auto &item) { return item.within_tolerance; })
            && std::all_of(higher->p0.begin(), higher->p0.end(),
                           [](const auto &item) { return item.within_tolerance; })
            && std::all_of(lower->p1.begin(), lower->p1.end(),
                           [](const auto &item) { return item.within_tolerance; })
            && std::all_of(higher->p1.begin(), higher->p1.end(),
                           [](const auto &item) { return item.within_tolerance; })
            && *gate.primary_gain_percent >= 5.0
            && *gate.worst_matrix_regression_percent <= 3.0
            && *gate.worst_kernel_regression_percent <= 3.0;
        result.push_back(gate);
    }
    return result;
}

void append_isa_set(std::ostream &output, const getnative::CpuIsaSet &set) {
    output << '[';
    bool first = true;
    for (const getnative::CpuIsa isa : {
             getnative::CpuIsa::scalar, getnative::CpuIsa::sse2,
             getnative::CpuIsa::avx2, getnative::CpuIsa::avx512}) {
        if (!set.contains(isa)) continue;
        if (!first) output << ',';
        first = false;
        output << getnative::benchmark::json_string(getnative::cpu_isa_name(isa));
    }
    output << ']';
}

[[nodiscard]] std::uint32_t physical_core_count() noexcept {
#if defined(_WIN32)
    static const std::uint32_t count = []() noexcept {
        DWORD byte_count = 0;
        if (::GetLogicalProcessorInformationEx(
                RelationProcessorCore, nullptr, &byte_count) != FALSE
            || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER
            || byte_count == 0) {
            return std::uint32_t{0};
        }
        std::vector<std::byte> buffer(byte_count);
        auto *data = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data());
        if (::GetLogicalProcessorInformationEx(
                RelationProcessorCore, data, &byte_count) == FALSE) {
            return std::uint32_t{0};
        }
        std::size_t offset = 0;
        std::uint32_t result = 0;
        constexpr std::size_t entry_header_size =
            offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
        while (offset < byte_count) {
            const std::size_t remaining = static_cast<std::size_t>(byte_count) - offset;
            if (remaining < entry_header_size) return 0U;
            const auto *entry = reinterpret_cast<
                const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(
                    buffer.data() + offset);
            if (entry->Size < entry_header_size
                || entry->Size > remaining) {
                return 0U;
            }
            if (entry->Relationship == RelationProcessorCore) ++result;
            offset += entry->Size;
        }
        return result;
    }();
    return count;
#else
    return 0;
#endif
}

void append_dispatch(std::ostream &output, const getnative::CpuDispatchInfo &dispatch) {
    const auto &snapshot = dispatch.snapshot;
    output << "{\"requested\":"
           << getnative::benchmark::json_string(getnative::cpu_isa_request_name(dispatch.request))
           << ",\"selected\":"
           << getnative::benchmark::json_string(getnative::cpu_isa_name(dispatch.selected))
           << ",\"forced\":" << (dispatch.forced ? "true" : "false")
           << ",\"request_available\":"
           << (dispatch.request_available ? "true" : "false")
           << ",\"math_mode\":\"production\",\"selection_reason\":"
           << getnative::benchmark::json_string(dispatch.selection_reason)
           << ",\"compiled_isa\":";
    append_isa_set(output, dispatch.compiled);
    output << ",\"available_isa\":";
    append_isa_set(output, dispatch.available);
    output << ",\"cpu\":{\"vendor\":"
           << getnative::benchmark::json_string(getnative::cpu_vendor(snapshot))
           << ",\"family\":" << snapshot.family
           << ",\"model\":" << snapshot.model
           << ",\"stepping\":" << snapshot.stepping
           << ",\"physical_cores\":" << physical_core_count()
           << ",\"logical_processors\":" << snapshot.logical_processor_count
           << ",\"maximum_basic_leaf\":" << snapshot.maximum_basic_leaf
           << ",\"leaf1\":{\"eax\":" << snapshot.leaf1.eax
           << ",\"ebx\":" << snapshot.leaf1.ebx
           << ",\"ecx\":" << snapshot.leaf1.ecx
           << ",\"edx\":" << snapshot.leaf1.edx << '}'
           << ",\"leaf7_subleaf0\":{\"eax\":" << snapshot.leaf7_subleaf0.eax
           << ",\"ebx\":" << snapshot.leaf7_subleaf0.ebx
           << ",\"ecx\":" << snapshot.leaf7_subleaf0.ecx
           << ",\"edx\":" << snapshot.leaf7_subleaf0.edx << '}'
           << ",\"xcr0\":" << snapshot.xcr0
           << ",\"xcr0_valid\":" << (snapshot.xcr0_valid ? "true" : "false")
           << "}}";
}

struct PowerPlanSnapshot {
    std::string status;
    std::string active_scheme_guid;
};

#if defined(_WIN32)
[[nodiscard]] std::string guid_string(const GUID &guid) {
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(8) << static_cast<std::uint32_t>(guid.Data1) << '-'
           << std::setw(4) << static_cast<std::uint32_t>(guid.Data2) << '-'
           << std::setw(4) << static_cast<std::uint32_t>(guid.Data3) << '-'
           << std::setw(2) << static_cast<std::uint32_t>(guid.Data4[0])
           << std::setw(2) << static_cast<std::uint32_t>(guid.Data4[1]) << '-';
    for (std::size_t index = 2; index < 8; ++index) {
        output << std::setw(2) << static_cast<std::uint32_t>(guid.Data4[index]);
    }
    return output.str();
}
#endif

[[nodiscard]] PowerPlanSnapshot capture_active_power_plan() {
#if defined(_WIN32)
    const HMODULE library = ::LoadLibraryW(L"PowrProf.dll");
    if (library == nullptr) {
        return {"powrprof-load-failed:" + std::to_string(::GetLastError()), {}};
    }
    using GetActiveSchemeFunction = DWORD(WINAPI *)(HKEY, GUID **);
    const auto get_active_scheme = reinterpret_cast<GetActiveSchemeFunction>(
        ::GetProcAddress(library, "PowerGetActiveScheme"));
    if (get_active_scheme == nullptr) {
        const DWORD error = ::GetLastError();
        ::FreeLibrary(library);
        return {"PowerGetActiveScheme-missing:" + std::to_string(error), {}};
    }
    GUID *scheme = nullptr;
    const DWORD status = get_active_scheme(nullptr, &scheme);
    if (status != ERROR_SUCCESS || scheme == nullptr) {
        ::FreeLibrary(library);
        return {"PowerGetActiveScheme-failed:" + std::to_string(status), {}};
    }
    const std::string value = guid_string(*scheme);
    ::LocalFree(scheme);
    ::FreeLibrary(library);
    return {"captured", value};
#else
    return {"not-applicable", {}};
#endif
}

void append_optional_double(
    std::ostream &output, const std::optional<double> &value) {
    if (value) {
        output << std::setprecision(17) << *value;
    } else {
        output << "null";
    }
}

void append_case_descriptor(
    std::ostream &output, const Matrix &matrix, const MatrixCase &benchmark_case) {
    const CandidatePoint first = candidate_point(matrix, benchmark_case, 0U);
    const CandidatePoint last = candidate_point(
        matrix, benchmark_case,
        static_cast<std::size_t>(matrix.candidate_count - 1));
    output << "{\"id\":" << getnative::benchmark::json_string(benchmark_case.id)
           << ",\"filter_id\":"
           << getnative::benchmark::json_string(benchmark_case.filter_id)
           << ",\"native_height\":" << benchmark_case.native_height
           << ",\"fractional_scan\":"
           << (benchmark_case.fractional_scan ? "true" : "false")
           << ",\"primary\":" << (benchmark_case.primary ? "true" : "false")
           << ",\"candidate_count\":" << matrix.candidate_count
           << ",\"first_candidate\":{\"index\":0,\"native_height\":"
           << first.native_height << ",\"active_height\":" << std::setprecision(17)
           << first.active_height << "}"
           << ",\"last_candidate\":{\"index\":" << matrix.candidate_count - 1
           << ",\"native_height\":" << last.native_height
           << ",\"active_height\":" << std::setprecision(17)
           << last.active_height << "}"
           << ",\"candidate_contract_fnv1a64\":"
           << getnative::benchmark::json_string(
                  candidate_contract_fingerprint(matrix, benchmark_case)) << '}';
}

[[nodiscard]] bool measurement_correct(
    const IsaMeasurement &measurement,
    const std::vector<MatrixCase> &expected_cases) noexcept {
    if (measurement.matrix_cases.size() != expected_cases.size()) return false;
    for (std::size_t index = 0; index < expected_cases.size(); ++index) {
        const auto &actual = measurement.matrix_cases[index];
        const auto &expected = expected_cases[index];
        if (actual.id != expected.id || actual.filter_id != expected.filter_id
            || actual.native_height != expected.native_height
            || actual.fractional_scan != expected.fractional_scan
            || actual.primary != expected.primary
            || !actual.result_within_tolerance) {
            return false;
        }
    }
    return std::all_of(
               measurement.p0.begin(), measurement.p0.end(),
               [](const auto &value) { return value.within_tolerance; })
        && std::all_of(
               measurement.p1.begin(), measurement.p1.end(),
               [](const auto &value) { return value.within_tolerance; });
}

[[nodiscard]] std::string make_json(
    const Configuration &config,
    const Matrix &matrix,
    const std::vector<float> &source_pixels,
    const std::vector<MatrixCase> &benchmark_cases,
    const std::vector<PlannerCaseMeasurement> &planner_cases,
    const PowerPlanSnapshot &power_plan,
    const std::vector<IsaMeasurement> &measurements,
    const std::vector<SelectionGate> &gates,
    bool formal_matrix_selected,
    bool assertions_pass,
    int argc,
    char **argv) {
    const std::size_t full_case_count = matrix.filters.size()
        * (matrix.native_heights.size()
           + (matrix.has_fractional_scan ? 1U : 0U));
    const std::string source_fingerprint = source_f32_fnv1a64(source_pixels);
    const std::string primary_id = matrix.primary_filter_id + "@"
        + std::to_string(matrix.primary_native_height);
    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_cpu_backend_benchmark", "batch-cache",
        "synthetic-from-metal-kernel-matrix-v1", argc, argv);
    output << ",\"planner_fp_mode\":"
           << getnative::benchmark::json_string(
                  GETNATIVE_BENCHMARK_PLANNER_FP_MODE)
           << ",\"planner_structure_mode\":"
           << getnative::benchmark::json_string(
                  GETNATIVE_BENCHMARK_PLANNER_STRUCTURE_MODE)
           << ",\"plan_content_contract_id\":"
           << getnative::benchmark::json_string(plan_content_contract_id)
           << ",\"plan_dump_path\":";
    if (config.plan_dump_path.empty()) {
        output << "null";
    } else {
        output << getnative::benchmark::json_string(config.plan_dump_path.string());
    }
    output << ",\"matrix\":{\"path\":"
           << getnative::benchmark::json_string(config.matrix_path.string())
           << ",\"fnv1a64\":"
           << getnative::benchmark::json_string(
                  getnative::benchmark::fnv1a64_file(config.matrix_path))
           << ",\"source_width\":" << matrix.source_width
           << ",\"source_height\":" << matrix.source_height
           << ",\"candidate_count\":" << matrix.candidate_count
           << ",\"tile_size\":" << matrix.tile_size
           << ",\"reduction_groups\":" << matrix.reduction_groups
           << ",\"inverse_threads\":" << matrix.inverse_threads
           << ",\"formal_matrix_case_count\":" << formal_matrix_case_count
           << ",\"full_case_count\":" << full_case_count
           << ",\"selected_case_count\":" << benchmark_cases.size()
           << ",\"formal_matrix_selected\":"
           << (formal_matrix_selected ? "true" : "false")
           << ",\"primary_case\":{\"id\":"
           << getnative::benchmark::json_string(primary_id)
           << ",\"formal_id\":"
           << getnative::benchmark::json_string(formal_primary_case_id)
           << ",\"filter_id\":"
           << getnative::benchmark::json_string(matrix.primary_filter_id)
           << ",\"native_height\":" << matrix.primary_native_height
           << ",\"candidate_generation\":\"fixed-native-height-open-interval\"}"
           << ",\"native_heights\":[";
    for (std::size_t index = 0; index < matrix.native_heights.size(); ++index) {
        if (index != 0) output << ',';
        output << matrix.native_heights[index];
    }
    output << "],\"filter_ids\":[";
    for (std::size_t index = 0; index < matrix.filters.size(); ++index) {
        if (index != 0) output << ',';
        output << getnative::benchmark::json_string(matrix.filters[index].id);
    }
    output << "],\"fractional_scan\":";
    if (matrix.has_fractional_scan) {
        output << "{\"id\":"
               << getnative::benchmark::json_string(matrix.scan_id)
               << ",\"native_start\":" << matrix.scan_native_start
               << ",\"native_end\":" << matrix.scan_native_end
               << ",\"active_start\":" << std::setprecision(17)
               << matrix.scan_active_start
               << ",\"active_end\":" << matrix.scan_active_end << "}";
    } else {
        output << "null";
    }
    output << ",\"metric\":{\"crop_left\":" << matrix.metric.crop_left
           << ",\"crop_right\":" << matrix.metric.crop_right
           << ",\"crop_top\":" << matrix.metric.crop_top
           << ",\"crop_bottom\":" << matrix.metric.crop_bottom
           << ",\"threshold\":" << matrix.metric.threshold
           << ",\"norm\":" << matrix.metric.norm << "}"
           << ",\"selected_cases\":[";
    for (std::size_t index = 0; index < benchmark_cases.size(); ++index) {
        if (index != 0) output << ',';
        append_case_descriptor(output, matrix, benchmark_cases[index]);
    }
    output << "]}"
           << ",\"source_fixture\":{\"kind\":\"deterministic-synthetic-v1\""
           << ",\"decoded_float32_fnv1a64\":"
           << getnative::benchmark::json_string(source_fingerprint) << '}'
           << ",\"candidate_contract\":{\"id\":"
           << getnative::benchmark::json_string(candidate_contract_id)
           << ",\"axis\":\"vertical\",\"offset\":0.0,\"border_mode\":\"mirror\""
           << ",\"candidate_ids\":\"candidate-{zero_based_index}\""
           << ",\"named_height_formula\":\"native + (index + 1) / (candidate_count + 1)\""
           << ",\"fractional_scan_formula\":\"inclusive linear interpolation; native=clamp(floor(active),native_start,native_end)\"}"
           << ",\"case_selection\":"
           << getnative::benchmark::json_string(
                  config.selected_case
                      ? "selected_case_diagnostic"
                      : (formal_matrix_selected
                             ? "full_matrix"
                             : "dev_or_custom_matrix"))
           << ",\"planner_only\":"
           << (config.planner_only ? "true" : "false")
           << ",\"sample_count\":" << config.samples
           << ",\"warmup_count\":1"
           << ",\"stages\":{\"cold_planner\":\"measured\""
           << ",\"warm_session_cache_planner\":\"measured\""
           << ",\"p0\":"
           << getnative::benchmark::json_string(
                  config.planner_only ? "not_run" : "measured")
           << ",\"p1\":"
           << getnative::benchmark::json_string(
                  config.planner_only ? "not_run" : "measured")
           << ",\"complete_execution\":"
           << getnative::benchmark::json_string(
                  config.planner_only ? "not_run" : "measured")
           << ",\"end_to_end\":"
           << getnative::benchmark::json_string(
                  config.planner_only ? "not_run" : "measured") << '}'
           << ",\"planner_cases\":[";
    for (std::size_t index = 0; index < planner_cases.size(); ++index) {
        if (index != 0) output << ',';
        const auto &planner = planner_cases[index];
        output << "{\"id\":" << getnative::benchmark::json_string(planner.id)
               << ",\"filter_id\":"
               << getnative::benchmark::json_string(planner.filter_id)
               << ",\"native_height\":" << planner.native_height
               << ",\"fractional_scan\":"
               << (planner.fractional_scan ? "true" : "false")
               << ",\"primary\":" << (planner.primary ? "true" : "false")
               << ",\"plan_content_fnv1a64\":"
               << getnative::benchmark::json_string(
                      planner.plan_content_fnv1a64)
               << ",\"plan_content_bytes\":" << planner.plan_content_bytes
               << ",\"cold_planner_ms\":";
        getnative::benchmark::append_summary(output, planner.cold_planner_ms);
        output << ",\"warm_session_cache_planner_ms\":";
        getnative::benchmark::append_summary(output, planner.warm_planner_ms);
        output << '}';
    }
    output << "],\"measurements\":[";
    for (std::size_t index = 0; index < measurements.size(); ++index) {
        if (index != 0) output << ',';
        const auto &measurement = measurements[index];
        const bool matrix_correct = measurement.matrix_cases.size() == benchmark_cases.size()
            && std::all_of(
                measurement.matrix_cases.begin(), measurement.matrix_cases.end(),
                [](const auto &value) { return value.result_within_tolerance; });
        output << "{\"dispatch\":";
        append_dispatch(output, measurement.dispatch);
        output << ",\"correctness\":{\"all_matrix_cases_within_tolerance\":"
               << (matrix_correct ? "true" : "false")
               << ",\"all_cases_and_kernels_within_tolerance\":"
               << (measurement_correct(measurement, benchmark_cases) ? "true" : "false")
               << ",\"math_mode\":\"production\""
               << ",\"metric_tolerance\":\"max(1e-7, 5e-4*|ref|)\"},\"matrix_cases\":[";
        for (std::size_t case_index = 0;
             case_index < measurement.matrix_cases.size(); ++case_index) {
            if (case_index != 0) output << ',';
            const auto &matrix_case = measurement.matrix_cases[case_index];
            output << "{\"id\":" << getnative::benchmark::json_string(matrix_case.id)
                   << ",\"filter_id\":"
                   << getnative::benchmark::json_string(matrix_case.filter_id)
                   << ",\"native_height\":" << matrix_case.native_height
                   << ",\"fractional_scan\":"
                   << (matrix_case.fractional_scan ? "true" : "false")
                   << ",\"primary\":" << (matrix_case.primary ? "true" : "false")
                   << ",\"correctness\":{\"result_within_tolerance\":"
                   << (matrix_case.result_within_tolerance ? "true" : "false")
                   << ",\"result_bits_identical\":"
                   << (matrix_case.result_bits_identical ? "true" : "false")
                   << ",\"max_abs_metric_error\":" << matrix_case.max_abs_metric_error
                   << '}'
                   << ",\"complete_execution_ms\":";
            getnative::benchmark::append_summary(output, matrix_case.execution_ms);
            output << ",\"end_to_end_ms\":";
            getnative::benchmark::append_summary(output, matrix_case.end_to_end_ms);
            output << '}';
        }
        output << "],\"p0_cases\":[";
        for (std::size_t case_index = 0; case_index < measurement.p0.size(); ++case_index) {
            if (case_index != 0) output << ',';
            const auto &p0 = measurement.p0[case_index];
            output << "{\"id\":" << getnative::benchmark::json_string(p0.id)
                   << ",\"within_tolerance\":"
                   << (p0.within_tolerance ? "true" : "false")
                   << ",\"bit_identical\":" << (p0.bit_identical ? "true" : "false")
                   << ",\"milliseconds\":";
            getnative::benchmark::append_summary(output, p0.milliseconds);
            output << '}';
        }
        output << "],\"p1_cases\":[";
        for (std::size_t case_index = 0; case_index < measurement.p1.size(); ++case_index) {
            if (case_index != 0) output << ',';
            const auto &p1 = measurement.p1[case_index];
            output << "{\"id\":" << getnative::benchmark::json_string(p1.id)
                   << ",\"within_tolerance\":"
                   << (p1.within_tolerance ? "true" : "false")
                   << ",\"bit_identical\":" << (p1.bit_identical ? "true" : "false")
                   << ",\"milliseconds\":";
            getnative::benchmark::append_summary(output, p1.milliseconds);
            output << '}';
        }
        output << "]}";
    }
    output << "],\"auto_production_denominator\":";
    const auto automatic = std::find_if(
        measurements.begin(), measurements.end(), [](const IsaMeasurement &value) {
            return value.request == getnative::CpuIsaRequest::automatic
                && !value.dispatch.forced;
        });
    const auto primary_case = std::find_if(
        benchmark_cases.begin(), benchmark_cases.end(), [](const MatrixCase &value) {
            return value.primary && value.id == formal_primary_case_id;
        });
    if (automatic == measurements.end() || primary_case == benchmark_cases.end()) {
        output << "null";
    } else {
        const auto denominator = std::find_if(
            automatic->matrix_cases.begin(), automatic->matrix_cases.end(),
            [](const MatrixCaseMeasurement &value) {
                return value.primary && value.id == formal_primary_case_id;
            });
        if (denominator == automatic->matrix_cases.end()) {
            output << "null";
        } else {
            const auto &snapshot = automatic->dispatch.snapshot;
            output << "{\"same_invocation\":true,\"backend\":\"cpu\",\"case_id\":"
                   << getnative::benchmark::json_string(denominator->id)
                   << ",\"requested\":\"auto\",\"selected_isa\":"
                   << getnative::benchmark::json_string(
                          getnative::cpu_isa_name(automatic->dispatch.selected))
                   << ",\"math_mode\":\"production\",\"forced\":false"
                   << ",\"selection_reason\":"
                   << getnative::benchmark::json_string(
                          automatic->dispatch.selection_reason)
                   << ",\"cpu_signature\":{\"vendor\":"
                   << getnative::benchmark::json_string(getnative::cpu_vendor(snapshot))
                   << ",\"family\":" << snapshot.family
                    << ",\"model\":" << snapshot.model
                    << ",\"stepping\":" << snapshot.stepping
                    << ",\"physical_cores\":" << physical_core_count()
                    << ",\"logical_processors\":"
                    << snapshot.logical_processor_count << '}'
                   << ",\"source_decoded_float32_fnv1a64\":"
                   << getnative::benchmark::json_string(source_fingerprint)
                   << ",\"candidate_contract_id\":"
                   << getnative::benchmark::json_string(candidate_contract_id)
                   << ",\"candidate_contract_fnv1a64\":"
                   << getnative::benchmark::json_string(
                          candidate_contract_fingerprint(matrix, *primary_case))
                   << ",\"correctness\":{\"result_within_tolerance\":"
                   << (denominator->result_within_tolerance ? "true" : "false")
                   << ",\"result_bits_identical\":"
                   << (denominator->result_bits_identical ? "true" : "false")
                   << ",\"max_abs_metric_error\":"
                   << denominator->max_abs_metric_error << '}'
                   << ",\"complete_execution_ms\":";
            getnative::benchmark::append_summary(output, denominator->execution_ms);
            output << ",\"end_to_end_ms\":";
            getnative::benchmark::append_summary(output, denominator->end_to_end_ms);
            output << '}';
        }
    }
    output << ",\"selection_gates\":[";
    for (std::size_t index = 0; index < gates.size(); ++index) {
        if (index != 0) output << ',';
        const auto &gate = gates[index];
        output << "{\"lower\":"
               << getnative::benchmark::json_string(getnative::cpu_isa_name(gate.lower))
               << ",\"higher\":"
               << getnative::benchmark::json_string(getnative::cpu_isa_name(gate.higher))
               << ",\"primary_case_id\":"
               << getnative::benchmark::json_string(formal_primary_case_id)
               << ",\"required_gain_percent\":5.0"
               << ",\"maximum_case_regression_percent\":3.0"
               << ",\"expected_matrix_case_count\":"
               << gate.expected_matrix_case_count
               << ",\"formal_matrix_case_count\":" << formal_matrix_case_count
               << ",\"diagnostic_only\":"
               << (gate.diagnostic_only ? "true" : "false")
               << ",\"matrix_complete\":"
               << (gate.matrix_complete ? "true" : "false")
               << ",\"formal_matrix_complete\":"
               << (gate.formal_matrix_complete ? "true" : "false")
               << ",\"matrix_correct\":"
               << (gate.matrix_correct ? "true" : "false")
               << ",\"kernel_complete\":"
               << (gate.kernel_complete ? "true" : "false")
               << ",\"timings_valid\":"
               << (gate.timings_valid ? "true" : "false")
               << ",\"observed_primary_gain_percent\":";
        append_optional_double(output, gate.primary_gain_percent);
        output << ",\"observed_worst_matrix_regression_percent\":";
        append_optional_double(output, gate.worst_matrix_regression_percent);
        output << ",\"observed_worst_kernel_regression_percent\":";
        append_optional_double(output, gate.worst_kernel_regression_percent);
        output << ",\"passes\":" << (gate.passes ? "true" : "false") << '}';
    }
    output << "]"
           << ",\"power_plan\":{\"status\":"
           << getnative::benchmark::json_string(power_plan.status)
           << ",\"active_scheme_guid\":"
           << getnative::benchmark::json_string(power_plan.active_scheme_guid) << '}'
           << ",\"correctness\":{\"assertions_requested\":"
           << (config.assert_correctness ? "true" : "false")
           << ",\"assertions_pass\":" << (assertions_pass ? "true" : "false")
           << "}}\n";
    return output.str();
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Configuration config = parse_arguments(argc, argv);
        const Matrix matrix = load_matrix(config.matrix_path);
        const std::vector<MatrixCase> all_cases = make_matrix_cases(matrix, std::nullopt);
        const std::vector<MatrixCase> benchmark_cases =
            make_matrix_cases(matrix, config.selected_case);
        const std::string matrix_primary_id = matrix.primary_filter_id + "@"
            + std::to_string(matrix.primary_native_height);
        const bool formal_matrix_selected = !config.selected_case
            && is_formal_matrix(matrix, all_cases)
            && benchmark_cases.size() == all_cases.size()
            && matrix_primary_id == formal_primary_case_id;
        if (config.list_cases) {
            std::cout << "full_case_count=" << all_cases.size()
                      << " selected_case_count=" << benchmark_cases.size()
                      << " formal_matrix_selected="
                      << (formal_matrix_selected ? "true" : "false") << '\n';
            for (const MatrixCase &benchmark_case : benchmark_cases) {
                std::cout << "case=" << benchmark_case.id
                          << " filter=" << benchmark_case.filter_id
                          << " native_height=" << benchmark_case.native_height
                          << " fractional_scan="
                          << (benchmark_case.fractional_scan ? "true" : "false")
                          << " primary=" << (benchmark_case.primary ? "true" : "false")
                          << '\n';
            }
            return EXIT_SUCCESS;
        }
        if (!config.plan_dump_path.empty() && benchmark_cases.size() != 1U) {
            throw std::invalid_argument(
                "--plan-dump requires exactly one selected matrix case");
        }

        const PowerPlanSnapshot power_plan = capture_active_power_plan();
        std::filesystem::create_directories(config.artifact_root);
        const std::filesystem::path artifact_path =
            config.artifact_root / "cpu-backend-results.json";
        getnative::benchmark::validate_json_output_path(artifact_path);

        const std::vector<float> source_pixels = make_source(matrix);
        const getnative::ConstImageView source{
            source_pixels.data(), matrix.source_width, matrix.source_height,
            matrix.source_width,
        };
        std::vector<P0Case> p0_cases;
        std::vector<P1Case> p1_cases;
        std::vector<getnative::CpuIsaRequest> requests;
        if (!config.planner_only) {
            p0_cases = make_p0_cases();
            p1_cases = make_p1_cases();
            requests = requests_to_run(config);
        }
        std::vector<IsaMeasurement> measurements;
        measurements.reserve(requests.size());
        for (const getnative::CpuIsaRequest request : requests) {
            std::cout << "measuring cpu_isa_kernels="
                      << getnative::cpu_isa_request_name(request)
                      << " samples=" << config.samples << '\n' << std::flush;
            measurements.push_back(
                benchmark_isa_kernels(config, p0_cases, p1_cases, request));
            measurements.back().matrix_cases.reserve(benchmark_cases.size());
        }

        std::vector<PlannerCaseMeasurement> planner_cases;
        planner_cases.reserve(benchmark_cases.size());
        for (std::size_t case_index = 0;
             case_index < benchmark_cases.size(); ++case_index) {
            const MatrixCase &benchmark_case = benchmark_cases[case_index];
            std::cout << "measuring matrix_case=" << benchmark_case.id
                      << " case_index=" << case_index + 1U
                      << '/' << benchmark_cases.size()
                      << " samples=" << config.samples << '\n' << std::flush;
            const PreparedCase prepared = prepare_case(matrix, benchmark_case);
            if (!config.plan_dump_path.empty()) {
                write_plan_dump(config.plan_dump_path, prepared.plan_content);
                std::cout << "plan_dump=" << config.plan_dump_path.string()
                          << " bytes=" << prepared.plan_content.size()
                          << " fnv1a64="
                          << plan_content_fingerprint(prepared.plan_content)
                          << '\n' << std::flush;
            }
            std::vector<getnative::CandidateResult> scalar_results;
            if (!config.planner_only) {
                scalar_results =
                    getnative::detail::analyze_batch_with_column_policy_f32(
                        source, prepared.candidates, matrix.metric,
                        getnative::detail::ColumnDispatchPolicy::scalar_only);
            }
            planner_cases.push_back(
                benchmark_planner_case(config, benchmark_case, prepared));
            for (IsaMeasurement &measurement : measurements) {
                measurement.matrix_cases.push_back(benchmark_matrix_case(
                    config, matrix, source, benchmark_case, prepared,
                    scalar_results, measurement));
            }
        }
        const auto gates = selection_gates(
            measurements, benchmark_cases, formal_matrix_selected);
        const bool planner_complete = planner_cases.size() == benchmark_cases.size()
            && std::equal(
                planner_cases.begin(), planner_cases.end(), benchmark_cases.begin(),
                [](const PlannerCaseMeasurement &actual, const MatrixCase &expected) {
                    return actual.id == expected.id
                        && actual.filter_id == expected.filter_id
                        && actual.native_height == expected.native_height
                        && actual.fractional_scan == expected.fractional_scan
                        && actual.primary == expected.primary;
                });
        const bool assertions_pass = planner_complete
            && std::all_of(
                measurements.begin(), measurements.end(),
                [&](const auto &measurement) {
                    return measurement_correct(measurement, benchmark_cases);
                });
        getnative::benchmark::atomic_write_json(
            artifact_path,
            make_json(
                config, matrix, source_pixels, benchmark_cases, planner_cases,
                power_plan, measurements, gates, formal_matrix_selected,
                assertions_pass, argc, argv));
        std::cout << "artifact=" << artifact_path.string()
                  << " assertions_pass=" << (assertions_pass ? "true" : "false") << '\n';
        if (config.assert_correctness && !assertions_pass) return EXIT_FAILURE;
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "CPU backend benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

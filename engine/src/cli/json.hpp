#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace getnative::cli {

// Minimal JSON value for the worker protocol. Objects preserve insertion
// order. Numbers keep their raw literal so decimal values survive without
// binary reformatting.
struct JsonValue {
    enum class Type : std::uint8_t { null_value, boolean, number, string, array, object };

    Type type = Type::null_value;
    bool bool_value = false;
    double number_value = 0.0;
    std::string raw_number;
    std::string string_value;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    [[nodiscard]] static JsonValue boolean(bool value) {
        JsonValue result;
        result.type = Type::boolean;
        result.bool_value = value;
        return result;
    }
    [[nodiscard]] static JsonValue number(double value, std::string raw = {});
    [[nodiscard]] static JsonValue integer(std::int64_t value);
    [[nodiscard]] static JsonValue string(std::string value) {
        JsonValue result;
        result.type = Type::string;
        result.string_value = std::move(value);
        return result;
    }
    [[nodiscard]] static JsonValue array(std::vector<JsonValue> values = {}) {
        JsonValue result;
        result.type = Type::array;
        result.items = std::move(values);
        return result;
    }
    [[nodiscard]] static JsonValue object(std::vector<std::pair<std::string, JsonValue>> values = {}) {
        JsonValue result;
        result.type = Type::object;
        result.members = std::move(values);
        return result;
    }

    [[nodiscard]] bool is_null() const noexcept { return type == Type::null_value; }
    [[nodiscard]] const JsonValue *find(std::string_view key) const noexcept;
    [[nodiscard]] std::string dump() const;
};

class JsonError : public std::runtime_error {
public:
    JsonError(std::string message, std::size_t offset)
        : std::runtime_error(std::move(message)), offset_(offset) {}
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

// Parses exactly one JSON document; trailing content raises JsonError.
[[nodiscard]] JsonValue parse_json(std::string_view text);

[[nodiscard]] std::string json_escape(std::string_view value);

} // namespace getnative::cli

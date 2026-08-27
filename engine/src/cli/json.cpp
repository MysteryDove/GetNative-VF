#include "json.hpp"
#include "getnative/number_parse.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace getnative::cli {

JsonValue JsonValue::number(double value, std::string raw) {
    JsonValue result;
    result.type = Type::number;
    result.number_value = value;
    if (raw.empty()) {
        std::ostringstream stream;
        stream << std::setprecision(17) << value;
        raw = stream.str();
    }
    result.raw_number = std::move(raw);
    return result;
}

JsonValue JsonValue::integer(std::int64_t value) {
    return number(static_cast<double>(value), std::to_string(value));
}

const JsonValue *JsonValue::find(std::string_view key) const noexcept {
    if (type != Type::object) return nullptr;
    for (const auto &[member_key, member_value] : members) {
        if (member_key == key) return &member_value;
    }
    return nullptr;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                result += "\\u00";
                result.push_back(hex[character >> 4U]);
                result.push_back(hex[character & 0x0FU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    result.push_back('"');
    return result;
}

std::string JsonValue::dump() const {
    std::string output;
    switch (type) {
    case Type::null_value: output = "null"; break;
    case Type::boolean: output = bool_value ? "true" : "false"; break;
    case Type::number: output = raw_number; break;
    case Type::string: output = json_escape(string_value); break;
    case Type::array: {
        output.push_back('[');
        bool first = true;
        for (const auto &item : items) {
            if (!first) output.push_back(',');
            first = false;
            output += item.dump();
        }
        output.push_back(']');
        break;
    }
    case Type::object: {
        output.push_back('{');
        bool first = true;
        for (const auto &[key, value] : members) {
            if (!first) output.push_back(',');
            first = false;
            output += json_escape(key);
            output.push_back(':');
            output += value.dump();
        }
        output.push_back('}');
        break;
    }
    }
    return output;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (offset_ != text_.size()) fail("unexpected trailing content");
        return value;
    }

private:
    std::string_view text_;
    std::size_t offset_ = 0;

    [[noreturn]] void fail(const std::string &message) const {
        throw JsonError(message, offset_);
    }

    char peek() const {
        if (offset_ >= text_.size()) fail("unexpected end of input");
        return text_[offset_];
    }

    char take() {
        const char character = peek();
        ++offset_;
        return character;
    }

    void expect(char expected) {
        if (take() != expected) fail(std::string{"expected '"} + expected + "'");
    }

    void skip_whitespace() {
        while (offset_ < text_.size()) {
            const char character = text_[offset_];
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r') return;
            ++offset_;
        }
    }

    void literal(std::string_view expected) {
        if (text_.substr(offset_, expected.size()) != expected) fail("invalid literal");
        offset_ += expected.size();
    }

    JsonValue parse_value() {
        switch (peek()) {
        case 'n': literal("null"); return JsonValue{};
        case 't': literal("true"); return JsonValue::boolean(true);
        case 'f': literal("false"); return JsonValue::boolean(false);
        case '"': return parse_string();
        case '[': return parse_array();
        case '{': return parse_object();
        default: return parse_number();
        }
    }

    JsonValue parse_string() {
        expect('"');
        std::string value;
        while (true) {
            const char character = take();
            if (character == '"') break;
            if (character == '\\') {
                const char escape = take();
                switch (escape) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'u': {
                    // Surrogate pairs are combined; other scalars pass through
                    // as UTF-8. Invalid sequences raise.
                    const std::uint32_t first = parse_hex4();
                    std::uint32_t codepoint = first;
                    if (first >= 0xD800U && first <= 0xDBFFU) {
                        if (take() != '\\' || take() != 'u') fail("unpaired surrogate");
                        const std::uint32_t second = parse_hex4();
                        if (second < 0xDC00U || second > 0xDFFFU) fail("unpaired surrogate");
                        codepoint = 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
                    } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(value, codepoint);
                    break;
                }
                default: fail("invalid escape");
                }
            } else {
                if (static_cast<unsigned char>(character) < 0x20U) fail("unescaped control character");
                value.push_back(character);
            }
        }
        return JsonValue::string(std::move(value));
    }

    std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = take();
            value <<= 4U;
            if (character >= '0' && character <= '9') value |= static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f') value |= static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value |= static_cast<std::uint32_t>(character - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    static void append_utf8(std::string &output, std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    JsonValue parse_number() {
        const std::size_t start = offset_;
        if (offset_ < text_.size() && text_[offset_] == '-') ++offset_;
        if (offset_ >= text_.size()) fail("invalid number");
        if (text_[offset_] == '0') {
            ++offset_;
        } else if (text_[offset_] >= '1' && text_[offset_] <= '9') {
            while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
        } else {
            fail("invalid number");
        }
        if (offset_ < text_.size() && text_[offset_] == '.') {
            ++offset_;
            if (offset_ >= text_.size() || text_[offset_] < '0' || text_[offset_] > '9') fail("invalid number");
            while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
        }
        if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-')) ++offset_;
            if (offset_ >= text_.size() || text_[offset_] < '0' || text_[offset_] > '9') fail("invalid number");
            while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
        }
        const std::string raw{text_.substr(start, offset_ - start)};
        double value = 0.0;
        if (!getnative::parse_finite_double(raw, value)) fail("invalid number");
        return JsonValue::number(value, raw);
    }

    JsonValue parse_array() {
        expect('[');
        skip_whitespace();
        std::vector<JsonValue> values;
        if (peek() == ']') {
            ++offset_;
            return JsonValue::array(std::move(values));
        }
        while (true) {
            skip_whitespace();
            values.push_back(parse_value());
            skip_whitespace();
            const char character = take();
            if (character == ']') break;
            if (character != ',') fail("expected ',' or ']'");
        }
        return JsonValue::array(std::move(values));
    }

    JsonValue parse_object() {
        expect('{');
        skip_whitespace();
        std::vector<std::pair<std::string, JsonValue>> values;
        if (peek() == '}') {
            ++offset_;
            return JsonValue::object(std::move(values));
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') fail("expected object key");
            JsonValue key = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            values.emplace_back(std::move(key.string_value), parse_value());
            skip_whitespace();
            const char character = take();
            if (character == '}') break;
            if (character != ',') fail("expected ',' or '}'");
        }
        return JsonValue::object(std::move(values));
    }
};

} // namespace

JsonValue parse_json(std::string_view text) {
    return Parser{text}.parse();
}

} // namespace getnative::cli

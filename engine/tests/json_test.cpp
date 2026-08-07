#include "../src/cli/json.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using getnative::cli::JsonValue;
using getnative::cli::parse_json;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

template <class Function>
void expect_throws(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

void test_scalars() {
    expect(parse_json("null").is_null(), "null parses");
    expect(parse_json("true").bool_value, "true parses");
    expect(!parse_json(" false ").bool_value, "whitespace tolerated");

    const JsonValue number = parse_json("-12.5e2");
    expect(number.type == JsonValue::Type::number, "number type");
    expect(number.number_value == -1250.0, "number value");
    expect(number.raw_number == "-12.5e2", "raw literal preserved");

    const JsonValue text = parse_json("\"hello\\nworld\\u0021\"");
    expect(text.string_value == "hello\nworld!", "escapes decode");
}

void test_structures() {
    const JsonValue value = parse_json(
        R"({"a":[1,2,{"b":"c"}],"d":null,"e":[{"f":3.5}]})");
    expect(value.type == JsonValue::Type::object, "object parses");
    const JsonValue* a = value.find("a");
    expect(a && a->items.size() == 3, "array members parse");
    expect(a->items[2].find("b")->string_value == "c", "nested object lookup");
    expect(value.find("d")->is_null(), "null member");
    expect(value.find("missing") == nullptr, "missing member lookup");
    expect(value.find("e")->items[0].find("f")->number_value == 3.5, "deep nesting");

    expect(parse_json("[]").items.empty(), "empty array");
    expect(parse_json("{}").members.empty(), "empty object");
}

void test_roundtrip() {
    const std::string source =
        R"({"key":"va\"lue","n":[0.1,-2e3],"unicode":"é帧"})";
    const JsonValue parsed = parse_json(source);
    const JsonValue reparsed = parse_json(parsed.dump());
    expect(reparsed.find("key")->string_value == "va\"lue", "string roundtrip");
    expect(reparsed.find("n")->items[0].number_value == 0.1, "number roundtrip");
    expect(reparsed.find("unicode")->string_value == "é帧", "utf8 roundtrip");
}

void test_rejections() {
    expect_throws([] { (void)parse_json(""); }, "empty input rejected");
    expect_throws([] { (void)parse_json("{} extra"); }, "trailing rejected");
    expect_throws([] { (void)parse_json("{\"a\":1,}"); }, "trailing comma rejected");
    expect_throws([] { (void)parse_json("[1 2]"); }, "missing comma rejected");
    expect_throws([] { (void)parse_json("\"\\uD800x\""); }, "unpaired surrogate rejected");
    expect_throws([] { (void)parse_json("01"); }, "leading zero rejected");
    expect_throws([] { (void)parse_json("1."); }, "missing fraction digits rejected");
    expect_throws([] { (void)parse_json("\"tab\there\""); }, "raw control rejected");
}

} // namespace

int main() {
    try {
        test_scalars();
        test_structures();
        test_roundtrip();
        test_rejections();
    } catch (const std::exception& error) {
        std::cerr << "json test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "json tests passed\n";
    return EXIT_SUCCESS;
}

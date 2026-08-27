#include "capabilities.hpp"
#include "worker.hpp"

#include "getnative/crop_geometry.hpp"
#include "getnative/number_parse.hpp"
#include "getnative/profile.hpp"
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::map<std::string, std::string, std::less<>> parse_options(int argc, char** argv, int first) {
    std::map<std::string, std::string, std::less<>> options;
    for (int index = first; index < argc; index += 2) {
        const std::string_view key = argv[index];
        if (!key.starts_with("--") || index + 1 >= argc) {
            throw std::invalid_argument("options must be --name value pairs");
        }
        options.emplace(std::string{key.substr(2)}, argv[index + 1]);
    }
    return options;
}

double required_double(const auto& options, std::string_view key) {
    const auto iterator = options.find(key);
    if (iterator == options.end()) {
        throw std::invalid_argument("missing --" + std::string{key});
    }
    double value = 0.0;
    const auto& text = iterator->second;
    if (!getnative::parse_finite_double(text, value)) {
        throw std::invalid_argument("invalid --" + std::string{key});
    }
    return value;
}

std::int64_t required_integer(const auto& options, std::string_view key) {
    const double value = required_double(options, key);
    if (std::trunc(value) != value) {
        throw std::invalid_argument("--" + std::string{key} + " must be an integer");
    }
    return getnative::python_int(value);
}

std::optional<std::int64_t> optional_integer(const auto& options, std::string_view key) {
    return options.contains(key) ? std::optional{required_integer(options, key)} : std::nullopt;
}

std::string number(double value) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

void print_geometry(int argc, char** argv) {
    const auto options = parse_options(argc, argv, 2);
    const auto profile_name = options.contains("profile")
        ? std::string_view{options.at("profile")}
        : std::string_view{"muf-d278cd3"};
    if (!getnative::parse_profile(profile_name)) {
        throw std::invalid_argument("unknown profile: " + std::string{profile_name});
    }

    const double active_width = required_double(options, "active-width");
    const double active_height = required_double(options, "active-height");
    const std::string_view mode = options.contains("mode")
        ? std::string_view{options.at("mode")}
        : std::string_view{"standard"};
    if (mode != "standard" && mode != "pro") {
        throw std::invalid_argument("unknown --mode: " + std::string{mode});
    }
    const bool pro = mode == "pro";
    getnative::Geometry geometry{};
    if (pro) {
        geometry = getnative::descale_geometry_pro(
            active_width,
            active_height,
            optional_integer(options, "base-height"),
            optional_integer(options, "base-width"));
    } else {
        geometry = getnative::descale_geometry(
            required_integer(options, "source-width"),
            required_integer(options, "source-height"),
            active_width,
            active_height,
            optional_integer(options, "base-height"));
    }

    std::cout << "{\"schema_version\":1,\"profile\":\"" << profile_name
              << "\",\"mode\":\"" << (pro ? "pro" : "standard")
              << "\",\"geometry\":{\"width\":" << geometry.width
              << ",\"height\":" << geometry.height
              << ",\"src_left\":" << number(geometry.src_left)
              << ",\"src_top\":" << number(geometry.src_top)
              << ",\"src_width\":" << number(geometry.src_width)
              << ",\"src_height\":" << number(geometry.src_height) << "}}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            throw std::invalid_argument(
                "usage: getnative-engine <capabilities|geometry|worker> [options]");
        }
        const std::string_view command = argv[1];
        if (command == "capabilities") {
            getnative::cli::write_capabilities(std::cout, false);
            return EXIT_SUCCESS;
        }
        if (command == "geometry") {
            print_geometry(argc, argv);
            return EXIT_SUCCESS;
        }
        if (command == "worker") {
            return getnative::cli::run_worker(std::cin, std::cout, std::cerr);
        }
        throw std::invalid_argument("unknown command: " + std::string{command});
    } catch (const std::exception& error) {
        std::cerr << "getnative-engine: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

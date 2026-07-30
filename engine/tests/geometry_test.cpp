#include "getnative/candidate_grid.hpp"
#include "getnative/crop_geometry.hpp"
#include "getnative/profile.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void expect_close(double actual, double expected, std::string_view message) {
    if (std::abs(actual - expected) > 1e-15) {
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

void test_python_rounding() {
    expect(getnative::python_round_to_even(2.5) == 2, "2.5 rounds to even 2");
    expect(getnative::python_round_to_even(3.5) == 4, "3.5 rounds to even 4");
    expect(getnative::python_round_to_even(-2.5) == -2, "-2.5 rounds to even -2");
    expect(getnative::python_round_to_even(-3.5) == -4, "-3.5 rounds to even -4");
    expect(getnative::python_int(-1.9) == -1, "Python int truncates toward zero");
}

void test_course_geometry() {
    const auto even = getnative::descale_geometry(1488, 837, 1488.0, 837.0, 838);
    expect(even.height == 838, "even base keeps an even output canvas");
    expect_close(even.src_top, 0.5, "837 active height is centered at top 0.5 in 838");

    const auto odd = getnative::descale_geometry_pro(1488.0, 837.0, 1001, std::nullopt);
    expect(odd.height == 837, "odd pro base can produce 837");
    expect_close(odd.src_top, 0.0, "odd pro base aligns exact parity at top zero");

    const auto width = getnative::descale_geometry_pro(1488.0, 837.0, std::nullopt, 1489);
    expect(width.width == 1489, "independent odd width base remains 1489");
    expect_close(width.src_left, 0.5, "1488 is centered at left 0.5 in 1489");
}

void test_no_base_rounding() {
    const auto geometry = getnative::descale_geometry(1920, 1080, 100.5, 101.5);
    expect(geometry.width == 100, "no-base width uses Python half-even");
    expect(geometry.height == 102, "no-base height uses Python half-even");
    expect_close(geometry.src_width, 100.0, "no-base src width is rounded canvas width");
}

void test_geometry_range_validation() {
    expect_throws(
        [] { (void)getnative::descale_geometry_pro(1e19, 1.0, 1, 1); },
        "parity multiplication overflow is rejected");
    expect_throws(
        [] { (void)getnative::descale_geometry_pro(1.0, 1e19, 1, 1); },
        "height parity multiplication overflow is rejected");
    expect_throws(
        [] { (void)getnative::descale_geometry_pro(0.49, 1.0); },
        "zero rounded width is rejected");
    expect_throws(
        [] { (void)getnative::descale_geometry(1920, 1080, 1.0, 0.49); },
        "zero rounded height is rejected");

    const auto fractional = getnative::descale_geometry_pro(1488.5, 837.25, 838, 1490);
    expect(fractional.width == 1490 && fractional.height == 838,
           "normal fractional parity geometry remains unchanged");
}

void test_candidate_grids() {
    const getnative::CandidateGridSpec spec{"0.1", "0.1", 12};
    const auto repeated = getnative::generate_candidates(spec, getnative::GridSemantics::repeated_addition);
    const auto multiplied = getnative::generate_candidates(spec, getnative::GridSemantics::index_multiplication);
    const auto decimal = getnative::generate_candidates(spec, getnative::GridSemantics::decimal_fixed_point);

    expect(repeated.size() == 12 && multiplied.size() == 12 && decimal.size() == 12, "all grids honor count");
    expect(repeated[6].value != multiplied[6].value, "legacy arithmetic modes preserve distinct binary drift");
    expect(decimal[6].decimal == "0.7", "decimal grid preserves exact decimal candidate");
    expect(decimal[11].decimal == "1.2", "decimal grid preserves exact end candidate");
    expect(repeated[0].decimal == "0.10000000000000001", "repeated grid serializes exact double value");

    const getnative::CandidateGridSpec overflowing{"1e308", "1e308", 2};
    expect_throws(
        [&] {
            (void)getnative::generate_candidates(
                overflowing, getnative::GridSemantics::repeated_addition);
        },
        "repeated candidate grids reject finite inputs whose sum overflows");
    expect_throws(
        [&] {
            (void)getnative::generate_candidates(
                overflowing, getnative::GridSemantics::index_multiplication);
        },
        "indexed candidate grids reject finite inputs whose product overflows");
}

void test_profiles() {
    const auto muf = getnative::parse_profile("muf-d278cd3");
    expect(muf.has_value(), "muf profile parses");
    expect(getnative::profile(*muf).default_grid == getnative::GridSemantics::repeated_addition,
           "muf defaults to repeated addition");
    expect(getnative::profile(getnative::CompatibilityProfile::getfnative_44c8d0f).default_crop == 10,
           "GetFnative crop default is ten");
}

} // namespace

int main() {
    try {
        test_python_rounding();
        test_course_geometry();
        test_no_base_rounding();
        test_geometry_range_validation();
        test_candidate_grids();
        test_profiles();
        std::cout << "all geometry tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "geometry test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

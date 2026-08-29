#include "getnative/candidate_grid.hpp"
#include "getnative/crop_geometry.hpp"
#include "getnative/profile.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

void test_candidate_geometry_resolver() {
    const auto height = getnative::resolve_candidate_geometry(
        1920, 1080, getnative::GeometryAxisMode::height_only, 837.25, 838);
    expect(height.width == 1920 && height.height == 838, "H-only keeps source width");
    expect_close(height.src_height, 837.25, "H-only keeps fractional active height");
    expect_close(height.src_top, 0.375, "H-only centers fractional active height");

    const auto oversized_base = getnative::resolve_candidate_geometry(
        1920, 1080, getnative::GeometryAxisMode::height_only, 843.8, 1081);
    expect(oversized_base.height == 845,
           "base height above source height remains a parity reference");
    expect_close(oversized_base.src_height, 843.8,
                 "oversized base does not replace the active height");

    const auto width = getnative::resolve_candidate_geometry(
        1920, 1080, getnative::GeometryAxisMode::width_only, 1488.5,
        std::nullopt, 1490);
    expect(width.width == 1490 && width.height == 1080, "W-only keeps source height");
    expect_close(width.src_width, 1488.5, "W-only keeps fractional active width");
    expect_close(width.src_left, 0.75, "W-only centers fractional active width");

    const auto both = getnative::resolve_candidate_geometry(
        1920, 1080, getnative::GeometryAxisMode::height_plus_width, 810.0);
    expect(both.width == 1440 && both.height == 810, "H+W derives integer width");
    expect_close(both.src_width, 1440.0, "H+W uses source aspect ratio");

    const auto explicit_width = getnative::resolve_candidate_geometry(
        1920, 1080, getnative::GeometryAxisMode::height_plus_width, 810.5,
        1001, 2001);
    const double derived_width = 1920.0 * 810.5 / 1080.0;
    expect_close(explicit_width.src_width, derived_width,
                 "explicit base width does not replace derived active width");
    expect(explicit_width.width == 1441 && explicit_width.height == 811,
           "explicit bases resolve parity canvases independently");
    expect_close(explicit_width.src_top, 0.25, "explicit base height resolves fractional shift");
    expect_close(explicit_width.src_left, (1441.0 - derived_width) / 2.0,
                 "explicit base width resolves fractional shift");

    expect_throws(
        [] {
            (void)getnative::resolve_candidate_geometry(
                1920, 1080, getnative::GeometryAxisMode::height_only,
                std::numeric_limits<double>::infinity());
        },
        "candidate resolver rejects non-finite geometry");
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

    const getnative::CandidateRangeSpec inclusive{
        "0.01", "0.19", "0.01", getnative::EndpointRule::inclusive, 100};
    const auto repeated_range = getnative::generate_candidate_range(
        inclusive, getnative::GridSemantics::repeated_addition);
    const auto indexed_range = getnative::generate_candidate_range(
        inclusive, getnative::GridSemantics::index_multiplication);
    const auto fixed_range = getnative::generate_candidate_range(
        inclusive, getnative::GridSemantics::decimal_fixed_point);
    expect(repeated_range.size() == 18, "MUF repeated addition preserves strict float stop");
    expect(indexed_range.size() == 19, "getnative indexed multiplication preserves its stop");
    expect(fixed_range.size() == 19 && fixed_range.back().decimal == "0.19",
           "modern fixed-point includes the exact stop");

    auto exclusive = inclusive;
    exclusive.endpoint = getnative::EndpointRule::exclusive_stop;
    expect(getnative::generate_candidate_range(
               exclusive, getnative::GridSemantics::decimal_fixed_point).size() == 18,
           "exclusive_stop omits the exact endpoint");

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
    const auto &muf_profile = getnative::profile(*muf);
    expect(muf_profile.default_axis == getnative::DefaultAxisMode::height_plus_width,
           "MUF defaults to H+W");
    expect(muf_profile.default_start == "500" && muf_profile.default_stop == "1000"
               && muf_profile.default_step == "1",
           "MUF default range matches upstream");
    expect(muf_profile.default_b == 0.0 && muf_profile.default_c == 0.5,
           "MUF default bicubic parameters match upstream");
}

} // namespace

int main() {
    try {
        test_python_rounding();
        test_course_geometry();
        test_no_base_rounding();
        test_geometry_range_validation();
        test_candidate_geometry_resolver();
        test_candidate_grids();
        test_profiles();
        std::cout << "all geometry tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "geometry test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

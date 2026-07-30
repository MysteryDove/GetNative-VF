#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace getnative {

enum class CompatibilityProfile {
    muf_d278cd3,
    getfnative_44c8d0f,
    modern,
};

enum class GridSemantics {
    repeated_addition,
    index_multiplication,
    decimal_fixed_point,
};

struct Profile {
    CompatibilityProfile id;
    std::string_view name;
    GridSemantics default_grid;
    std::int32_t default_crop;
    double default_threshold;
    bool threshold_is_strict;
};

struct Candidate {
    std::string decimal;
    double value;
};

struct CandidateGridSpec {
    std::string start;
    std::string step;
    std::size_t count;
};

struct Geometry {
    std::int64_t width;
    std::int64_t height;
    double src_left;
    double src_top;
    double src_width;
    double src_height;
};

} // namespace getnative

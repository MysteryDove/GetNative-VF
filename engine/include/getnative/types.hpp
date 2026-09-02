#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace getnative {

enum class CompatibilityProfile {
    muf_d278cd3,
};

enum class GridSemantics {
    repeated_addition,
    index_multiplication,
    decimal_fixed_point,
};

enum class EndpointRule {
    inclusive,
    exclusive_stop,
};

enum class DefaultAxisMode {
    height_only,
    height_plus_width,
};

enum class GeometryAxisMode {
    height_only,
    width_only,
    height_plus_width,
};

struct Profile {
    CompatibilityProfile id;
    std::string_view name;
    GridSemantics default_grid;
    std::string_view default_start;
    std::string_view default_stop;
    std::string_view default_step;
    EndpointRule default_endpoint;
    DefaultAxisMode default_axis;
    std::int32_t default_crop;
    double default_threshold;
    bool threshold_is_strict;
    std::string_view default_kernel;
    double default_b;
    double default_c;
    std::int32_t default_taps;
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

struct CandidateRangeSpec {
    std::string start;
    std::string stop;
    std::string step;
    EndpointRule endpoint = EndpointRule::inclusive;
    std::size_t maximum_count = 100000;
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

#include "getnative/axis_plan.hpp"

#include <dsmvc/engine.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

template <class T>
void write_array(std::ostream &out, std::span<const T> values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) out << ',';
        if constexpr (std::is_floating_point_v<T>) {
            out << std::setprecision(9) << values[index];
        } else {
            out << values[index];
        }
    }
    out << ']';
}

template <class T>
void write_array(std::ostream &out, const std::vector<T> &values) {
    write_array(out, std::span<const T>{values});
}

struct KernelCase {
    std::string_view name;
    getnative::Filter getnative_filter;
    dsmvc::KernelSpec dsmvc_filter;
};

template <class T, class U>
void require_equal(std::span<const T> actual, std::span<const U> expected,
                   std::string_view label) {
    if (actual.size() != expected.size()) {
        throw std::runtime_error(std::string{label} + " size differs");
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if constexpr (std::is_floating_point_v<T> || std::is_floating_point_v<U>) {
            if (!std::isfinite(static_cast<double>(actual[index]))
                || std::abs(static_cast<double>(actual[index])
                            - static_cast<double>(expected[index])) > 1e-6) {
                throw std::runtime_error(std::string{label} + " exceeds 1e-6 tolerance");
            }
        } else if (actual[index] != expected[index]) {
            throw std::runtime_error(std::string{label} + " differs");
        }
    }
}

template <class T, class U>
void require_equal(const std::vector<T> &actual, const std::vector<U> &expected,
                   std::string_view label) {
    require_equal(std::span<const T>{actual}, std::span<const U>{expected}, label);
}

} // namespace

int main() {
    const std::array<double, 5> blurs{0.75, 1.0, 1.01, 1.25, 1.5};
    const std::array geometries{
        std::array<double, 4>{17.0, 11.0, 11.375, -0.125},
        std::array<double, 4>{23.0, 15.0, 15.625, 0.375},
    };
    const std::array kernels{
        KernelCase{"bilinear", getnative::Filter::bilinear(),
                   {dsmvc::KernelKind::bilinear, 1, 0.0, 0.0, 1.0}},
        KernelCase{"bicubic", getnative::Filter::bicubic(0.0, 0.5),
                   {dsmvc::KernelKind::bicubic, 2, 0.0, 0.5, 1.0}},
        KernelCase{"lanczos3", getnative::Filter::lanczos(3),
                   {dsmvc::KernelKind::lanczos, 3, 0.0, 0.0, 1.0}},
        KernelCase{"spline16", getnative::Filter::spline16(),
                   {dsmvc::KernelKind::spline16, 2, 0.0, 0.0, 1.0}},
        KernelCase{"spline36", getnative::Filter::spline36(),
                   {dsmvc::KernelKind::spline36, 3, 0.0, 0.0, 1.0}},
        KernelCase{"spline64", getnative::Filter::spline64(),
                   {dsmvc::KernelKind::spline64, 4, 0.0, 0.0, 1.0}},
    };

    std::cout << "{\"schema_version\":1,\"reference_commit\":\"652cc95\","
                 "\"provenance\":{\"inverse\":\"dsmvc\","
                 "\"forward\":\"getnative-zimg-retained; dsmvc planner is inverse-only\"},"
                 "\"cases\":[";
    bool first = true;
    for (const auto &geometry : geometries) {
        for (const auto &kernel : kernels) {
            for (const double blur : blurs) {
                auto getnative_filter = kernel.getnative_filter;
                getnative_filter.blur = blur;
                auto dsmvc_filter = kernel.dsmvc_filter;
                dsmvc_filter.blur = blur;
                const auto source = static_cast<std::int32_t>(geometry[0]);
                const auto destination = static_cast<std::int32_t>(geometry[1]);
                const auto native = getnative::build_axis_plan({
                    source, destination, geometry[2], geometry[3], getnative_filter,
                    getnative::BorderMode::mirror});
                const auto oracle = dsmvc::build_axis_plan({
                    source, destination, geometry[2], geometry[3], dsmvc_filter,
                    dsmvc::BorderMode::symmetric, dsmvc::F64Mode::float32_only});
                if (native.support != oracle.support
                    || native.half_bandwidth != oracle.half_bandwidth) {
                    throw std::runtime_error("dsmvc plan shape differs");
                }
                require_equal(native.transpose_offsets, oracle.transpose_offsets,
                              "transpose offsets");
                require_equal(native.transpose_indices, oracle.transpose_indices,
                              "transpose indices");
                require_equal(native.transpose_weights, oracle.transpose_weights,
                              "transpose weights");
                require_equal(native.lower_ld, oracle.lower_ld, "lower LDLT band");
                require_equal(native.upper_l, oracle.upper_l, "upper LDLT band");
                require_equal(native.inverse_diagonal, oracle.inverse_diagonal,
                              "inverse diagonal");
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"kernel\":\"" << kernel.name << "\",\"blur\":"
                          << std::setprecision(17) << blur << ",\"source_size\":" << source
                          << ",\"destination_size\":" << destination
                          << ",\"active_length\":" << geometry[2]
                          << ",\"shift\":" << geometry[3]
                          << ",\"effective_support\":" << native.support
                          << ",\"half_bandwidth\":" << native.half_bandwidth
                          << ",\"forward_width\":" << native.forward_width;
                std::cout << ",\"transpose_offsets\":"; write_array(std::cout, oracle.transpose_offsets);
                std::cout << ",\"transpose_indices\":"; write_array(std::cout, oracle.transpose_indices);
                std::cout << ",\"transpose_weights\":"; write_array(std::cout, oracle.transpose_weights);
                std::cout << ",\"lower_ld\":"; write_array(std::cout, oracle.lower_ld);
                std::cout << ",\"upper_l\":"; write_array(std::cout, oracle.upper_l);
                std::cout << ",\"inverse_diagonal\":"; write_array(std::cout, oracle.inverse_diagonal);
                std::cout << ",\"forward_offsets\":"; write_array(std::cout, native.forward_offsets);
                std::cout << ",\"forward_indices\":"; write_array(std::cout, native.forward_indices);
                std::cout << ",\"forward_weights\":"; write_array(std::cout, native.forward_weights);
                std::cout << '}';
            }
        }
    }
    std::cout << "]}\n";
}

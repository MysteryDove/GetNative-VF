#include "getnative/profile.hpp"

#include <array>

namespace getnative {
namespace {

constexpr std::array<Profile, 3> kProfiles{{
    {CompatibilityProfile::muf_d278cd3, "muf-d278cd3", GridSemantics::repeated_addition, 5, 0.015, true},
    {CompatibilityProfile::getfnative_44c8d0f, "getfnative-44c8d0f", GridSemantics::index_multiplication, 10, 0.0, true},
    {CompatibilityProfile::modern, "modern", GridSemantics::decimal_fixed_point, 5, 0.015, true},
}};

} // namespace

std::span<const Profile> profiles() noexcept {
    return kProfiles;
}

const Profile& profile(CompatibilityProfile id) noexcept {
    for (const auto& candidate : kProfiles) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    return kProfiles.front();
}

std::optional<CompatibilityProfile> parse_profile(std::string_view name) noexcept {
    for (const auto& candidate : kProfiles) {
        if (candidate.name == name) {
            return candidate.id;
        }
    }
    return std::nullopt;
}

std::string_view grid_semantics_name(GridSemantics semantics) noexcept {
    switch (semantics) {
    case GridSemantics::repeated_addition:
        return "repeated_addition";
    case GridSemantics::index_multiplication:
        return "index_multiplication";
    case GridSemantics::decimal_fixed_point:
        return "decimal_fixed_point";
    }
    return "unknown";
}

} // namespace getnative

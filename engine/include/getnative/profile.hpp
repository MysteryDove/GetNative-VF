#pragma once

#include "getnative/types.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace getnative {

[[nodiscard]] std::span<const Profile> profiles() noexcept;
[[nodiscard]] const Profile& profile(CompatibilityProfile id) noexcept;
[[nodiscard]] std::optional<CompatibilityProfile> parse_profile(std::string_view name) noexcept;
[[nodiscard]] std::string_view grid_semantics_name(GridSemantics semantics) noexcept;

} // namespace getnative

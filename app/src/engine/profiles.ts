import type { EngineEnvelope, ProfileCapability } from "./types";

export const BUILTIN_PROFILES: ProfileCapability[] = [
  {
    id: "muf-d278cd3",
    grid_semantics: "repeated_addition",
    default_grid: { start: "500", stop: "1000", step: "1", endpoint_rule: "inclusive" },
    default_axis_mode: "h_plus_w",
    default_crop: 5,
    default_threshold: 0.015,
    threshold_comparison: "strict_greater_than",
    default_kernel: { id: "bicubic", b: 0, c: 0.5, taps: 3 },
  },
  {
    id: "getfnative-44c8d0f",
    grid_semantics: "index_multiplication",
    default_grid: { start: "500", stop: "1000", step: "0.25", endpoint_rule: "inclusive" },
    default_axis_mode: "h_plus_w",
    default_crop: 10,
    default_threshold: 0.015,
    threshold_comparison: "strict_greater_than",
    default_kernel: { id: "bicubic", b: 0, c: 0.5, taps: 3 },
  },
  {
    id: "modern",
    grid_semantics: "decimal_fixed_point",
    default_grid: { start: "500", stop: "1000", step: "1", endpoint_rule: "inclusive" },
    default_axis_mode: "h_plus_w",
    default_crop: 5,
    default_threshold: 0.015,
    threshold_comparison: "strict_greater_than",
    default_kernel: { id: "bicubic", b: 0, c: 0.5, taps: 3 },
  },
];

export function profilesFor(capabilities: EngineEnvelope | null): ProfileCapability[] {
  if (!capabilities?.payload.profiles.length) return BUILTIN_PROFILES;
  return capabilities.payload.profiles.map((profile) => {
    const fallback = BUILTIN_PROFILES.find((item) => item.id === profile.id) ?? BUILTIN_PROFILES[0];
    return {
      ...fallback,
      ...profile,
      default_grid: { ...fallback.default_grid, ...profile.default_grid },
      default_kernel: { ...fallback.default_kernel, ...profile.default_kernel },
    };
  });
}

export function profileFor(
  profileId: string,
  capabilities: EngineEnvelope | null = null,
): ProfileCapability {
  return (
    profilesFor(capabilities).find((profile) => profile.id === profileId) ??
    BUILTIN_PROFILES[0]
  );
}

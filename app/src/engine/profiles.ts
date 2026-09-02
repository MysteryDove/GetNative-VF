import type { EngineEnvelope, ProfileCapability } from "./types";

/** The only compatibility contract: muvsfunc.getnative (`muf-d278cd3`). */
export const MUF_PROFILE_ID = "muf-d278cd3";

export const BUILTIN_PROFILES: ProfileCapability[] = [
  {
    id: MUF_PROFILE_ID,
    grid_semantics: "repeated_addition",
    default_grid: { start: "500", stop: "1000", step: "1", endpoint_rule: "inclusive" },
    default_axis_mode: "h_plus_w",
    default_crop: 5,
    default_threshold: 0.015,
    threshold_comparison: "strict_greater_than",
    default_kernel: { id: "bicubic", b: 0, c: 0.5, taps: 3 },
  },
];

export function profilesFor(capabilities: EngineEnvelope | null): ProfileCapability[] {
  const fallback = BUILTIN_PROFILES[0];
  const advertised = capabilities?.payload.profiles.find((profile) => profile.id === MUF_PROFILE_ID);
  if (!advertised) return BUILTIN_PROFILES;
  return [
    {
      ...fallback,
      ...advertised,
      default_grid: { ...fallback.default_grid, ...advertised.default_grid },
      default_kernel: { ...fallback.default_kernel, ...advertised.default_kernel },
    },
  ];
}

/** Always the muvsfunc profile. Unknown ids are coerced, not selected. */
export function profileFor(
  _profileId?: string | null,
  capabilities: EngineEnvelope | null = null,
): ProfileCapability {
  return profilesFor(capabilities)[0] ?? BUILTIN_PROFILES[0];
}

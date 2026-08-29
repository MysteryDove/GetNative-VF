import type { Translator } from "../i18n";
import type { ActualBackend, AxisMode, BackendPreference } from "./protocol";
import type { BackendCapability, EngineEnvelope } from "./types";

function capabilityAxis(axisMode: AxisMode | undefined): string | null {
  if (axisMode === "h_only") return "vertical";
  if (axisMode === "w_only") return "horizontal";
  if (axisMode === "h_plus_w") return "both";
  return null;
}

function supportsTask(
  backend: BackendCapability | undefined,
  pNorm: number,
  axisMode?: AxisMode,
): boolean {
  if (
    !backend?.compiled ||
    !backend.device_available ||
    !backend.analysis_command_available
  ) {
    return false;
  }
  const axis = capabilityAxis(axisMode);
  if (axis && !backend.axes.includes(axis)) return false;
  const range = backend.p_norms;
  return range !== null && pNorm >= range.minimum && pNorm <= range.maximum;
}

export function selectableBackends(capabilities: EngineEnvelope | null): BackendPreference[] {
  const backends = capabilities?.payload.backends ?? [];
  const options: BackendPreference[] = ["auto"];
  for (const id of ["cpu", "cuda", "vulkan", "metal"] as const) {
    const backend = backends.find((item) => item.id === id);
    if (backend?.compiled && backend.device_available && backend.analysis_command_available) {
      options.push(id);
    }
  }
  return options;
}

export function verifySelectableBackends(
  capabilities: EngineEnvelope | null,
): BackendPreference[] {
  // The legacy frame-asset verify is CPU-only. Engine-side media verify
  // supports explicit accelerators; Metal additionally requires the
  // VideoToolbox-to-Metal zero-copy path reported by the engine.
  if (capabilities?.payload.features?.verify_engine_decode !== true) {
    return ["auto", "cpu"];
  }
  const metal = capabilities?.payload.decode_backends?.find((item) => item.id === "videotoolbox");
  const metalReady = metal?.compiled === true
    && metal.runtime_device === true
    && metal.zero_copy === true;
  return selectableBackends(capabilities).filter((backend) => backend !== "metal" || metalReady);
}

export function resolveBackendPreference(
  capabilities: EngineEnvelope | null,
  backend: BackendPreference,
  pNorm = 1,
  axisMode?: AxisMode,
): Exclude<BackendPreference, "auto"> {
  if (backend !== "auto") return backend;
  const backends = capabilities?.payload.backends ?? [];
  const cuda = backends.find((item) => item.id === "cuda");
  // Missing means a pre-auto_priority engine: retain its CUDA -> CPU policy.
  const cudaAdmitted = cuda?.auto_priority === 10 || cuda?.auto_priority === undefined;
  if (cudaAdmitted && pNorm <= 4 && supportsTask(cuda, pNorm, axisMode)) return "cuda";

  const vulkan = backends.find((item) => item.id === "vulkan");
  if (
    pNorm >= 1 && pNorm <= 4 &&
    vulkan?.auto_priority === 20 &&
    vulkan.device_type === "discrete_gpu" &&
    supportsTask(vulkan, pNorm, axisMode)
  ) {
    return "vulkan";
  }

  const metal = backends.find((item) => item.id === "metal");
  if (
    metal?.auto_priority === 30 &&
    pNorm >= 1 && pNorm <= 4 &&
    supportsTask(metal, pNorm, axisMode)
  ) {
    return "metal";
  }
  return "cpu";
}

export function validateBackendPNorm(
  capabilities: EngineEnvelope | null,
  backend: BackendPreference,
  pNorm: number,
  axisMode?: AxisMode,
): { ok: true } | { ok: false; reason: string } {
  if (!Number.isInteger(pNorm) || pNorm < 1 || pNorm > 4_294_967_295) {
    return { ok: false, reason: "p_norm_invalid" };
  }
  const resolved = resolveBackendPreference(capabilities, backend, pNorm, axisMode);
  if (resolved !== "cpu" && resolved !== "cuda" && resolved !== "vulkan" && resolved !== "metal") {
    return { ok: false, reason: "backend_unsupported" };
  }
  const reported = capabilities?.payload.backends.find((item) => item.id === resolved);
  const range = reported?.p_norms ?? (resolved === "cpu"
    ? { minimum: 1, maximum: 4_294_967_295 }
    : { minimum: 1, maximum: 4 });
  return pNorm >= range.minimum && pNorm <= range.maximum
    ? { ok: true }
    : { ok: false, reason: "backend_p_norm_unsupported" };
}

/**
 * pNorm upper bound for the resolved backend: the engine-reported maximum when
 * known, else the same fallback table validateBackendPNorm uses
 * (cuda/vulkan/metal → 4, cpu/auto/unknown → 4_294_967_295).
 */
export function pNormMaximumForBackend(
  capabilities: EngineEnvelope | null,
  resolvedBackend: string,
): number {
  return (
    capabilities?.payload.backends.find((backend) => backend.id === resolvedBackend)
      ?.p_norms?.maximum ??
    (resolvedBackend === "cuda"
      ? 4
      : resolvedBackend === "vulkan" || resolvedBackend === "metal" ? 4 : 4_294_967_295)
  );
}

export function backendOptionLabel(
  t: Translator,
  backend: BackendPreference,
  capabilities: EngineEnvelope | null,
  pNorm = 1,
  axisMode?: AxisMode,
  legacyVerify = false,
): string {
  if (backend !== "auto") return t(`backend.${backend}` as "backend.cpu");
  if (legacyVerify) return t("analyze.backend.autoResolved", { backend: t("backend.cpu") });
  if (!capabilities) return t("analyze.backend.autoDetecting");
  const resolved = resolveBackendPreference(capabilities, backend, pNorm, axisMode);
  const resolvedName = t(`backend.${resolved}` as "backend.cpu");
  const device = capabilities.payload.backends.find((item) => item.id === resolved)?.device;
  return device && resolved !== "cpu"
    ? t("analyze.backend.autoResolvedDevice", { backend: resolvedName, device })
    : t("analyze.backend.autoResolved", { backend: resolvedName });
}

export function actualBackendLabel(
  t: Translator,
  backend: ActualBackend,
  device?: string | null,
): string {
  const backendName = t(`backend.${backend}` as "backend.cpu");
  return device ? `${backendName} · ${device}` : backendName;
}

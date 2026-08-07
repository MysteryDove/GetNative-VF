export type GeometryMode = "standard" | "pro";
export type EngineState = "checking" | "ready" | "missing";

export type Geometry = {
  width: number;
  height: number;
  src_left: number;
  src_top: number;
  src_width: number;
  src_height: number;
};

export type KernelCapability = {
  id: string;
  parameters: Record<string, string | number | boolean>;
};

export type BackendCapability = {
  id: string;
  compiled: boolean;
  device_available: boolean;
  analysis_command_available: boolean;
  axes: string[];
  p_norms: { minimum: number; maximum: number } | null;
  max_half_bandwidth: number | null;
  max_forward_width: number | null;
  device?: string;
  reason?: string;
  compiled_isa?: string[];
  available_isa?: string[];
  selected_isa?: string;
  math_modes?: string[];
  selected_math_mode?: string;
  selection_reason?: string;
};

export type EngineEnvelope = {
  path: string;
  payload: {
    schema_version: number;
    engine: string;
    version: string;
    commands: {
      capabilities: boolean;
      geometry: boolean;
      analyze: boolean;
    };
    kernels: KernelCapability[];
    backends: BackendCapability[];
    profiles: Array<{ id: string; default_crop: number }>;
  };
};

export type GeometryEnvelope = {
  path: string;
  payload: {
    geometry: Geometry;
    profile: string;
    mode: GeometryMode;
  };
};

export type BackendDisplayRow = {
  id: "cpu" | "metal" | "cuda" | "vulkan";
  reported: boolean;
  compiled: boolean | null;
  deviceAvailable: boolean | null;
  analysisCommandAvailable: boolean | null;
  axes: string[];
  detail: string;
  reason?: string;
  selectedIsa?: string;
  selectedMathMode?: string;
};

export type BackendDisplayState = "ready" | "partial" | "unavailable";

export function backendDisplayState(row: BackendDisplayRow): BackendDisplayState {
  if (
    row.reported &&
    row.compiled &&
    row.deviceAvailable &&
    row.analysisCommandAvailable
  ) {
    return "ready";
  }
  if (row.reported && row.compiled && row.deviceAvailable) {
    return "partial";
  }
  return "unavailable";
}

/** Merge engine backends with a reserved Vulkan row that never claims support when unreported. */
export function buildBackendRows(backends: BackendCapability[] | undefined): BackendDisplayRow[] {
  const byId = new Map((backends ?? []).map((backend) => [backend.id, backend]));
  const order = ["cpu", "metal", "cuda", "vulkan"] as const;

  return order.map((id) => {
    const backend = byId.get(id);
    if (!backend) {
      return {
        id,
        reported: false,
        compiled: null,
        deviceAvailable: null,
        analysisCommandAvailable: null,
        axes: [],
        detail: "unreported",
        reason: "unreported",
      };
    }
    return {
      id,
      reported: true,
      compiled: backend.compiled,
      deviceAvailable: backend.device_available,
      analysisCommandAvailable: backend.analysis_command_available,
      axes: backend.axes,
      detail: backend.reason ?? "",
      reason: backend.reason,
      selectedIsa: backend.selected_isa,
      selectedMathMode: backend.selected_math_mode,
    };
  });
}

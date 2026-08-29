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

export type GridSemantics =
  | "repeated_addition"
  | "index_multiplication"
  | "decimal_fixed_point";

export type ProfileCapability = {
  id: string;
  grid_semantics: GridSemantics;
  default_grid: {
    start: string;
    stop: string;
    step: string;
    endpoint_rule: "inclusive" | "exclusive_stop";
  };
  default_axis_mode: "h_only" | "h_plus_w";
  default_crop: number;
  default_threshold: number;
  threshold_comparison: "strict_greater_than";
  default_kernel: {
    id: string;
    b: number;
    c: number;
    taps: number;
  };
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
  device_type?: "discrete_gpu" | "integrated_gpu" | "virtual_gpu" | "cpu" | "other";
  auto_priority?: number | null;
  reason?: string;
  compiled_isa?: string[];
  available_isa?: string[];
  selected_isa?: string;
  math_modes?: string[];
  selected_math_mode?: string;
  selection_reason?: string;
};

export type DecodeBackendCapability = {
  id: "software" | "nvdec" | "vulkan_video" | "videotoolbox";
  compiled: boolean;
  runtime_device: boolean;
  codecs: string[];
  surface_formats?: string[];
  zero_copy: boolean;
  reason?: string;
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
      media_index_begin?: boolean;
      media_frame_window?: boolean;
      media_preview_begin?: boolean;
      media_asset_batch_begin?: boolean;
    };
    kernels: KernelCapability[];
    backends: BackendCapability[];
    profiles: Array<
      Pick<ProfileCapability, "id" | "default_crop"> &
      Partial<Omit<ProfileCapability, "id" | "default_crop">>
    >;
    features?: {
      verify_frame_ring?: boolean;
      media_frame_batch?: boolean;
      verify_engine_decode?: boolean;
      media_verify_concurrency?: {
        min: number;
        max: number;
        default: number;
      };
    };
    decode_backends?: DecodeBackendCapability[];
    media?: {
      available: boolean;
      ffmpeg_abi: string | null;
      index_version: number | null;
      index_format: string;
    };
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
  device?: string;
  deviceType?: BackendCapability["device_type"];
  autoPriority?: number | null;
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
      device: backend.device,
      deviceType: backend.device_type,
      autoPriority: backend.auto_priority,
    };
  });
}

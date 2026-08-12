import type {
  GeometrySnapshot,
  AxisMode,
  KernelRef,
  MathMode,
  MetricSpec,
} from "../engine/protocol";

export type SourceKind = "still" | "video" | "animated";
export type SourceState =
  | "added"
  | "probing"
  | "ready"
  | "unsupported"
  | "missing"
  | "error";

export type ProjectRoute =
  | "overview"
  | "media"
  | "samples"
  | "analyze"
  | "verify"
  | "results"
  | "settings"
  | "diagnostics";

export type VideoStream = {
  index: number;
  codecName?: string | null;
  width?: number | null;
  height?: number | null;
  durationSeconds?: number | null;
  frameCount?: number | null;
  timeBaseNum?: number | null;
  timeBaseDen?: number | null;
  frameRateNum?: number | null;
  frameRateDen?: number | null;
};

export type Source = {
  id: string;
  kind: SourceKind;
  path: string;
  fingerprint?: string | null;
  state: SourceState;
  label?: string | null;
  sizeBytes?: number | null;
  width?: number | null;
  height?: number | null;
  durationSeconds?: number | null;
  decoder?: string | null;
  videoStreams: VideoStream[];
  selectedStreamIndex?: number | null;
  errorCode?: string | null;
  errorDetail?: string | null;
};

export type Sample = {
  id: string;
  sourceId: string;
  sourceFingerprint?: string | null;
  label?: string | null;
  included: boolean;
  order: number;
  frameIndex?: number | null;
  streamIndex?: number | null;
  pts?: number | null;
  bestEffortTimestamp?: number | null;
  timeBaseNum?: number | null;
  timeBaseDen?: number | null;
  timestampSeconds?: number | null;
  tags: string[];
  /** Set when the Sample was added from a VerificationRun review. */
  originRunId?: string | null;
};

/**
 * Recipe lifecycle per DESIGN.md: a draft is mutable; locking makes it immutable;
 * locking a derived draft supersedes its locked parent. Only locked (never draft
 * or superseded) Recipes may be newly activated, and activation is explicit.
 */
export type RecipeStatus = "draft" | "locked" | "superseded";

/**
 * Analysis Recipe (分析方案). A locked Recipe owns every semantic value that
 * changes the metric: geometry, kernel, metric crop, pixel exclusion threshold,
 * p-norm, compatibility profile, and math mode. Payload fields may be partial
 * while drafting; locking requires all of them.
 */
export type Recipe = {
  id: string;
  name: string;
  /** Optional user tag appended to derived names: `WxH-kernel-suffix`. */
  nameSuffix?: string | null;
  status: RecipeStatus;
  /** Derived view kept for the schema-1 manifest `locked` flag: status !== "draft". */
  locked: boolean;
  revision: number;
  parentRecipeId: string | null;
  createdAt: string;
  updatedAt: string;
  geometry: GeometrySnapshot | null;
  kernel: KernelRef | null;
  metric: MetricSpec | null;
  axisMode: AxisMode;
  profileId: string | null;
  mathMode: MathMode | null;
};

/** UI-level group of independent engine Runs created by one user command. */
export type RunGroup = {
  id: string;
  memberRunIds: string[];
  groupType: string;
  label: string;
  createdAt: string;
  /** Frozen user intent; never mutated after creation. */
  intentSnapshot: unknown | null;
};

/**
 * Immutable analysis/verification Run record.
 * `inputSnapshot` is frozen at create time; `result` is append-only from the engine.
 */
export type Run = {
  id: string;
  runType: string;
  status: string;
  runGroupId: string | null;
  sampleId: string | null;
  sourceId: string | null;
  createdAt: string;
  updatedAt: string;
  inputSnapshot: unknown | null;
  result: unknown | null;
  errorCode: string | null;
  errorMessage: string | null;
  completed: number;
  total: number;
};

export type VerificationReview = {
  runId: string;
  tags: string[];
};

export type ProjectMeta = {
  id: string;
  name: string;
  schemaVersion: number;
  createdAt: string;
  updatedAt: string;
  activeRecipeId: string | null;
  untitled: boolean;
  storagePath: string | null;
  readOnly: boolean;
  dirty: boolean;
};

export type ProjectState = {
  project: ProjectMeta;
  sourcesById: Record<string, Source>;
  samplesById: Record<string, Sample>;
  recipesById: Record<string, Recipe>;
  runGroupsById: Record<string, RunGroup>;
  runsById: Record<string, Run>;
  verificationReviewsByRunId: Record<string, VerificationReview>;
  uiStateByRoute: Record<string, unknown>;
};

/** Wire format for the schema-1 Project manifest (snake_case, language-neutral). */
export type ProjectManifestDto = {
  schema_version: number;
  id: string;
  name: string;
  created_at: string;
  updated_at: string;
  active_recipe_id: string | null;
  untitled: boolean;
  sources: Array<{
    id: string;
    kind: SourceKind;
    path: string;
    fingerprint?: string | null;
    state?: SourceState;
    label?: string | null;
    size_bytes?: number | null;
    width?: number | null;
    height?: number | null;
    duration_seconds?: number | null;
    decoder?: string | null;
    video_streams?: Array<{
      index: number;
      codec_name?: string | null;
      width?: number | null;
      height?: number | null;
      duration_seconds?: number | null;
      frame_count?: number | null;
      time_base_num?: number | null;
      time_base_den?: number | null;
      frame_rate_num?: number | null;
      frame_rate_den?: number | null;
    }>;
    selected_stream_index?: number | null;
    error_code?: string | null;
    error_detail?: string | null;
  }>;
  samples: Array<{
    id: string;
    source_id: string;
    source_fingerprint?: string | null;
    label?: string | null;
    included?: boolean;
    order?: number;
    frame_index?: number | null;
    stream_index?: number | null;
    pts?: number | null;
    best_effort_timestamp?: number | null;
    time_base_num?: number | null;
    time_base_den?: number | null;
    timestamp_seconds?: number | null;
    tags?: string[];
    origin_run_id?: string | null;
  }>;
  recipes: Array<{
    id: string;
    name: string;
    name_suffix?: string | null;
    /** Legacy schema-1 flag: true for both locked and superseded Recipes. */
    locked?: boolean;
    status?: RecipeStatus;
    revision?: number;
    parent_recipe_id?: string | null;
    created_at?: string;
    updated_at?: string;
    geometry?: GeometrySnapshot | null;
    kernel?: KernelRef | null;
    metric?: MetricSpec | null;
    axis_mode?: AxisMode;
    profile_id?: string | null;
    math_mode?: MathMode | null;
  }>;
  run_groups: Array<{
    id: string;
    member_run_ids?: string[];
    group_type?: string;
    label?: string;
    created_at?: string;
    intent_snapshot?: unknown | null;
  }>;
  runs: Array<{
    id: string;
    run_type?: string;
    status?: string;
    run_group_id?: string | null;
    sample_id?: string | null;
    source_id?: string | null;
    created_at?: string;
    updated_at?: string;
    input_snapshot?: unknown | null;
    result?: unknown | null;
    error_code?: string | null;
    error_message?: string | null;
    completed?: number;
    total?: number;
  }>;
  verification_reviews: Array<{
    run_id: string;
    tags?: string[];
  }>;
  ui_state_by_route?: Record<string, unknown>;
};

export type OpenedProjectDto = {
  manifest: ProjectManifestDto;
  storage_path: string | null;
  missing_source_ids: string[];
  read_only: boolean;
  schema_status: string;
};

export type ManifestErrorDto = {
  code:
    | "invalid_json"
    | "invalid_manifest"
    | "unsupported_schema"
    | "missing_id"
    | "missing_name"
    | "duplicate_id"
    | "project_mismatch"
    | "path_rejected"
    | "io_error"
    | "cancelled";
  message: string;
};

export type RecentProjectEntry = {
  path: string;
  id: string;
  name: string;
  last_opened_at: string;
  source_count: number;
  active_recipe_name: string | null;
  has_missing_media: boolean;
};

export type RecoveryInfo = {
  present: boolean;
  path: string | null;
  name: string | null;
  updated_at: string | null;
};

export type ProjectCommandResult = {
  ok: boolean;
  opened: OpenedProjectDto | null;
  recent: RecentProjectEntry[] | null;
  recovery: RecoveryInfo | null;
  error: ManifestErrorDto | null;
  warnings: ManifestErrorDto[];
};

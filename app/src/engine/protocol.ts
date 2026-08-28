/**
 * Application-side engine worker protocol (semantic).
 * Exact wire field names remain owned by the engine/protocol integration lane.
 * These types model what the GUI must send/receive; no transport is implied yet.
 */

export type ProtocolVersion = 1;

export type JobId = string;
export type RunId = string;
export type RequestId = string;

export type ComputationMode = "height" | "kernel" | "verify";

export type AxisMode = "h_plus_w" | "h_only" | "w_only";

export type SearchPreset = "integer_coarse" | "fractional_refine" | "custom";

export type EndpointRule = "inclusive" | "exclusive_stop";

export type MathMode = "raw" | "log_display";

export type BackendPreference = "cpu" | "cuda" | "vulkan" | "metal" | "auto";
export type ActualBackend = "cpu" | "cuda" | "vulkan" | "metal";

/** Decimal grid; candidates are resolved as exact decimal strings, not binary floats. */
export type CandidateGridSpec = {
  axis: "height" | "width" | "b" | "c" | "taps" | string;
  start: string;
  stop: string;
  step: string;
  endpointRule: EndpointRule;
  gridSemantics?: import("./types").GridSemantics;
  /** Fully resolved candidate sequence, or empty when only the content hash is stored. */
  candidates: string[];
  contentHash?: string | null;
  preset?: SearchPreset | null;
};

export type MetricSpec = {
  cropLeft: number;
  cropRight: number;
  cropTop: number;
  cropBottom: number;
  /** Pixel Exclusion Threshold applied before reduction (not Frame Review Threshold). */
  pixelExclusionThreshold: number;
  pNorm: number;
};

export type KernelRef = {
  id: string;
  parameters: Record<string, string | number | boolean>;
};

export type GeometrySnapshot = {
  mode: "standard" | "pro";
  /** Source shape this resolved geometry was measured against. */
  sourceWidth?: number;
  sourceHeight?: number;
  /** Compatibility aliases. New code must use srcWidth/srcHeight. */
  activeWidth: number;
  activeHeight: number;
  canvasWidth: number;
  canvasHeight: number;
  srcLeft: number;
  srcTop: number;
  srcWidth: number;
  srcHeight: number;
  baseWidth?: number | null;
  baseHeight?: number | null;
  parity?: "even" | "odd" | null;
  /** Historical geometry whose fractional meaning could not be proven. */
  needsReview?: boolean;
};

export type BaseMode = "integer" | "even" | "odd";

/** The complete geometry object sent over the worker wire. */
export type GeometryWire = {
  width: number;
  height: number;
  srcLeft: number;
  srcTop: number;
  srcWidth: number;
  srcHeight: number;
  baseWidth?: number | null;
  baseHeight?: number | null;
};

/** One engine-valid Height computation: one Sample, one fixed kernel, many heights. */
export type HeightAnalyzeRequest = {
  schemaVersion: ProtocolVersion;
  requestId: RequestId;
  mode: "height";
  sampleId: string;
  sampleFingerprint?: string | null;
  sourcePath: string;
  sourceFingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  kernel: KernelRef;
  axisMode: AxisMode;
  heightGrid: CandidateGridSpec;
  widthGrid?: CandidateGridSpec | null;
  geometry?: GeometrySnapshot | null;
  /** Fractional-scan base-canvas overrides as decimal integer strings. */
  baseHeight?: string | null;
  baseWidth?: string | null;
  metric: MetricSpec;
  profileId: string;
  mathMode: MathMode;
  backendPreference: BackendPreference;
};

/** One engine-valid Kernel computation: one Sample, one fixed geometry, many kernels. */
export type KernelAnalyzeRequest = {
  schemaVersion: ProtocolVersion;
  requestId: RequestId;
  mode: "kernel";
  sampleId: string;
  sampleFingerprint?: string | null;
  sourcePath: string;
  sourceFingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  geometry: GeometrySnapshot;
  axisMode: AxisMode;
  kernels: KernelRef[];
  metric: MetricSpec;
  profileId: string;
  mathMode: MathMode;
  backendPreference: BackendPreference;
};

export type ScanSelectionRule = "all" | "decoded_i_picture" | "every_n";

export type ScanScope = {
  streamIndex: number;
  selection: ScanSelectionRule;
  everyN?: number | null;
  startFrame?: number | null;
  endFrame?: number | null;
};

/** One engine-valid Verification: one Source, one locked Recipe snapshot, one ScanScope. */
export type VerifyRequest = {
  schemaVersion: ProtocolVersion;
  requestId: RequestId;
  mode: "verify";
  sourceId: string;
  sourcePath: string;
  sourceFingerprint?: string | null;
  recipeId: string;
  recipeRevision: number;
  geometry: GeometrySnapshot;
  kernel: KernelRef;
  metric: MetricSpec;
  axisMode: AxisMode;
  profileId: string;
  mathMode: MathMode;
  scanScope: ScanScope;
  backendPreference: BackendPreference;
  concurrency: number;
};

export type AnalyzeRequest = HeightAnalyzeRequest | KernelAnalyzeRequest | VerifyRequest;

export type WorkerEventBase = {
  protocolVersion: ProtocolVersion;
  requestId: RequestId;
  jobId: JobId;
  runId: RunId;
  timestampMs: number;
};

export type WorkerAcceptedEvent = WorkerEventBase & {
  type: "accepted";
  mode: ComputationMode;
  /** Verify mode (v1.1): engine-advised producer pacing hint. */
  suggestedInFlight?: number;
  workerCount?: number;
  concurrency?: number;
  /** Backend initialized and confirmed by the worker. Missing on older workers. */
  backend?: ActualBackend;
  device?: string;
};

/** One streamed verify frame result (v1.1 progress batches). */
export type VerifyFrameResult = {
  seq: number;
  /** Null marks a frame the engine failed to load; the job continues. */
  error: number | null;
  frameIndex?: number;
  pts?: number | null;
  timestampSeconds?: number | null;
};

/** Exact media verification coverage, available after presentation-order indexing. */
export type VerifyCoverage = {
  selection: ScanScope["selection"];
  eligibleFrames: number;
  selectedFrames: number;
  processedFrames: number;
  failedFrames: number;
};

export type WorkerProgressEvent = WorkerEventBase & {
  type: "progress";
  completed: number;
  total: number;
  detail?: string | null;
  /** Verify mode: batched per-frame results, possibly out of order. */
  results?: VerifyFrameResult[];
  coverage?: VerifyCoverage;
};

export type WorkerWarningEvent = WorkerEventBase & {
  type: "warning";
  code: string;
  message: string;
  from?: string;
  to?: string;
  reason?: string;
  frameSeq?: number;
};

/** Result payloads are engine-owned; the GUI stores them append-only and never fabricates them. */
export type WorkerResultEvent = WorkerEventBase & {
  type: "result";
  mode: ComputationMode;
  payload: unknown;
  coverage?: VerifyCoverage;
};

export type WorkerCancelledEvent = WorkerEventBase & {
  type: "cancelled";
  partial: boolean;
  coverage?: VerifyCoverage;
};

export type WorkerErrorEvent = WorkerEventBase & {
  type: "error";
  code: string;
  message: string;
  retryable: boolean;
};

export type WorkerEvent =
  | WorkerAcceptedEvent
  | WorkerProgressEvent
  | WorkerWarningEvent
  | WorkerResultEvent
  | WorkerCancelledEvent
  | WorkerErrorEvent;

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

export type BackendPreference = "cpu" | "cuda" | "metal" | "auto";

/** Decimal grid; candidates are resolved as exact decimal strings, not binary floats. */
export type CandidateGridSpec = {
  axis: "height" | "width" | "b" | "c" | "taps" | string;
  start: string;
  stop: string;
  step: string;
  endpointRule: EndpointRule;
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
  /**
   * Fractional-scan context (engine worker v1.1): explicit base-canvas
   * overrides as decimal strings. Choosing an odd base height/width is how
   * the user explores base parity (the reverse-peak check). Engine v1 ignores
   * these (fixed shift=0, floor canvas); the semantic contract carries them
   * so v1.1 wiring needs no app-side reshaping.
   */
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
  profileId: string;
  mathMode: MathMode;
  scanScope: ScanScope;
  backendPreference: BackendPreference;
};

export type AnalyzeRequest = HeightAnalyzeRequest | KernelAnalyzeRequest | VerifyRequest;

export type WorkerClientCommand =
  | { type: "hello"; protocolVersion: ProtocolVersion }
  | { type: "analyze"; request: AnalyzeRequest }
  | { type: "cancel"; requestId: RequestId; jobId?: JobId | null }
  | { type: "shutdown" };

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
};

/** One streamed verify frame result (v1.1 progress batches). */
export type VerifyFrameResult = {
  seq: number;
  /** Null marks a frame the engine failed to load; the job continues. */
  error: number | null;
};

export type WorkerProgressEvent = WorkerEventBase & {
  type: "progress";
  completed: number;
  total: number;
  detail?: string | null;
  /** Verify mode: batched per-frame results, possibly out of order. */
  results?: VerifyFrameResult[];
};

export type WorkerWarningEvent = WorkerEventBase & {
  type: "warning";
  code: string;
  message: string;
};

/** Result payloads are engine-owned; the GUI stores them append-only and never fabricates them. */
export type WorkerResultEvent = WorkerEventBase & {
  type: "result";
  mode: ComputationMode;
  payload: unknown;
};

export type WorkerCancelledEvent = WorkerEventBase & {
  type: "cancelled";
  partial: boolean;
};

export type WorkerErrorEvent = WorkerEventBase & {
  type: "error";
  code: string;
  message: string;
  retryable: boolean;
};

export type WorkerHelloEvent = {
  type: "hello_ok";
  protocolVersion: ProtocolVersion;
  engineVersion: string;
  commands: {
    analyze: boolean;
    cancel: boolean;
  };
};

export type WorkerEvent =
  | WorkerHelloEvent
  | WorkerAcceptedEvent
  | WorkerProgressEvent
  | WorkerWarningEvent
  | WorkerResultEvent
  | WorkerCancelledEvent
  | WorkerErrorEvent;

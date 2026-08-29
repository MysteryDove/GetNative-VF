import ReactDOM from "react-dom/client";
import { useMemo, useState } from "react";
import { ProjectShell } from "./components/ProjectShell";
import { createTranslator } from "./i18n";
import { emptyExecutionState, type ExecutionState, type JobRecord } from "./engine/runReducer";
import type { EngineEnvelope } from "./engine/types";
import type { ProjectRoute, ProjectState } from "./project/types";
import "./App.css";

const metric = {
  cropLeft: 5,
  cropRight: 5,
  cropTop: 5,
  cropBottom: 5,
  pixelExclusionThreshold: 0.015,
  pNorm: 1,
};

const initialState: ProjectState = {
  project: {
    id: "project-1",
    name: "Layout reproduction",
    schemaVersion: 2,
    createdAt: "2026-08-18T00:00:00Z",
    updatedAt: "2026-08-18T00:00:00Z",
    activeRecipeId: "recipe-1",
    untitled: true,
    storagePath: null,
    readOnly: false,
    dirty: false,
  },
  sourcesById: {
    "source-1": {
      id: "source-1",
      kind: "video",
      path: "C:/fixture.m2ts",
      fingerprint: "fixture",
      state: "ready",
      label: "fixture.m2ts",
      width: 1920,
      height: 1080,
      durationSeconds: 120,
      videoStreams: [],
      selectedStreamIndex: 0,
    },
  },
  samplesById: {
    "sample-1": {
      id: "sample-1",
      sourceId: "source-1",
      sourceFingerprint: "fixture",
      label: "fixture.m2ts #100",
      included: true,
      order: 0,
      frameIndex: 100,
      streamIndex: 0,
      tags: [],
    },
  },
  recipesById: {
    "recipe-1": {
      id: "recipe-1",
      name: "1536x864-bicubic",
      status: "draft",
      locked: false,
      revision: 1,
      parentRecipeId: null,
      createdAt: "2026-08-18T00:00:00Z",
      updatedAt: "2026-08-18T00:00:00Z",
      geometry: {
        activeHeight: 864,
        activeWidth: 1536,
        baseHeight: 864,
        baseWidth: null,
        canvasHeight: 864,
        canvasWidth: 1536,
        mode: "standard",
        parity: null,
        srcHeight: 864,
        srcLeft: 0,
        srcTop: 0,
        srcWidth: 1536,
      },
      kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
      metric,
      axisMode: "h_plus_w",
      profileId: "muf-d278cd3",
      mathMode: "raw",
    },
  },
  runGroupsById: {
    "group-1": {
      id: "group-1",
      memberRunIds: ["run-1"],
      groupType: "single_height",
      label: "Resolution Test",
      createdAt: "2026-08-18T00:00:00Z",
      intentSnapshot: null,
    },
  },
  runsById: {
    "run-1": {
      id: "run-1",
      runType: "height",
      status: "completed",
      runGroupId: "group-1",
      sampleId: "sample-1",
      sourceId: "source-1",
      createdAt: "2026-08-18T00:00:00Z",
      updatedAt: "2026-08-18T00:00:01Z",
      inputSnapshot: {
        metric,
        kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
      },
      result: {
        candidates: [
          { id: "862", error: 0.00033 },
          { id: "863", error: 0.00031 },
          { id: "864", error: 3.88e-8 },
          { id: "865", error: 0.00032 },
        ],
      },
      errorCode: null,
      errorMessage: null,
      completed: 4,
      total: 4,
    },
  },
  verificationReviewsByRunId: {},
  verificationFusionsById: {},
  uiStateByRoute: {},
};

const baseRecipe = initialState.recipesById["recipe-1"];
initialState.recipesById["recipe-2"] = {
  ...baseRecipe,
  id: "recipe-2",
  name: "1536x864-lanczos",
  status: "locked",
  locked: true,
  kernel: { id: "lanczos", parameters: { taps: 3 } },
};

const fullVerifyFrames = Array.from({ length: 34_072 }, (_, seq) => ({
  seq,
  frameIndex: seq,
  pts: seq * 1001,
  timestampSeconds: seq / 24,
  error: 1e-7 + ((seq * 17) % 97) * 1e-6,
}));
const previewVerifyFrames = Array.from({ length: 1_609 }, (_, seq) => ({
  seq,
  frameIndex: seq * 21,
  pts: seq * 21 * 1001,
  timestampSeconds: seq * 21 / 24,
  error: 2e-7 + ((seq * 31) % 89) * 1e-6,
}));

initialState.runGroupsById["verify-group"] = {
  id: "verify-group",
  memberRunIds: ["verify-full", "verify-preview"],
  groupType: "single_verification",
  label: "Coverage comparison",
  createdAt: "2026-08-18T00:01:00Z",
  intentSnapshot: null,
};
initialState.runsById["verify-full"] = {
  id: "verify-full",
  runType: "verification",
  status: "completed",
  runGroupId: "verify-group",
  sampleId: null,
  sourceId: "source-1",
  createdAt: "2026-08-18T00:02:00Z",
  updatedAt: "2026-08-18T00:03:00Z",
  inputSnapshot: {
    request: { recipeId: "recipe-1" },
    scanScope: { streamIndex: 0, selection: "all" },
  },
  result: {
    frames: fullVerifyFrames,
    coverage: {
      selection: "all",
      eligibleFrames: 34_072,
      selectedFrames: 34_072,
      processedFrames: 34_072,
      failedFrames: 0,
    },
  },
  errorCode: null,
  errorMessage: null,
  completed: 34_072,
  total: 34_072,
};
initialState.runsById["verify-preview"] = {
  id: "verify-preview",
  runType: "verification",
  status: "completed",
  runGroupId: "verify-group",
  sampleId: null,
  sourceId: "source-1",
  createdAt: "2026-08-18T00:01:00Z",
  updatedAt: "2026-08-18T00:02:00Z",
  inputSnapshot: {
    request: { recipeId: "recipe-2" },
    scanScope: { streamIndex: 0, selection: "decoded_i_picture" },
  },
  result: {
    frames: previewVerifyFrames,
    coverage: {
      selection: "decoded_i_picture",
      eligibleFrames: 34_072,
      selectedFrames: 1_609,
      processedFrames: 1_609,
      failedFrames: 0,
    },
  },
  errorCode: null,
  errorMessage: null,
  completed: 1_609,
  total: 1_609,
};

const capabilities: EngineEnvelope = {
  path: "fixture-engine",
  payload: {
    schema_version: 1,
    engine: "getnative",
    version: "fixture",
    commands: { capabilities: true, geometry: true, analyze: true },
    kernels: [
      { id: "bicubic", parameters: { b: 0, c: 0.5 } },
      { id: "bilinear", parameters: {} },
    ],
    backends: [
      {
        id: "cpu",
        compiled: true,
        device_available: true,
        analysis_command_available: true,
        axes: ["horizontal", "vertical", "both"],
        p_norms: { minimum: 1, maximum: 4 },
        max_half_bandwidth: null,
        max_forward_width: null,
      },
    ],
    profiles: [],
  },
};

/**
 * Jobs-tray overflow repro: completed jobs whose RunGroups no longer exist in
 * project state (e.g. their results were deleted), so the tray falls back to
 * raw rgrp_* id labels. Those long unbreakable strings used to inflate the
 * implicit auto column of .project-content past the container width and clip
 * the analyze params panel off the right edge; the pinned minmax(0, 1fr)
 * column lets the tray's own overflow-hidden list clip entries instead.
 */
function completedTrayJob(id: string, runGroupId: string): JobRecord {
  return {
    id,
    requestId: `req-${id}`,
    runId: `run-${id}`,
    runGroupId,
    projectRunId: null,
    mode: "height",
    phase: "completed",
    label: runGroupId,
    completed: 501,
    total: 501,
    warnings: [],
    cancelRequested: false,
    backend: "cuda",
    device: "NVIDIA GeForce RTX 5080",
    rateElapsedMs: 75,
    rateCompleted: 501,
    updatedAtMs: 0,
  };
}

const busyTrayExecution: ExecutionState = {
  ...emptyExecutionState(),
  jobsById: Object.fromEntries(
    [
      completedTrayJob("job-1", "group-1"),
      completedTrayJob("job-2", "rgrp_678070fa-6a3b-4d8b-be27-9ff10b2c3d4e"),
      completedTrayJob("job-3", "rgrp_7702301f-d64d-435c-886a-fcfc1c9a2b3d4e"),
      completedTrayJob("job-4", "rgrp_3f4a2b1c-9d8e-4c7b-a6f5-0e1d2c3b4a59"),
    ].map((job) => [job.id, job]),
  ),
};

function Harness() {
  const [state, setState] = useState(initialState);
  const [route, setRoute] = useState<ProjectRoute>(() =>
    new URLSearchParams(window.location.search).get("route") === "verify"
      ? "verify"
      : "analyze",
  );
  const execution = useMemo(() => busyTrayExecution, []);
  return (
    <ProjectShell
      t={createTranslator("zh-CN")}
      state={state}
      route={route}
      engineState="ready"
      enginePath="fixture-engine"
      engineError=""
      projectError={null}
      capabilities={capabilities}
      language="zh-CN"
      onLanguageChange={() => undefined}
      themeMode="system"
      onThemeChange={() => undefined}
      axisPlanCacheDir={null}
      onAxisPlanCacheDirChange={async () => undefined}
      onNavigate={setRoute}
      onClose={() => undefined}
      onSave={() => undefined}
      onProjectChange={(updater) => setState((current) => updater(current))}
      onEngineError={() => undefined}
      onGeometrySuccess={() => undefined}
      busy={false}
      execution={execution}
      executionBridge={{ queue: () => undefined, cancel: () => undefined }}
    />
  );
}

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(<Harness />);

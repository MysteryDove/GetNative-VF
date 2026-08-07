import { describe, it } from "vitest";
import {
  formatExactDecimal,
  integerCoarseGrid,
  resolveCandidateSequence,
  workEstimate,
} from "./candidateGrid";
import {
  rejectMixedHeightAndKernel,
  validateHeightShape,
  validateKernelShape,
  validateMetricSpec,
  validateVerifyShape,
} from "./shapeGuards";
import {
  emptyExecutionState,
  queueJob,
  reduceWorkerEvent,
  requestCancel,
  activeJobs,
} from "./runReducer";
import type { HeightAnalyzeRequest, KernelAnalyzeRequest, VerifyRequest } from "./protocol";
import {
  extractHeightSeries,
  materializeHeightRunGroup,
  metricCompatibilityKey,
  planHeightRunGroup,
} from "./runGroupPlan";
import { defaultHeightDraft } from "./heightDraft";

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

function sampleHeightRequest(overrides?: Partial<HeightAnalyzeRequest>): HeightAnalyzeRequest {
  return {
    schemaVersion: 1,
    requestId: "req_1",
    mode: "height",
    sampleId: "smp_1",
    sourcePath: "/tmp/a.png",
    kernel: { id: "bicubic", parameters: { b: 1 / 3, c: 1 / 3 } },
    axisMode: "h_only",
    heightGrid: {
      axis: "height",
      start: "500",
      stop: "502",
      step: "1",
      endpointRule: "inclusive",
      candidates: ["500", "501", "502"],
      preset: "integer_coarse",
    },
    metric: {
      cropLeft: 0,
      cropRight: 0,
      cropTop: 0,
      cropBottom: 0,
      pixelExclusionThreshold: 0,
      pNorm: 2,
    },
    profileId: "muf-d278cd3",
    mathMode: "raw",
    backendPreference: "cpu",
    ...overrides,
  };
}

describe("GUI-3 analysis foundation", () => {
  it("resolves exact candidate grids and work estimates", () => {
    const integer = integerCoarseGrid({ start: 500, stop: 504, step: 2 });
    assert(integer.ok, "integer grid ok");
    assert(
      integer.grid.candidates.join(",") === "500,502,504",
      `integer candidates: ${integer.grid.candidates.join(",")}`,
    );

    const tenths = resolveCandidateSequence({
      start: "719.5",
      stop: "720.5",
      step: "0.5",
      endpointRule: "inclusive",
    });
    assert(tenths.ok, "fractional grid ok");
    assert(tenths.candidates.join(",") === "719.5,720,720.5", tenths.candidates.join(","));

    assert(formatExactDecimal(720, "1") === "720", "integer format");
    assert(workEstimate({ sampleCount: 2, fixedKernelCount: 3, candidateCount: 10 }) === 60, "work");

    const empty = resolveCandidateSequence({
      start: "10",
      stop: "10",
      step: "1",
      endpointRule: "exclusive_stop",
    });
    assert(!empty.ok, "exclusive empty at single point");
  });

  it("enforces engine computation shape guards", () => {
    assert(validateHeightShape(sampleHeightRequest()).ok, "valid height");
    assert(
      !validateHeightShape(sampleHeightRequest({ sampleId: "" })).ok,
      "height needs sample",
    );
    assert(
      !validateHeightShape(
        sampleHeightRequest({
          heightGrid: {
            axis: "height",
            start: "720",
            stop: "720",
            step: "1",
            endpointRule: "inclusive",
            candidates: ["720"],
          },
        }),
      ).ok,
      "height needs >=2 candidates",
    );

    const kernel: KernelAnalyzeRequest = {
      schemaVersion: 1,
      requestId: "req_k",
      mode: "kernel",
      sampleId: "smp_1",
      sourcePath: "/tmp/a.png",
      geometry: {
        mode: "standard",
        activeWidth: 1280,
        activeHeight: 720,
        canvasWidth: 1280,
        canvasHeight: 720,
        srcLeft: 0,
        srcTop: 0,
        srcWidth: 1920,
        srcHeight: 1080,
      },
      kernels: [
        { id: "bilinear", parameters: {} },
        { id: "bicubic", parameters: { b: 0, c: 0.5 } },
      ],
      metric: sampleHeightRequest().metric,
      profileId: "muf-d278cd3",
      mathMode: "raw",
      backendPreference: "auto",
    };
    assert(validateKernelShape(kernel).ok, "valid kernel");
    assert(!validateKernelShape({ ...kernel, kernels: [kernel.kernels[0]] }).ok, "kernel needs 2+");

    const verify: VerifyRequest = {
      schemaVersion: 1,
      requestId: "req_v",
      mode: "verify",
      sourceId: "src_1",
      sourcePath: "/tmp/v.mkv",
      recipeId: "recipe_1",
      recipeRevision: 1,
      geometry: kernel.geometry,
      kernel: { id: "bicubic", parameters: { b: 1 / 3, c: 1 / 3 } },
      metric: sampleHeightRequest().metric,
      profileId: "muf-d278cd3",
      mathMode: "raw",
      scanScope: { streamIndex: 0, selection: "all" },
      backendPreference: "cpu",
    };
    assert(validateVerifyShape(verify).ok, "valid verify");
    assert(
      !validateVerifyShape({
        ...verify,
        scanScope: { streamIndex: 0, selection: "every_n", everyN: 0 },
      }).ok,
      "every_n invalid",
    );

    assert(!rejectMixedHeightAndKernel({ heightCandidateCount: 3, kernelCandidateCount: 2 }).ok, "mixed");
    assert(rejectMixedHeightAndKernel({ heightCandidateCount: 3, kernelCandidateCount: 1 }).ok, "height ok");
    assert(!validateMetricSpec({ ...sampleHeightRequest().metric, pNorm: 0 }).ok, "p-norm");
  });

  it("reduces queue, progress, cancel, result, and error without fabricating payloads", () => {
    let state = emptyExecutionState();
    state = queueJob(state, {
      jobId: "job_1",
      requestId: "req_1",
      runId: "run_1",
      mode: "height",
      label: "Resolution Test",
      total: 10,
      inputSnapshotKey: "snap_1",
      nowMs: 1,
    });
    assert(activeJobs(state).length === 1, "queued is active");

    state = reduceWorkerEvent(state, {
      type: "accepted",
      protocolVersion: 1,
      requestId: "req_1",
      jobId: "job_1",
      runId: "run_1",
      timestampMs: 2,
      mode: "height",
    });
    assert(state.jobsById.job_1.phase === "running", "accepted -> running");

    state = reduceWorkerEvent(state, {
      type: "progress",
      protocolVersion: 1,
      requestId: "req_1",
      jobId: "job_1",
      runId: "run_1",
      timestampMs: 3,
      completed: 4,
      total: 10,
    });
    assert(state.jobsById.job_1.completed === 4, "progress");

    state = requestCancel(state, { requestId: "req_1", nowMs: 4 });
    assert(state.jobsById.job_1.cancelRequested, "cancel requested");

    state = reduceWorkerEvent(state, {
      type: "cancelled",
      protocolVersion: 1,
      requestId: "req_1",
      jobId: "job_1",
      runId: "run_1",
      timestampMs: 5,
      partial: true,
    });
    assert(state.jobsById.job_1.phase === "partial", "partial cancel");
    assert(state.runsById.run_1.result === null, "no fabricated result on cancel");

    state = queueJob(state, {
      jobId: "job_2",
      requestId: "req_2",
      runId: "run_2",
      mode: "height",
      label: "Resolution Test",
      total: 3,
      inputSnapshotKey: "snap_2",
      nowMs: 6,
    });
    const enginePayload = { series: [{ height: "720", metric: 1.2 }] };
    state = reduceWorkerEvent(state, {
      type: "result",
      protocolVersion: 1,
      requestId: "req_2",
      jobId: "job_2",
      runId: "run_2",
      timestampMs: 7,
      mode: "height",
      payload: enginePayload,
    });
    assert(state.runsById.run_2.status === "completed", "result completes");
    assert(state.runsById.run_2.result === enginePayload, "engine payload preserved");

    state = queueJob(state, {
      jobId: "job_3",
      requestId: "req_3",
      runId: "run_3",
      mode: "height",
      label: "Resolution Test",
      total: 1,
      inputSnapshotKey: "snap_3",
      nowMs: 8,
    });
    state = reduceWorkerEvent(state, {
      type: "error",
      protocolVersion: 1,
      requestId: "req_3",
      jobId: "job_3",
      runId: "run_3",
      timestampMs: 9,
      code: "engine_failed",
      message: "backend error",
      retryable: true,
    });
    assert(state.jobsById.job_3.phase === "failed", "error fails job");
    assert(state.runsById.run_3.result === null, "error does not invent result");
  });

  it("plans multi-sample multi-kernel Height RunGroups with immutable snapshots", () => {
    const draft = defaultHeightDraft(null);
    draft.compareCommonKernels = true;
    draft.start = "720";
    draft.stop = "722";
    draft.step = "1";
    draft.kernelId = "bicubic";

    const planned = planHeightRunGroup({
      draft,
      samples: [
        {
          id: "smp_a",
          label: "A",
          sourceId: "src_1",
          sourceFingerprint: "fp1",
          included: true,
          frameIndex: 10,
          streamIndex: 0,
        },
        {
          id: "smp_b",
          label: "B",
          sourceId: "src_1",
          sourceFingerprint: "fp1",
          included: true,
          frameIndex: 20,
          streamIndex: 0,
        },
      ],
      sourcesById: {
        src_1: { id: "src_1", path: "/tmp/v.mkv", fingerprint: "fp1", state: "ready" },
      },
      capabilities: {
        path: "/engine",
        payload: {
          schema_version: 2,
          engine: "getnative-engine",
          version: "0",
          commands: { capabilities: true, geometry: true, analyze: false },
          kernels: [
            { id: "bicubic", parameters: { b: 1 / 3, c: 1 / 3 } },
            { id: "bilinear", parameters: {} },
            { id: "lanczos", parameters: { taps: 3 } },
            { id: "spline36", parameters: {} },
          ],
          backends: [],
          profiles: [{ id: "muf-d278cd3", default_crop: 0 }],
        },
      },
      nowMs: 1000,
      requestIdPrefix: "test",
    });
    assert(planned.ok, "plan ok");
    if (!planned.ok) return;
    assert(planned.plan.memberCount === 8, `members ${planned.plan.memberCount}`);
    assert(planned.plan.groupType === "multi_sample_multi_kernel_height", planned.plan.groupType);
    assert(planned.plan.workEstimate === 8 * 3, "work estimate");
    assert(
      planned.plan.members.every((member) => member.request.mode === "height"),
      "each member is height-shaped",
    );

    let seq = 0;
    const material = materializeHeightRunGroup({
      plan: planned.plan,
      idFactory: () => String(++seq),
      nowIso: "2026-08-01T00:00:00.000Z",
    });
    assert(material.runs.length === 8, "materialized runs");
    assert(material.runGroup.memberRunIds.length === 8, "group members");
    assert(
      material.runs.every((run) => run.result === null && run.status === "queued"),
      "no fabricated results",
    );
    assert(
      material.runs.every((run) => run.inputSnapshot != null && run.runGroupId === material.runGroup.id),
      "immutable snapshots linked",
    );

    const series = extractHeightSeries({
      series: [
        { height: "720", metric: 1.5 },
        { height: "721", metric: 0.2 },
      ],
    });
    assert(series?.length === 2, "extract real series");
    assert(extractHeightSeries({ fake: true }) === null, "reject unknown shapes");
    assert(
      metricCompatibilityKey(draft.metric) === metricCompatibilityKey({ ...draft.metric }),
      "metric key stable",
    );
    assert(
      metricCompatibilityKey(draft.metric) !==
        metricCompatibilityKey({ ...draft.metric, pNorm: 2 }),
      "metric key changes with p-norm",
    );
  });
});

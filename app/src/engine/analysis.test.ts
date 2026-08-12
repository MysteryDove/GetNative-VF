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
  runGroupProgress,
} from "./runReducer";
import type { HeightAnalyzeRequest, KernelAnalyzeRequest, VerifyRequest } from "./protocol";
import type { EngineEnvelope } from "./types";
import { createTranslator } from "../i18n";
import { backendOptionLabel, verifySelectableBackends } from "./backendSelection";
import {
  extractHeightSeries,
  materializeHeightRunGroup,
  metricCompatibilityKey,
  planHeightRunGroup,
} from "./runGroupPlan";
import {
  applyProfileDefaults,
  defaultHeightDraft,
  fixedKernelsForDraft,
  kernelSignature,
  resolveBackendPreference,
  resolveHeightGrid,
  selectableBackends,
  validateBackendPNorm,
} from "./heightDraft";

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

    const hundredths = integerCoarseGrid({ start: 500, stop: 501, step: 0.01 });
    assert(hundredths.ok, "integer coarse grid accepts a 0.01 step");
    assert(hundredths.grid.step === "0.01", `hundredths step: ${hundredths.grid.step}`);
    assert(hundredths.grid.candidates.length === 101, "0.01 step candidate count");
    assert(
      hundredths.grid.candidates[1] === "500.01" &&
        hundredths.grid.candidates[hundredths.grid.candidates.length - 1] === "501",
      `hundredths candidates: ${hundredths.grid.candidates.join(",")}`,
    );

    // App-side cap matches the worker protocol (100k): 500-800 @ 0.01 must pass.
    const wide = integerCoarseGrid({ start: 500, stop: 800, step: 0.01 });
    assert(wide.ok, "integer coarse grid accepts 500-800 @ 0.01");
    assert(wide.grid.candidates.length === 30001, `wide count: ${wide.grid.candidates.length}`);

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

    const repeated = resolveCandidateSequence({
      start: "0.01", stop: "0.19", step: "0.01", endpointRule: "inclusive",
      gridSemantics: "repeated_addition",
    });
    const indexed = resolveCandidateSequence({
      start: "0.01", stop: "0.19", step: "0.01", endpointRule: "inclusive",
      gridSemantics: "index_multiplication",
    });
    const fixed = resolveCandidateSequence({
      start: "0.01", stop: "0.19", step: "0.01", endpointRule: "inclusive",
      gridSemantics: "decimal_fixed_point",
    });
    assert(
      repeated.ok && repeated.candidates[repeated.candidates.length - 1] === "0.18",
      "MUF repeated-addition stop",
    );
    assert(
      indexed.ok && indexed.candidates[indexed.candidates.length - 1] === "0.19",
      "getnative indexed stop",
    );
    assert(
      fixed.ok && fixed.candidates[fixed.candidates.length - 1] === "0.19",
      "modern fixed-point stop",
    );
  });

  it("uses MUF defaults and only resets edits through the explicit profile action", () => {
    const draft = defaultHeightDraft(null);
    assert(draft.profileId === "muf-d278cd3", "MUF is the default profile");
    assert(draft.axisMode === "h_plus_w", "MUF defaults to H+W");
    assert(draft.start === "500" && draft.stop === "1000" && draft.step === "1", "MUF grid");
    assert(draft.endpointRule === "inclusive", "MUF endpoint");
    assert(draft.metric.cropLeft === 5 && draft.metric.cropBottom === 5, "MUF crop");
    assert(draft.metric.pixelExclusionThreshold === 0.015, "MUF threshold");
    assert(draft.kernelParameters.b === 0 && draft.kernelParameters.c === 0.5, "MUF bicubic");
    assert(draft.baseHeight === "" && draft.baseWidth === "", "MUF bases unspecified");

    const edited = { ...draft, profileId: "modern", start: "701", axisMode: "h_only" as const };
    assert(edited.start === "701" && edited.axisMode === "h_only", "profile selection preserves edits");
    const reset = applyProfileDefaults(edited, null);
    assert(reset.profileId === "modern" && reset.start === "500", "explicit reset applies profile grid");
    assert(reset.axisMode === "h_plus_w", "explicit reset applies profile axis");

    const fractional = resolveHeightGrid({
      ...draft,
      preset: "fractional_refine",
      refineSelected: "720",
      refineHalfSpan: "0.5",
      step: "0.5",
      endpointRule: "exclusive_stop",
    });
    assert(fractional.ok, "fractional grid resolves");
    if (fractional.ok) {
      assert(fractional.grid.endpointRule === "exclusive_stop", "fractional endpoint is preserved");
      assert(fractional.grid.candidates.join(",") === "719.5,720", "exclusive fractional endpoint");
    }
  });

  it("gates p-norm by the resolved backend", () => {
    assert(validateBackendPNorm(null, "cpu", 2).ok, "CPU accepts p=2");
    assert(validateBackendPNorm(null, "auto", 2).ok, "auto falls back to CPU without CUDA");
    assert(validateBackendPNorm(null, "cuda", 4).ok, "CUDA fallback contract accepts p=4");
    assert(!validateBackendPNorm(null, "cuda", 5).ok, "CUDA fallback contract rejects p=5");
    assert(validateBackendPNorm(null, "vulkan", 1).ok, "Vulkan accepts p=1");
    assert(!validateBackendPNorm(null, "vulkan", 2).ok, "Vulkan rejects p=2");
    assert(!validateBackendPNorm(null, "cpu", 0).ok, "zero is invalid");
    assert(!validateBackendPNorm(null, "cpu", 4_294_967_296).ok, "uint32 overflow invalid");

    const cudaCapabilities: EngineEnvelope = {
      path: "/engine",
      payload: {
        schema_version: 1,
        engine: "getnative-engine",
        version: "0",
        commands: { capabilities: true, geometry: true, analyze: true },
        kernels: [],
        profiles: [],
        backends: [{
          id: "cuda",
          compiled: true,
          device_available: true,
          analysis_command_available: true,
          axes: ["horizontal", "vertical"],
          p_norms: { minimum: 1, maximum: 1 },
          max_half_bandwidth: null,
          max_forward_width: null,
          device: "NVIDIA GeForce RTX 5080",
        }],
      },
    };
    assert(resolveBackendPreference(cudaCapabilities, "auto") === "cuda", "auto prefers CUDA");
    assert(resolveBackendPreference(cudaCapabilities, "auto", 2) === "cpu", "auto falls back for p>1");
    assert(validateBackendPNorm(cudaCapabilities, "auto", 2).ok, "CPU fallback accepts p>1");
    assert(!validateBackendPNorm(cudaCapabilities, "cuda", 2).ok, "legacy CUDA maximum=1 is preserved");

    const cudaPNorms = cudaCapabilities.payload.backends[0].p_norms;
    assert(cudaPNorms !== null, "CUDA p-norm range is reported");
    cudaPNorms.maximum = 4;
    assert(resolveBackendPreference(cudaCapabilities, "auto", 4) === "cuda", "auto uses CUDA for p=4");
    assert(validateBackendPNorm(cudaCapabilities, "cuda", 4).ok, "reported CUDA accepts p=4");
    assert(resolveBackendPreference(cudaCapabilities, "auto", 5) === "cpu", "auto falls back for p=5");

    cudaCapabilities.payload.backends.push({
      id: "vulkan",
      compiled: true,
      device_available: true,
      analysis_command_available: true,
      axes: ["horizontal", "vertical", "both"],
      p_norms: { minimum: 1, maximum: 1 },
      max_half_bandwidth: null,
      max_forward_width: null,
      device: "Discrete Vulkan GPU",
      device_type: "discrete_gpu",
      auto_priority: 20,
    });
    assert(selectableBackends(cudaCapabilities).includes("vulkan"), "available Vulkan is selectable");
    assert(validateBackendPNorm(cudaCapabilities, "vulkan", 1).ok, "reported Vulkan accepts p=1");
    assert(!validateBackendPNorm(cudaCapabilities, "vulkan", 2).ok, "reported Vulkan rejects p=2");
    assert(resolveBackendPreference(cudaCapabilities, "auto") === "cuda", "auto keeps CUDA first");

    cudaCapabilities.payload.backends[0].auto_priority = null;
    assert(resolveBackendPreference(cudaCapabilities, "auto") === "vulkan", "discrete Vulkan follows CUDA");
    assert(resolveBackendPreference(cudaCapabilities, "auto", 2) === "cpu", "Vulkan p>1 uses CPU");
    cudaCapabilities.payload.backends[1].device_type = "integrated_gpu";
    cudaCapabilities.payload.backends[1].auto_priority = null;
    assert(resolveBackendPreference(cudaCapabilities, "auto") === "cpu", "integrated Vulkan is explicit-only");
    assert(selectableBackends(cudaCapabilities).includes("vulkan"), "integrated Vulkan stays explicit");

    const t = createTranslator("en");
    assert(backendOptionLabel(t, "auto", null) === "Auto (Detecting…)", "loading label");
    cudaCapabilities.payload.backends[0].auto_priority = 10;
    assert(
      backendOptionLabel(t, "auto", cudaCapabilities, 1)
        === "Auto (CUDA · NVIDIA GeForce RTX 5080)",
      "Auto label includes the complete device name",
    );
    assert(
      backendOptionLabel(t, "auto", cudaCapabilities, 1, undefined, true) === "Auto (CPU)",
      "Legacy verify Auto is CPU-only",
    );
    assert(
      verifySelectableBackends(cudaCapabilities).join(",") === "auto,cpu",
      "Verify options stay CPU-only without engine decode",
    );
    const decodeCapabilities: typeof cudaCapabilities = {
      ...cudaCapabilities,
      payload: {
        ...cudaCapabilities.payload,
        features: { verify_engine_decode: true },
      },
    };
    assert(
      verifySelectableBackends(decodeCapabilities).join(",") === "auto,cuda,vulkan",
      "media verify unlocks the reported GPU backends",
    );
    assert(
      backendOptionLabel(t, "auto", decodeCapabilities, 1, undefined, false)
        === "Auto (CUDA · NVIDIA GeForce RTX 5080)",
      "media verify Auto resolves like analysis",
    );
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
      axisMode: "h_plus_w",
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
      axisMode: "h_only",
      profileId: "muf-d278cd3",
      mathMode: "raw",
      scanScope: { streamIndex: 0, selection: "all" },
      backendPreference: "cpu",
      concurrency: 2,
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
      backend: "cuda",
      device: "NVIDIA GeForce RTX 5080",
    });
    assert(state.jobsById.job_1.phase === "running", "accepted -> running");
    assert(state.jobsById.job_1.backend === "cuda", "accepted stores actual backend");
    assert(state.jobsById.job_1.device === "NVIDIA GeForce RTX 5080", "accepted stores device");

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

  it("isolates candidate throughput from plan progress and keeps it monotonic", () => {
    let state = emptyExecutionState();
    state = queueJob(state, {
      jobId: "job_fps",
      requestId: "req_fps",
      runId: "run_fps",
      mode: "height",
      label: "Resolution Test",
      total: 100,
      inputSnapshotKey: "snap_fps",
      nowMs: 1000,
    });
    state = reduceWorkerEvent(state, {
      type: "accepted",
      protocolVersion: 1,
      requestId: "req_fps",
      jobId: "job_fps",
      runId: "run_fps",
      timestampMs: 2000,
      mode: "height",
    });
    const progress = (
      timestampMs: number,
      completed: number,
      total: number,
      detail: "plan" | "candidates",
    ): Parameters<typeof reduceWorkerEvent>[1] => ({
      type: "progress",
      protocolVersion: 1,
      requestId: "req_fps",
      jobId: "job_fps",
      runId: "run_fps",
      timestampMs,
      completed,
      total,
      detail,
    });
    state = reduceWorkerEvent(state, progress(2100, 64, 200, "plan"));
    state = reduceWorkerEvent(state, progress(2200, 200, 200, "plan"));
    let job = state.jobsById.job_fps;
    assert(job.completed === 0 && job.total === 100, "plan progress does not count candidates");
    assert(job.fpsAvg == null, "plan progress has no candidate rate");

    state = reduceWorkerEvent(state, progress(2300, 32, 100, "candidates"));
    job = state.jobsById.job_fps;
    assert(job.fpsCurrent == null, "first progress only anchors the interval");
    assert(job.fpsAvg === 320, `avg after first candidate chunk: ${job.fpsAvg}`);

    // Same-millisecond bursts accumulate until a measurable interval exists.
    state = reduceWorkerEvent(state, progress(2300, 64, 100, "candidates"));
    state = reduceWorkerEvent(state, progress(2400, 96, 100, "candidates"));
    job = state.jobsById.job_fps;
    assert(job.fpsCurrent === 640, `current candidate rate: ${job.fpsCurrent}`);
    assert(job.fpsAvg === 480, `average candidate rate: ${job.fpsAvg}`);

    // A delayed lower completion event cannot move progress or timing backwards.
    state = reduceWorkerEvent(state, progress(2390, 80, 100, "candidates"));
    job = state.jobsById.job_fps;
    assert(job.completed === 96, `monotonic completed count: ${job.completed}`);
    assert(job.rateElapsedMs === 200, `monotonic elapsed time: ${job.rateElapsedMs}`);

    state = reduceWorkerEvent(state, {
      type: "result",
      protocolVersion: 1,
      requestId: "req_fps",
      jobId: "job_fps",
      runId: "run_fps",
      timestampMs: 2500,
      mode: "height",
      payload: {
        candidates: Array.from({ length: 100 }, (_, index) => ({ id: String(index), error: 0 })),
        telemetry: { candidates_ms: 250 },
      },
    });
    job = state.jobsById.job_fps;
    assert(job.fpsAvg === 400, `terminal telemetry rate: ${job.fpsAvg}`);
    assert(job.rateElapsedMs === 250, `terminal telemetry elapsed: ${job.rateElapsedMs}`);
  });

  it("aggregates member jobs into RunGroup progress", () => {
    let state = emptyExecutionState();
    const enqueue = (st: typeof state, suffix: string) =>
      queueJob(st, {
        jobId: `job_${suffix}`,
        requestId: `req_${suffix}`,
        runId: `run_${suffix}`,
        runGroupId: "rgrp_1",
        mode: "height",
        label: "Resolution Test",
        total: 100,
        inputSnapshotKey: `snap_${suffix}`,
        nowMs: 1000,
      });
    state = enqueue(state, "a");
    state = enqueue(state, "b");
    for (const suffix of ["a", "b"]) {
      state = reduceWorkerEvent(state, {
        type: "accepted",
        protocolVersion: 1,
        requestId: `req_${suffix}`,
        jobId: `job_${suffix}`,
        runId: `run_${suffix}`,
        timestampMs: 1100,
        mode: "height",
        backend: "vulkan",
        device: "Discrete Vulkan GPU",
      });
    }
    const event = (
      suffix: string,
      timestampMs: number,
      completed: number,
      detail: "plan" | "candidates",
    ): Parameters<typeof reduceWorkerEvent>[1] => ({
      type: "progress",
      protocolVersion: 1,
      requestId: `req_${suffix}`,
      jobId: `job_${suffix}`,
      runId: `run_${suffix}`,
      timestampMs,
      completed,
      total: 100,
      detail,
    });
    state = reduceWorkerEvent(state, event("a", 1500, 100, "plan"));
    state = reduceWorkerEvent(state, event("a", 2000, 40, "candidates"));
    state = reduceWorkerEvent(state, event("b", 2500, 100, "plan"));
    state = reduceWorkerEvent(state, event("b", 3000, 50, "candidates"));

    const groups = runGroupProgress(state);
    assert(groups.length === 1, `one group: ${groups.length}`);
    const group = groups[0];
    assert(group.id === "rgrp_1", "group id");
    assert(group.completed === 90 && group.total === 200, "summed progress");
    assert(group.phase === "running", "any running member -> running");
    assert(group.activeJobIds.length === 2, "both members active");
    assert(group.backend === "vulkan", `actual backend: ${group.backend}`);
    assert(group.device === "Discrete Vulkan GPU", `actual device: ${group.device}`);
    // avg = 90 candidates / (500ms + 500ms measured candidate work).
    assert(group.fpsAvg === 90, `group avg fps: ${group.fpsAvg}`);
    assert(group.rateUnit === "candidates", `group rate unit: ${group.rateUnit}`);
  });

  it("plans multi-sample multi-kernel Height RunGroups with immutable snapshots", () => {
    const draft = defaultHeightDraft(null);
    draft.compareKernels = [
      { id: "bilinear", parameters: {} },
      { id: "lanczos", parameters: { taps: 3 } },
      { id: "spline36", parameters: {} },
    ];
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

  it("fixedKernelsForDraft puts the fixed kernel first and dedupes compare picks", () => {
    const draft = defaultHeightDraft(null);
    draft.kernelId = "bicubic";
    draft.compareKernels = [
      { id: "bicubic", parameters: { b: 0, c: 0.5 } }, // same as primary -> dropped
      { id: "lanczos", parameters: { taps: 2 } },
      { id: "lanczos", parameters: { taps: 3 } }, // taps variants are distinct
      { id: "lanczos", parameters: { taps: 3 } }, // exact duplicate → dropped
    ];
    const kernels = fixedKernelsForDraft(draft, null);
    assert(
      kernels.map((kernel) => kernelSignature(kernel)).join(",") ===
        "bicubic:{\"b\":0,\"c\":0.5},lanczos:{\"taps\":2},lanczos:{\"taps\":3}",
      `kernels: ${kernels.map((kernel) => kernelSignature(kernel)).join(",")}`,
    );

    draft.compareKernels = [];
    assert(fixedKernelsForDraft(draft, null).length === 1, "no compare picks → fixed only");
  });
});

describe("height plan base-canvas overrides (engine v1.1 contract)", () => {
  const baseSamples = [
    {
      id: "smp_a",
      label: "A",
      sourceId: "src_1",
      sourceFingerprint: "fp1",
      included: true,
      frameIndex: 10,
      streamIndex: 0,
    },
  ];
  const baseSources = {
    src_1: { id: "src_1", path: "/tmp/v.mkv", fingerprint: "fp1", state: "ready" },
  };

  it("carries baseHeight/baseWidth into every member request and intent snapshot", () => {
    const draft = {
      ...defaultHeightDraft(null),
      preset: "fractional_refine" as const,
      refineSelected: "837",
      refineHalfSpan: "0.5",
      step: "0.1",
      baseHeight: "1001",
      baseWidth: "2001",
    };
    const planned = planHeightRunGroup({
      draft,
      samples: baseSamples,
      sourcesById: baseSources,
      capabilities: null,
      nowMs: 1,
      requestIdPrefix: "t",
    });
    assert(planned.ok, "plan ok");
    if (!planned.ok) return;
    assert(
      planned.plan.members.every(
        (member) => member.request.baseHeight === "1001" && member.request.baseWidth === "2001",
      ),
      "base overrides reach every request",
    );
    assert(planned.plan.intentSnapshot.baseHeight === "1001", "intent snapshot base height");
    assert(
      planned.plan.members[0]?.heightGrid.candidates.includes("836.5"),
      "fractional grid resolves decimal candidates",
    );
  });

  it("rejects malformed base overrides and defaults to null", () => {
    const bad = planHeightRunGroup({
      draft: { ...defaultHeightDraft(null), baseHeight: "abc" },
      samples: baseSamples,
      sourcesById: baseSources,
      capabilities: null,
    });
    assert(!bad.ok && bad.reason === "base_invalid", "malformed base rejected");

    const fractionalBase = planHeightRunGroup({
      draft: { ...defaultHeightDraft(null), baseHeight: "720.5" },
      samples: baseSamples,
      sourcesById: baseSources,
      capabilities: null,
    });
    assert(!fractionalBase.ok && fractionalBase.reason === "base_invalid", "fractional base rejected");

    const clean = planHeightRunGroup({
      draft: defaultHeightDraft(null),
      samples: baseSamples,
      sourcesById: baseSources,
      capabilities: null,
      nowMs: 1,
    });
    assert(clean.ok, "clean plan ok");
    if (clean.ok) {
      assert(clean.plan.members[0]?.request.baseHeight === null, "empty base -> null");
    }
  });
});

import { describe, expect, it } from "vitest";
import { buildVerificationFusion, fusionEligibility } from "./verificationFusion";
import { buildVerificationFusionCsv, buildVerificationFusionJson } from "./export";
import type { ProjectState } from "./types";

const metric = {
  cropLeft: 0,
  cropRight: 0,
  cropTop: 0,
  cropBottom: 0,
  pixelExclusionThreshold: 0.01,
  pNorm: 1,
};

function state(): ProjectState {
  const source = {
    id: "src",
    kind: "video" as const,
    path: "video.m2ts",
    fingerprint: "fp",
    state: "ready" as const,
    label: "Video",
    videoStreams: [],
    selectedStreamIndex: 0,
  };
  const recipe = (id: string, createdAt: string) => ({
    id,
    name: id,
    status: "locked" as const,
    locked: true,
    revision: 1,
    parentRecipeId: null,
    createdAt,
    updatedAt: createdAt,
    geometry: { mode: "standard" as const, activeWidth: 10, activeHeight: 10, canvasWidth: 10, canvasHeight: 10, srcLeft: 0, srcTop: 0, srcWidth: 10, srcHeight: 10 },
    kernel: { id: "k", parameters: {} },
    metric,
    axisMode: "h_plus_w" as const,
    profileId: "p",
    mathMode: "raw" as const,
  });
  const run = (id: string, recipeId: string, errors: Array<[number, number]>) => ({
    id,
    runType: "verification",
    status: "completed",
    runGroupId: null,
    sampleId: null,
    sourceId: "src",
    createdAt: "2026-01-01T00:00:00Z",
    updatedAt: "2026-01-01T00:00:00Z",
    inputSnapshot: {
      request: {
        sourceId: "src",
        sourceFingerprint: "fp",
        recipeId,
        recipeRevision: 1,
        metric,
        axisMode: "h_plus_w",
        profileId: "p",
        mathMode: "raw",
        scanScope: { streamIndex: 0, selection: "all" },
      },
    },
    result: {
      frames: errors.map(([frameIndex, error]) => ({ frameIndex, error })),
      coverage: { selection: "all", eligibleFrames: errors.length, selectedFrames: errors.length, processedFrames: errors.length, failedFrames: 0 },
    },
    errorCode: null,
    errorMessage: null,
    completed: errors.length,
    total: errors.length,
  });
  return {
    project: { id: "p", name: "P", schemaVersion: 2, createdAt: "", updatedAt: "", activeRecipeId: null, untitled: true, storagePath: null, readOnly: false, dirty: false },
    sourcesById: { src: source },
    samplesById: {},
    recipesById: { r1: recipe("r1", "2026-01-01T00:00:00Z"), r2: recipe("r2", "2026-01-02T00:00:00Z") },
    runGroupsById: {},
    runsById: {
      a: run("a", "r1", [[3, 0.4], [1, 0.3]]),
      b: run("b", "r2", [[3, 0.2], [2, 0.5]]),
    },
    verificationReviewsByRunId: {},
    verificationFusionsById: {},
    uiStateByRoute: {},
  };
}

describe("verification fusion", () => {
  it("unions sparse frames and keeps all candidates", () => {
    const result = buildVerificationFusion({ state: state(), sourceId: "src", runIds: ["a", "b"], id: "f" });
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.fusion.frames.map((frame) => frame.frameIndex)).toEqual([1, 2, 3]);
    expect(result.fusion.frames.find((frame) => frame.frameIndex === 3)?.fusedError).toBe(0.2);
    expect(result.fusion.frames.find((frame) => frame.frameIndex === 3)?.candidateCount).toBe(2);
    expect(result.fusion.statistics.singleCandidateFrames).toBe(2);
  });

  it("rejects incomplete or malformed runs", () => {
    const project = state();
    project.runsById.a.result = { frames: [{ frameIndex: 1, error: null }], coverage: { selection: "all", eligibleFrames: 1, selectedFrames: 1, processedFrames: 1, failedFrames: 0 } };
    expect(fusionEligibility(project.runsById.a, project, project.sourcesById.src)).toEqual({ ok: false, reason: "invalid_frames" });
  });

  it("uses recipe creation time as a stable tie break", () => {
    const project = state();
    (project.runsById.a.result as { frames: Array<{ frameIndex: number; error: number }> }).frames.push({ frameIndex: 4, error: 0.25 });
    (project.runsById.b.result as { frames: Array<{ frameIndex: number; error: number }> }).frames.push({ frameIndex: 4, error: 0.25 });
    (project.runsById.a.result as { coverage: Record<string, unknown> }).coverage = { selection: "all", processedFrames: 3, selectedFrames: 3, eligibleFrames: 3, failedFrames: 0 };
    (project.runsById.b.result as { coverage: Record<string, unknown> }).coverage = { selection: "all", processedFrames: 3, selectedFrames: 3, eligibleFrames: 3, failedFrames: 0 };
    const result = buildVerificationFusion({ state: project, sourceId: "src", runIds: ["b", "a"], id: "tie" });
    expect(result.ok).toBe(true);
    if (result.ok) expect(result.fusion.frames.find((frame) => frame.frameIndex === 4)?.winnerRecipeId).toBe("r1");
  });

  it("exports a deterministic self-contained JSON and candidate CSV", () => {
    const result = buildVerificationFusion({ state: state(), sourceId: "src", runIds: ["a", "b"], id: "exported" });
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    const json = JSON.parse(buildVerificationFusionJson(result.fusion)) as Record<string, unknown>;
    expect(json.export_schema_version).toBe(1);
    expect(json.kind).toBe("getnative_verification_fusion");
    expect((json.fusion as Record<string, unknown>).id).toBe("exported");
    expect(buildVerificationFusionCsv(result.fusion).split("\n")[0]).toContain("candidate_recipe_name");
  });
});

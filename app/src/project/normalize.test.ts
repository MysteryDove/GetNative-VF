import { backendDisplayState, buildBackendRows } from "../engine/types";
import { describe, it } from "vitest";
import { assertLocaleParity, createTranslator, missingLocaleKeys } from "../i18n";
import {
  emptyProjectState,
  openedToProjectState,
  projectStateToManifest,
  readiness,
  restoredProjectRoute,
} from "./normalize";
import { autosaveStoragePath } from "./storage";
import type { OpenedProjectDto } from "./types";

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) {
    throw new Error(message);
  }
}

function sampleOpened(overrides?: Partial<OpenedProjectDto>): OpenedProjectDto {
  return {
    manifest: {
      schema_version: 1,
      id: "prj_1",
      name: "Demo",
      created_at: "2026-01-01T00:00:00Z",
      updated_at: "2026-01-01T00:00:00Z",
      active_recipe_id: null,
      untitled: false,
      sources: [
        {
          id: "src_1",
          kind: "still",
          path: "/missing.png",
          state: "ready",
          size_bytes: 1024,
          width: 1920,
          height: 1080,
          decoder: "image-rs/Png",
        },
        {
          id: "src_2",
          kind: "video",
          path: "/clip.mkv",
          fingerprint: "quick-sha256-v1:abc",
          state: "ready",
          width: 1920,
          height: 1080,
          duration_seconds: 4.5,
          decoder: "FFmpeg (bundled)",
          selected_stream_index: 0,
          video_streams: [
            {
              index: 0,
              codec_name: "h264",
              width: 1920,
              height: 1080,
              frame_count: 108,
              time_base_num: 1,
              time_base_den: 24000,
              frame_rate_num: 24000,
              frame_rate_den: 1001,
            },
          ],
        },
      ],
      samples: [
        {
          id: "smp_1",
          source_id: "src_2",
          source_fingerprint: "quick-sha256-v1:abc",
          included: true,
          order: 0,
          stream_index: 0,
          frame_index: 12,
          pts: 12012,
          best_effort_timestamp: 12013,
          time_base_num: 1,
          time_base_den: 24000,
          timestamp_seconds: 0.5005,
          tags: ["高细节"],
        },
      ],
      recipes: [],
      run_groups: [],
      runs: [],
      verification_reviews: [],
      ui_state_by_route: { overview: { selected: true } },
    },
    storage_path: "/tmp/demo.getnative.json",
    missing_source_ids: ["src_1"],
    read_only: false,
    schema_status: "supported",
    ...overrides,
  };
}

export function runPureModuleChecks(): void {
  assertLocaleParity();
  assert(missingLocaleKeys("en").length === 0, "en locale keys must match zh-CN");

  const tZh = createTranslator("zh-CN");
  const tEn = createTranslator("en");
  assert(tZh("hub.newProject") === "新建项目", "zh-CN hub.newProject");
  assert(tEn("hub.newProject") === "New Project", "en hub.newProject");
  assert(tZh("hub.revealProject") === "在文件夹中显示", "zh-CN reveal Project term");
  assert(tEn("hub.revealProject") === "Show in Folder", "en reveal Project term");
  assert(tEn("engine.version", { version: "1.0" }) === "Engine 1.0", "placeholder expansion");
  assert(tZh("overview.step.height") === "分辨率测试", "zh-CN Resolution Test term");
  assert(tZh("overview.step.kernel") === "算法测试", "zh-CN Algorithm Test term");
  assert(tZh("overview.step.verify") === "全视频检查", "zh-CN Full Video Check term");
  assert(tZh("recipe.active") === "当前", "zh-CN active Recipe state");
  assert(tZh("diagnostics.axis.twoAxis") === "两个方向", "zh-CN both-directions term");
  assert(tZh("diagnostics.kernelName.bicubic") === "Bicubic", "canonical Bicubic spelling");
  assert(tEn("overview.step.verify") === "Full Video Check", "en Full Video Check term");
  assert(tEn("diagnostics.axis.twoAxis") === "Both directions", "en both-directions term");

  const opened = sampleOpened();
  const state = openedToProjectState(opened);
  assert(state.project.id === "prj_1", "project id");
  assert(state.sourcesById.src_1.state === "missing", "missing source marked");
  assert(state.sourcesById.src_1.width === 1920, "still dimensions preserved");
  assert(state.sourcesById.src_2.videoStreams[0]?.frameCount === 108, "video stream preserved");
  assert(state.samplesById.smp_1.pts === 12012, "sample PTS preserved");
  assert(
    state.samplesById.smp_1.sourceFingerprint === "quick-sha256-v1:abc",
    "Sample Source fingerprint preserved",
  );
  assert(
    state.samplesById.smp_1.bestEffortTimestamp === 12013,
    "sample best-effort timestamp preserved",
  );
  assert(state.samplesById.smp_1.timeBaseDen === 24000, "sample timebase preserved");
  assert(state.uiStateByRoute.overview !== undefined, "ui state preserved");

  const manifest = projectStateToManifest(state);
  assert(manifest.schema_version === 1, "schema version round-trip");
  assert(manifest.sources[0]?.state === "missing", "source state round-trip");
  assert(manifest.sources[1]?.video_streams?.[0]?.codec_name === "h264", "stream round-trip");
  assert(manifest.samples[0]?.timestamp_seconds === 0.5005, "sample timestamp round-trip");
  assert(manifest.samples[0]?.pts === 12012, "sample PTS round-trip");
  assert(
    manifest.samples[0]?.source_fingerprint === "quick-sha256-v1:abc",
    "Sample Source fingerprint round-trip",
  );
  assert(
    manifest.samples[0]?.best_effort_timestamp === 12013,
    "sample best-effort timestamp round-trip",
  );

  const flags = readiness(state, false);
  assert(flags.hasMedia, "has media");
  assert(flags.hasMissingMedia, "missing media flag");
  assert(!flags.hasUnavailableIncludedSamples, "unreferenced missing source does not block Samples");
  assert(flags.hasReadyVideo, "ready video detected");
  assert(!flags.heightReady, "height blocked without analyze");

  const runnable = readiness(state, true);
  assert(runnable.heightReady, "included Sample with a ready source enables Resolution Test");
  assert(runnable.kernelReady, "included Sample with a ready source enables Algorithm Test");
  assert(!runnable.verifyReady, "Full Video Check requires an active locked Recipe");

  state.recipesById.recipe_1 = {
    id: "recipe_1",
    name: "Bicubic",
    status: "locked",
    locked: true,
    revision: 1,
    parentRecipeId: null,
    createdAt: "2026-01-01T00:00:00Z",
    updatedAt: "2026-01-01T00:00:00Z",
    geometry: null,
    kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
    metric: null,
    profileId: "muf-d278cd3",
    mathMode: "raw",
  };
  state.project.activeRecipeId = "recipe_1";
  assert(readiness(state, true).verifyReady, "ready video and active locked Recipe enable checking");

  const recipeManifest = projectStateToManifest(state);
  const storedRecipe = recipeManifest.recipes[0];
  assert(storedRecipe?.status === "locked", "recipe status round-trip");
  assert(storedRecipe?.locked === true, "locked flag written for locked recipe");
  assert(storedRecipe?.profile_id === "muf-d278cd3", "recipe profile round-trip");
  assert(
    (storedRecipe?.kernel as { id?: string } | null)?.id === "bicubic",
    "recipe kernel payload round-trip",
  );
  const reloaded = openedToProjectState({
    ...sampleOpened(),
    manifest: recipeManifest,
  });
  assert(reloaded.recipesById.recipe_1?.status === "locked", "recipe status reload");
  assert(reloaded.recipesById.recipe_1?.locked === true, "recipe locked derived on reload");

  const legacy = openedToProjectState({
    ...sampleOpened(),
    manifest: {
      ...sampleOpened().manifest,
      recipes: [{ id: "legacy_1", name: "Legacy", locked: true, revision: 2 }],
    },
  });
  assert(
    legacy.recipesById.legacy_1?.status === "locked",
    "schema-1 locked flag migrates to locked status",
  );
  assert(
    legacy.recipesById.legacy_1?.revision === 2,
    "schema-1 revision preserved on migration",
  );

  state.sourcesById.src_2.fingerprint = "quick-sha256-v1:changed";
  const staleSample = readiness(state, true);
  assert(staleSample.hasUnavailableIncludedSamples, "changed Sample Source fingerprint detected");
  assert(!staleSample.heightReady, "changed Sample Source fingerprint blocks Resolution Test");
  state.sourcesById.src_2.fingerprint = "quick-sha256-v1:abc";

  state.sourcesById.src_2.state = "missing";
  const unavailableSample = readiness(state, true);
  assert(unavailableSample.hasUnavailableIncludedSamples, "missing included Sample source detected");
  assert(!unavailableSample.heightReady, "missing included Sample source blocks Resolution Test");
  assert(!unavailableSample.verifyReady, "Full Video Check requires a ready video");

  state.samplesById.smp_1.included = false;
  const excludedUnavailableSample = readiness(state, true);
  assert(!excludedUnavailableSample.hasSamples, "excluded Samples do not satisfy readiness");
  assert(
    !excludedUnavailableSample.hasUnavailableIncludedSamples,
    "excluded unavailable Samples do not block included-Sample readiness",
  );

  const empty = emptyProjectState({ name: "Empty" });
  assert(Object.keys(empty.sourcesById).length === 0, "empty sources");
  assert(Object.keys(empty.jobsById).length === 0, "empty jobs");

  const untitled = emptyProjectState({
    untitled: true,
    storagePath: "/tmp/recovery/untitled.getnative.json",
  });
  assert(
    autosaveStoragePath(untitled) === null,
    "untitled autosave must keep recovery storage separate from Save As",
  );
  assert(
    autosaveStoragePath(state) === "/tmp/demo.getnative.json",
    "named Project autosave must retain its storage path",
  );

  state.uiStateByRoute.shell = { lastRoute: "diagnostics" };
  assert(
    restoredProjectRoute(state, "overview") === "diagnostics",
    "valid persisted route must be restored",
  );
  state.uiStateByRoute.shell = { lastRoute: "unknown" };
  assert(
    restoredProjectRoute(state, "overview") === "overview",
    "unknown persisted route must use the caller fallback",
  );

  const rows = buildBackendRows([
    {
      id: "cpu",
      compiled: true,
      device_available: true,
      analysis_command_available: false,
      axes: ["horizontal"],
      p_norms: null,
      max_half_bandwidth: null,
      max_forward_width: null,
    },
    {
      id: "metal",
      compiled: true,
      device_available: true,
      analysis_command_available: false,
      axes: ["vertical"],
      p_norms: null,
      max_half_bandwidth: null,
      max_forward_width: null,
    },
    {
      id: "cuda",
      compiled: false,
      device_available: false,
      analysis_command_available: false,
      axes: [],
      p_norms: null,
      max_half_bandwidth: null,
      max_forward_width: null,
    },
  ]);
  assert(rows.length === 4, "cpu/metal/cuda/vulkan rows");
  const vulkan = rows.find((row) => row.id === "vulkan");
  assert(vulkan && !vulkan.reported, "vulkan unreported");
  assert(vulkan?.compiled === null, "vulkan must not claim compiled");
  assert(vulkan?.analysisCommandAvailable === null, "vulkan must not claim command");
  assert(
    backendDisplayState(rows[0]) === "partial",
    "compiled CPU with a device but no analysis command must be partial",
  );
}

describe("Project normalization and capability truth", () => {
  it("preserves the schema, locale, readiness, and reserved backend contracts", () => {
    runPureModuleChecks();
  });
});

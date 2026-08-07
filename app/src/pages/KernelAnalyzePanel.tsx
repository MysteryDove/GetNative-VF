import { useEffect, useMemo, useState } from "react";
import { Play, SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import { resolveGeometrySnapshot } from "../engine/geometryResolve";
import type {
  BackendPreference,
  GeometrySnapshot,
  KernelRef,
  MathMode,
  MetricSpec,
} from "../engine/protocol";
import {
  defaultKernelDraft,
  estimateKernelWork,
  geometryGroupKey,
  resolveKernelCandidates,
  KERNEL_FAMILY_ORDER,
  type KernelDraft,
  type ResolvedGeometryMap,
} from "../engine/kernelDraft";
import { extractKernelResultRows, planKernelRunGroup } from "../engine/kernelRunGroup";
import { metricCompatibilityKey } from "../engine/runGroupPlan";
import { applyPayloadToRecipeDraft } from "../project/recipeApply";
import type { ProjectState, Run } from "../project/types";
import { BlockedState } from "../components/BlockedState";

type SampleDims = { width: number; height: number };

export function KernelAnalyzePanel({
  t,
  state,
  capabilities,
  analyzeAvailable,
  inheritedMetric,
  inheritedProfileId,
  inheritedMathMode,
  inheritedBackend,
  onOpenDiagnostics,
  onProjectChange,
}: {
  t: Translator;
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  analyzeAvailable: boolean;
  inheritedMetric: MetricSpec;
  inheritedProfileId: string;
  inheritedMathMode: MathMode;
  inheritedBackend: BackendPreference;
  onOpenDiagnostics: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
}) {
  const [draft, setDraft] = useState<KernelDraft>(() =>
    defaultKernelDraft(
      capabilities,
      inheritedMetric,
      inheritedProfileId,
      inheritedMathMode,
      inheritedBackend,
    ),
  );
  const [inheritMetric, setInheritMetric] = useState(true);
  const [geometries, setGeometries] = useState<ResolvedGeometryMap>({});
  const [geometryBusy, setGeometryBusy] = useState(false);
  const [geometryError, setGeometryError] = useState("");
  const [selectedCandidate, setSelectedCandidate] = useState<number | null>(null);
  const [applyNotice, setApplyNotice] = useState("");

  // MetricSpec inherits from Height by default; an explicit unlink is visible.
  useEffect(() => {
    if (inheritMetric) {
      setDraft((current) => ({ ...current, metric: { ...inheritedMetric } }));
    }
  }, [inheritMetric, inheritedMetric]);

  function patch(partial: Partial<KernelDraft>) {
    setDraft((current) => ({ ...current, ...partial }));
  }

  const includedSamples = useMemo(
    () =>
      Object.values(state.samplesById)
        .filter((sample) => sample.included)
        .sort((a, b) => a.order - b.order),
    [state.samplesById],
  );

  const sampleDims = useMemo(() => {
    const map: Record<string, SampleDims | null> = {};
    for (const sample of includedSamples) {
      const source = state.sourcesById[sample.sourceId];
      map[sample.id] =
        source?.width && source?.height
          ? { width: source.width, height: source.height }
          : null;
    }
    return map;
  }, [includedSamples, state.sourcesById]);

  /** Distinct source shapes whose fixed geometry must be resolved. */
  const geometryGroups = useMemo(() => {
    const groups = new Map<string, { key: string; dims: SampleDims; sampleCount: number }>();
    for (const sample of includedSamples) {
      const dims = sampleDims[sample.id];
      if (!dims) continue;
      const key = geometryGroupKey({
        sourceWidth: dims.width,
        sourceHeight: dims.height,
        baseHeight: draft.baseHeight,
        baseWidth: draft.baseWidth,
        profileId: draft.profileId,
      });
      const existing = groups.get(key);
      if (existing) existing.sampleCount += 1;
      else groups.set(key, { key, dims, sampleCount: 1 });
    }
    return [...groups.values()];
  }, [includedSamples, sampleDims, draft.baseHeight, draft.baseWidth, draft.profileId]);

  const unresolvedGroups = geometryGroups.filter((group) => !geometries[group.key]);

  const candidates = useMemo(
    () => resolveKernelCandidates(draft, capabilities),
    [draft, capabilities],
  );

  const plan = useMemo(() => {
    const result = planKernelRunGroup({
      draft,
      samples: includedSamples,
      sourcesById: state.sourcesById,
      geometries,
      capabilities,
    });
    return result.ok ? result.plan : null;
  }, [draft, includedSamples, state.sourcesById, geometries, capabilities]);

  const kernelRuns = useMemo(
    () =>
      Object.values(state.runsById)
        .filter((run) => run.runType === "kernel")
        .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [state.runsById],
  );
  const activeMetricKey = metricCompatibilityKey(draft.metric);
  const resultRows = useMemo(
    () => buildKernelResultRows(kernelRuns, state, activeMetricKey),
    [kernelRuns, state, activeMetricKey],
  );

  async function resolveGeometries() {
    setGeometryBusy(true);
    setGeometryError("");
    try {
      const entries: Array<[string, GeometrySnapshot]> = [];
      for (const group of unresolvedGroups) {
        const baseHeight = Number(draft.baseHeight);
        if (!Number.isFinite(baseHeight) || baseHeight <= 0) {
          throw new Error(t("analyze.k.baseHeightInvalid"));
        }
        const baseWidthText = draft.baseWidth.trim();
        const baseWidth = baseWidthText ? Number(baseWidthText) : null;
        if (baseWidthText && (!Number.isFinite(baseWidth) || (baseWidth ?? 0) <= 0)) {
          throw new Error(t("analyze.k.baseWidthInvalid"));
        }
        const result = await resolveGeometrySnapshot({
          profileId: draft.profileId,
          sourceWidth: group.dims.width,
          sourceHeight: group.dims.height,
          baseHeight,
          baseWidth,
        });
        entries.push([group.key, result]);
      }
      setGeometries((current) => ({ ...current, ...Object.fromEntries(entries) }));
    } catch (error) {
      setGeometryError(String(error));
    } finally {
      setGeometryBusy(false);
    }
  }

  const work = candidates.ok
    ? estimateKernelWork({
        sampleCount: includedSamples.length,
        candidateCount: candidates.candidates.length,
      })
    : 0;

  const selectedKernel =
    candidates.ok && selectedCandidate !== null
      ? (candidates.candidates[selectedCandidate] ?? null)
      : null;

  /** Apply the selected kernel candidate (+ resolved geometry) to the Recipe Draft. */
  function applyKernelToRecipeDraft(includeDivergedMetric: boolean) {
    if (!selectedKernel) return;
    const geometry = geometryGroups.length
      ? (geometries[geometryGroups[0].key] ?? null)
      : null;
    const result = applyPayloadToRecipeDraft(
      state,
      {
        kernel: { id: selectedKernel.id, parameters: { ...selectedKernel.parameters } },
        ...(geometry ? { geometry } : {}),
        profileId: draft.profileId,
        mathMode: draft.mathMode,
        ...(includeDivergedMetric ? { metric: { ...draft.metric } } : {}),
      },
      t("recipe.defaultName"),
    );
    if (!result.ok) {
      setApplyNotice(t("recipe.applyFailed"));
      return;
    }
    const next = result.state;
    onProjectChange(() => next);
    setApplyNotice(t("recipe.applied", { name: result.recipe.name }));
  }

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : includedSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : !candidates.ok
        ? t("analyze.k.candidatesInvalid")
        : candidates.candidates.length < 2
          ? t("analyze.k.tooFewCandidates")
          : unresolvedGroups.length > 0
            ? t("analyze.k.geometryUnresolved")
            : !plan
              ? t("analyze.k.planInvalid")
              : t("analyze.runBlocked.noWorker");

  return (
    <div className="analyze-layout">
      <aside className="analyze-samples pane">
        <h3>{t("analyze.samplesTitle")}</h3>
        {includedSamples.length === 0 ? (
          <p className="empty-copy">{t("analyze.noSamples")}</p>
        ) : (
          <ul className="analyze-sample-list">
            {includedSamples.map((sample) => {
              const source = state.sourcesById[sample.sourceId];
              const dims = sampleDims[sample.id];
              return (
                <li key={sample.id}>
                  <div>
                    <strong>{sample.label || sample.id}</strong>
                    <span>
                      {source?.label || source?.path || sample.sourceId}
                      {sample.frameIndex != null ? ` · #${sample.frameIndex}` : ""}
                      {dims ? ` · ${dims.width}×${dims.height}` : ""}
                    </span>
                  </div>
                </li>
              );
            })}
          </ul>
        )}

        {plan ? (
          <div className="run-group-plan">
            <h3>{t("analyze.runGroupPlan")}</h3>
            <p className="help-copy">
              {t("analyze.runGroupType", { type: plan.groupType })}
              {" · "}
              {t("analyze.memberCount", { count: String(plan.memberCount) })}
              {" · "}
              {t("analyze.workEstimate", { count: String(plan.workEstimate) })}
            </p>
            {plan.memberCount > 1 ? (
              <p className="help-copy">{t("analyze.k.memberPerSample")}</p>
            ) : null}
          </div>
        ) : null}
      </aside>

      <section className="analyze-plot pane">
        <div className="analyze-table-host">
          <h3>{t("analyze.k.geometryTitle")}</h3>
          <p className="help-copy">{t("analyze.k.geometryReadOnly")}</p>
          {geometryGroups.length === 0 ? (
            <p className="empty-copy">{t("analyze.noSamples")}</p>
          ) : (
            <div className="dense-table">
              {geometryGroups.map((group) => {
                const geometry = geometries[group.key];
                return (
                  <div className="dense-row" key={group.key}>
                    <strong>
                      {group.dims.width}×{group.dims.height} → {draft.baseHeight}
                    </strong>
                    <span>
                      {geometry
                        ? `${t("analyze.k.canvas")} ${geometry.canvasWidth}×${geometry.canvasHeight}` +
                          ` · src (${geometry.srcLeft}, ${geometry.srcTop}) ` +
                          `${geometry.srcWidth}×${geometry.srcHeight}`
                        : t("analyze.k.geometryMissing")}
                    </span>
                  </div>
                );
              })}
            </div>
          )}
          {unresolvedGroups.length > 0 ? (
            <button
              className="secondary-button"
              type="button"
              disabled={geometryBusy || includedSamples.length === 0}
              onClick={resolveGeometries}
            >
              {geometryBusy ? t("diagnostics.working") : t("analyze.k.resolveGeometry")}
            </button>
          ) : null}
          {geometryError ? <p className="help-copy warning-copy">{geometryError}</p> : null}
        </div>

        <div className="analyze-table-host">
          <h3>{t("analyze.k.candidatesTitle")}</h3>
          {candidates.ok ? (
            <div className="candidate-preview" role="table" aria-label={t("analyze.k.candidatesTitle")}>
              <div className="candidate-preview-meta">
                {t("analyze.k.candidateCount", { count: String(candidates.candidates.length) })}
                {work > 0 ? ` · ${t("analyze.workEstimate", { count: String(work) })}` : ""}
              </div>
              <div className="candidate-chips">
                {candidates.candidates.slice(0, 48).map((kernel, index) => (
                  <button
                    type="button"
                    className={`candidate-chip ${selectedCandidate === index ? "selected" : ""}`}
                    key={`${kernel.id}-${index}`}
                    onClick={() =>
                      setSelectedCandidate((current) => (current === index ? null : index))
                    }
                  >
                    {kernelChipLabel(t, kernel)}
                  </button>
                ))}
                {candidates.candidates.length > 48 ? (
                  <span className="candidate-chip muted">
                    +{candidates.candidates.length - 48}
                  </span>
                ) : null}
              </div>
              <p className="help-copy">{t("analyze.k.sequenceExact")}</p>
              <div className="analyze-table-toolbar">
                <button
                  className="secondary-button"
                  type="button"
                  disabled={!selectedKernel}
                  onClick={() => applyKernelToRecipeDraft(false)}
                >
                  {t("analyze.k.applyToRecipe")}
                </button>
                {!inheritMetric ? (
                  <button
                    className="secondary-button"
                    type="button"
                    disabled={!selectedKernel}
                    onClick={() => applyKernelToRecipeDraft(true)}
                  >
                    {t("analyze.k.applyWithDivergedMetric")}
                  </button>
                ) : null}
                {applyNotice ? <span className="help-copy">{applyNotice}</span> : null}
              </div>
            </div>
          ) : (
            <p className="help-copy">{t("analyze.k.candidatesInvalid")}</p>
          )}
        </div>

        <div className="analyze-table-host">
          <h3>{t("analyze.resultsTable")}</h3>
          {resultRows.rows.length ? (
            <div className="result-table" role="table" aria-label={t("analyze.resultsTable")}>
              <div className="result-table-head" role="row">
                <span role="columnheader">{t("analyze.col.kernel")}</span>
                <span role="columnheader">{t("analyze.col.metric")}</span>
                <span role="columnheader">{t("analyze.col.sample")}</span>
                <span role="columnheader">{t("analyze.col.run")}</span>
              </div>
              {resultRows.rows.slice(0, 200).map((row) => (
                <div className="result-table-row" role="row" key={`${row.runId}-${row.kernelLabel}`}>
                  <span role="cell">{row.kernelLabel}</span>
                  <span role="cell">{row.displayValue}</span>
                  <span role="cell">{row.sampleLabel}</span>
                  <span role="cell">{row.runId.slice(0, 10)}</span>
                </div>
              ))}
            </div>
          ) : (
            <BlockedState
              title={
                analyzeAvailable ? t("analyze.waitingWorkerTitle") : t("analyze.blockedTitle")
              }
              body={
                analyzeAvailable
                  ? t("analyze.waitingWorkerBody")
                  : t("analyze.blockedBody")
              }
              action={
                analyzeAvailable ? undefined : (
                  <button className="secondary-button" type="button" onClick={onOpenDiagnostics}>
                    {t("nav.diagnostics")}
                  </button>
                )
              }
            />
          )}
          {resultRows.incompatibleCount > 0 ? (
            <p className="help-copy warning-copy">
              {t("analyze.incompatibleMetricHidden", {
                count: String(resultRows.incompatibleCount),
              })}
            </p>
          ) : null}
        </div>
      </section>

      <aside className="analyze-params pane">
        <h3>
          <SlidersHorizontal size={15} />
          {t("analyze.paramsTitle")}
        </h3>

        <label className="block">
          <span>{t("analyze.k.scanMode")}</span>
          <select
            value={draft.scanMode}
            onChange={(event) =>
              patch({ scanMode: event.target.value as KernelDraft["scanMode"] })
            }
          >
            <option value="preset_families">{t("analyze.k.modePreset")}</option>
            <option value="bicubic_grid">{t("analyze.k.modeBicubicGrid")}</option>
          </select>
        </label>

        {draft.scanMode === "preset_families" ? (
          <fieldset className="metric-fieldset">
            <legend>{t("analyze.k.families")}</legend>
            {KERNEL_FAMILY_ORDER.map((family) => (
              <label className="checkbox-row" key={family}>
                <input
                  type="checkbox"
                  checked={draft.families[family]}
                  onChange={(event) =>
                    patch({
                      families: { ...draft.families, [family]: event.target.checked },
                    })
                  }
                />
                <span>{kernelDisplayName(t, family)}</span>
              </label>
            ))}
            {draft.families.bicubic ? (
              <div className="metric-grid">
                <label className="block">
                  <span>b</span>
                  <input
                    value={draft.bicubicB}
                    onChange={(event) => patch({ bicubicB: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>c</span>
                  <input
                    value={draft.bicubicC}
                    onChange={(event) => patch({ bicubicC: event.target.value })}
                  />
                </label>
              </div>
            ) : null}
            {draft.families.lanczos ? (
              <label className="block">
                <span>{t("analyze.k.lanczosTaps")}</span>
                <input
                  value={draft.lanczosTaps}
                  onChange={(event) => patch({ lanczosTaps: event.target.value })}
                />
              </label>
            ) : null}
          </fieldset>
        ) : (
          <fieldset className="metric-fieldset">
            <legend>{t("analyze.k.bicubicGrid")}</legend>
            {(
              [
                ["bStart", "b", t("analyze.start")],
                ["bStop", "b", t("analyze.stop")],
                ["bStep", "b", t("analyze.step")],
                ["cStart", "c", t("analyze.start")],
                ["cStop", "c", t("analyze.stop")],
                ["cStep", "c", t("analyze.step")],
              ] as const
            ).map(([key, axis, label]) => (
              <label className="block" key={key}>
                <span>{`${axis} ${label}`}</span>
                <input
                  value={draft[key]}
                  onChange={(event) => patch({ [key]: event.target.value })}
                />
              </label>
            ))}
            <p className="help-copy">{t("analyze.k.gridEndpoints")}</p>
          </fieldset>
        )}

        <fieldset className="metric-fieldset">
          <legend>{t("analyze.k.geometryParams")}</legend>
          <label className="block">
            <span>{t("diagnostics.baseH")}</span>
            <input
              value={draft.baseHeight}
              onChange={(event) => patch({ baseHeight: event.target.value })}
            />
          </label>
          <label className="block">
            <span>{t("diagnostics.baseW")}</span>
            <input
              value={draft.baseWidth}
              placeholder={t("analyze.optional")}
              onChange={(event) => patch({ baseWidth: event.target.value })}
            />
          </label>
          <label className="block">
            <span>{t("diagnostics.profile")}</span>
            <select
              value={draft.profileId}
              onChange={(event) => patch({ profileId: event.target.value })}
            >
              {(capabilities?.payload.profiles ?? []).map((profile) => (
                <option key={profile.id} value={profile.id}>
                  {profileDisplayName(t, profile.id)}
                </option>
              ))}
              {(capabilities?.payload.profiles ?? []).length === 0 ? (
                <option value={draft.profileId}>{draft.profileId}</option>
              ) : null}
            </select>
          </label>
          <label className="block">
            <span>{t("analyze.backend")}</span>
            <select
              value={draft.backendPreference}
              onChange={(event) =>
                patch({ backendPreference: event.target.value as BackendPreference })
              }
            >
              <option value="auto">{t("analyze.backend.auto")}</option>
              <option value="cpu">{t("backend.cpu")}</option>
              <option value="metal">{t("backend.metal")}</option>
            </select>
          </label>
        </fieldset>

        <fieldset className="metric-fieldset">
          <legend>{t("analyze.metricSpec")}</legend>
          <label className="checkbox-row">
            <input
              type="checkbox"
              checked={inheritMetric}
              onChange={(event) => setInheritMetric(event.target.checked)}
            />
            <span>{t("analyze.k.inheritMetric")}</span>
          </label>
          {inheritMetric ? (
            <p className="help-copy">{t("analyze.k.inheritMetricNote")}</p>
          ) : (
            <>
              <p className="help-copy warning-copy">{t("analyze.k.metricDiverged")}</p>
              <MetricEditor
                t={t}
                metric={draft.metric}
                onChange={(metric) => patch({ metric })}
              />
            </>
          )}
        </fieldset>

        <div className="analyze-run-block">
          <button className="primary-button" type="button" disabled>
            <Play size={15} />
            {t("analyze.k.runKernel")}
          </button>
          <p className="help-copy">{runBlockedReason}</p>
        </div>
      </aside>
    </div>
  );
}

function MetricEditor({
  t,
  metric,
  onChange,
}: {
  t: Translator;
  metric: MetricSpec;
  onChange: (metric: MetricSpec) => void;
}) {
  return (
    <>
      <div className="metric-grid">
        {(
          [
            ["cropLeft", t("analyze.cropLeft")],
            ["cropRight", t("analyze.cropRight")],
            ["cropTop", t("analyze.cropTop")],
            ["cropBottom", t("analyze.cropBottom")],
          ] as const
        ).map(([key, label]) => (
          <label key={key} className="block">
            <span>{label}</span>
            <input
              type="number"
              min={0}
              step={1}
              value={metric[key]}
              onChange={(event) =>
                onChange({ ...metric, [key]: Number(event.target.value) })
              }
            />
          </label>
        ))}
      </div>
      <label className="block">
        <span>{t("analyze.pixelExclusion")}</span>
        <input
          type="number"
          min={0}
          step="any"
          value={metric.pixelExclusionThreshold}
          onChange={(event) =>
            onChange({ ...metric, pixelExclusionThreshold: Number(event.target.value) })
          }
        />
      </label>
      <label className="block">
        <span>{t("analyze.pNorm")}</span>
        <input
          type="number"
          min={1}
          step={1}
          value={metric.pNorm}
          onChange={(event) => onChange({ ...metric, pNorm: Number(event.target.value) })}
        />
      </label>
    </>
  );
}

function kernelChipLabel(t: Translator, kernel: KernelRef): string {
  const name = kernelDisplayName(t, kernel.id);
  const params = Object.entries(kernel.parameters);
  if (!params.length) return name;
  return `${name} (${params.map(([key, value]) => `${key}=${value}`).join(", ")})`;
}

type KernelResultRow = {
  runId: string;
  kernelLabel: string;
  metric: number;
  displayValue: string;
  sampleLabel: string;
};

function buildKernelResultRows(
  runs: Run[],
  state: ProjectState,
  activeMetricKey: string,
): { rows: KernelResultRow[]; incompatibleCount: number } {
  const rows: KernelResultRow[] = [];
  let incompatibleCount = 0;
  for (const run of runs) {
    const snapshot = run.inputSnapshot as {
      metric?: MetricSpec;
    } | null;
    if (snapshot?.metric && metricCompatibilityKey(snapshot.metric) !== activeMetricKey) {
      incompatibleCount += 1;
      continue;
    }
    const extracted = extractKernelResultRows(run.result);
    if (!extracted) continue;
    const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
    for (const row of extracted) {
      const params = Object.entries(row.parameters);
      rows.push({
        runId: run.id,
        kernelLabel: params.length
          ? `${row.kernelId} (${params.map(([key, value]) => `${key}=${value}`).join(", ")})`
          : row.kernelId,
        metric: row.metric,
        displayValue: row.metric.toPrecision(6),
        sampleLabel: sample?.label ?? run.sampleId ?? "—",
      });
    }
  }
  return { rows, incompatibleCount };
}

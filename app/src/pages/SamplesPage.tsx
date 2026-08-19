import { Fragment, useCallback, useEffect, useMemo, useState } from "react";
import {
  ArrowDown,
  ArrowUp,
  CheckSquare,
  FileImage,
  Film,
  FolderPlus,
  LoaderCircle,
  Square,
  Tag,
  Trash2,
} from "lucide-react";
import type { Translator } from "../i18n";
import { getMediaPreview } from "../media/service";
import { importMediaPaths } from "../media/importSources";
import { useFileDrop } from "../media/useFileDrop";
import { dimensionText, formatSeconds } from "../media/format";
import { applySourceError } from "../project/samples";
import { toggleSetValue } from "../utils/collections";
import type { ProjectState, Sample } from "../project/types";

type ProjectUpdater = (updater: (state: ProjectState) => ProjectState) => void;

export function SamplesPage({
  t,
  state,
  onProjectChange,
  onStartHeightAnalysis,
  active = true,
}: {
  t: Translator;
  state: ProjectState;
  onProjectChange: ProjectUpdater;
  onStartHeightAnalysis?: (selectedIds?: string[]) => void;
  active?: boolean;
}) {
  const samples = useMemo(
    () => Object.values(state.samplesById).sort((a, b) => a.order - b.order),
    [state.samplesById],
  );
  const [selectedIds, setSelectedIds] = useState<Set<string>>(new Set());
  const [focusedId, setFocusedId] = useState<string | null>(samples[0]?.id ?? null);
  const [tag, setTag] = useState("");
  const [groupBySource, setGroupBySource] = useState(false);
  const [dropNotice, setDropNotice] = useState("");
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [previewBusy, setPreviewBusy] = useState(false);
  const [previewError, setPreviewError] = useState("");
  const focusedSample = focusedId ? state.samplesById[focusedId] : null;
  const focusedSource = focusedSample ? state.sourcesById[focusedSample.sourceId] : null;
  const focusedSourceChanged = sampleSourceChanged(focusedSample, focusedSource);
  const sampleGroups = useMemo(
    () => groupSamples(samples, state, groupBySource),
    [groupBySource, samples, state],
  );

  /**
   * Drop-to-sample: still images become included Samples directly; videos
   * import as Sources and keep frame selection on the Media page.
   */
  const handleDropPaths = useCallback(
    async (paths: string[]) => {
      if (state.project.readOnly) return;
      setDropNotice("");
      const outcome = await importMediaPaths({
        paths,
        state,
        onProjectChange,
        createSamplesForStills: true,
      });
      if (outcome.pending === 0) return;
      setDropNotice(
        t("samples.dropResult", {
          sampled: String(outcome.sampled),
          videos: String(outcome.videos),
          failed: String(outcome.failed),
        }),
      );
    },
    [onProjectChange, state, t],
  );
  const dropActive = useFileDrop((paths) => void handleDropPaths(paths), active);

  useEffect(() => {
    if (focusedId && state.samplesById[focusedId]) return;
    setFocusedId(samples[0]?.id ?? null);
  }, [focusedId, samples, state.samplesById]);

  useEffect(() => {
    let cancelled = false;
    setPreviewUrl((current) => {
      if (current) URL.revokeObjectURL(current);
      return null;
    });
    setPreviewError("");
    if (!active || !focusedSample || !focusedSource || focusedSource.state !== "ready") return;
    if (focusedSourceChanged) {
      setPreviewError(t("samples.sourceChanged"));
      return;
    }
    setPreviewBusy(true);
    getMediaPreview({
      path: focusedSource.path,
      fingerprint: focusedSample.sourceFingerprint ?? focusedSource.fingerprint,
      streamIndex: focusedSample.streamIndex,
      frameIndex: focusedSample.frameIndex,
      exact: focusedSample.frameIndex != null,
    })
      .then((url) => {
        if (cancelled) URL.revokeObjectURL(url);
        else setPreviewUrl(url);
      })
      .catch((reason) => {
        if (cancelled) return;
        const detail = String(reason);
        setPreviewError(detail);
        if (
          detail.includes("media_fingerprint_mismatch")
          || detail.includes("media_fingerprint_error")
        ) {
          onProjectChange((current) =>
            applySourceError(current, focusedSource.id, {
              code: "media_fingerprint_mismatch",
              detail,
            }),
          );
        }
      })
      .finally(() => {
        if (!cancelled) setPreviewBusy(false);
      });
    return () => {
      cancelled = true;
    };
  }, [active, focusedSample, focusedSource, focusedSourceChanged, onProjectChange, t]);

  useEffect(
    () => () => {
      if (previewUrl) URL.revokeObjectURL(previewUrl);
    },
    [previewUrl],
  );

  function toggleSelected(id: string) {
    setSelectedIds((current) => toggleSetValue(current, id));
  }

  function updateSelected(transform: (sample: Sample) => Sample) {
    onProjectChange((current) => ({
      ...current,
      samplesById: Object.fromEntries(
        Object.entries(current.samplesById).map(([id, sample]) => [
          id,
          selectedIds.has(id) ? transform(sample) : sample,
        ]),
      ),
    }));
  }

  function setIncluded(included: boolean) {
    updateSelected((sample) => ({ ...sample, included }));
  }

  function applyTag() {
    const value = tag.trim();
    if (!value) return;
    updateSelected((sample) => ({
      ...sample,
      tags: sample.tags.includes(value) ? sample.tags : [...sample.tags, value],
    }));
    setTag("");
  }

  function removeSelected() {
    onProjectChange((current) => ({
      ...current,
      samplesById: normalizeOrder(
        Object.fromEntries(
          Object.entries(current.samplesById).filter(([id]) => !selectedIds.has(id)),
        ),
      ),
    }));
    setSelectedIds(new Set());
  }

  function moveSelected(direction: -1 | 1) {
    if (selectedIds.size !== 1) return;
    const [selectedId] = selectedIds;
    const index = samples.findIndex((sample) => sample.id === selectedId);
    const swapIndex = index + direction;
    if (index < 0 || swapIndex < 0 || swapIndex >= samples.length) return;
    const next = [...samples];
    [next[index], next[swapIndex]] = [next[swapIndex], next[index]];
    onProjectChange((current) => ({
      ...current,
      samplesById: Object.fromEntries(
        next.map((sample, order) => [sample.id, { ...current.samplesById[sample.id], order }]),
      ),
    }));
  }

  const selectedCount = selectedIds.size;
  const includedReadyCount = samples.filter((sample) => {
    if (!sample.included) return false;
    const source = state.sourcesById[sample.sourceId];
    if (!source || source.state !== "ready") return false;
    if (
      sample.sourceFingerprint &&
      source.fingerprint &&
      sample.sourceFingerprint !== source.fingerprint
    ) {
      return false;
    }
    return true;
  }).length;
  const selectedReadyCount = samples.filter((sample) => {
    if (!selectedIds.has(sample.id) || !sample.included) return false;
    const source = state.sourcesById[sample.sourceId];
    return Boolean(
      source?.state === "ready" &&
        (!sample.sourceFingerprint ||
          !source.fingerprint ||
          sample.sourceFingerprint === source.fingerprint),
    );
  }).length;
  const analysisReady = selectedCount > 0 ? selectedReadyCount > 0 : includedReadyCount > 0;

  return (
    <div className={`page-panel samples-page ${dropActive ? "drop-active" : ""}`}>
      <div className="page-header">
        <div>
          <h2>{t("samples.title")}</h2>
          <span>{t("samples.subtitle", { count: samples.length })}</span>
        </div>
        {onStartHeightAnalysis ? (
          <button
            className="secondary-button primary-command"
            type="button"
            disabled={!analysisReady}
            onClick={() => onStartHeightAnalysis(selectedCount > 0 ? [...selectedIds] : undefined)}
          >
            {t("samples.startHeightAnalysis")}
          </button>
        ) : null}
      </div>

      <div className="sample-toolbar">
        <button className="secondary-button" type="button" disabled={!selectedCount || state.project.readOnly} onClick={() => setIncluded(true)}>
          <CheckSquare size={15} />
          {t("samples.include")}
        </button>
        <button className="secondary-button" type="button" disabled={!selectedCount || state.project.readOnly} onClick={() => setIncluded(false)}>
          <Square size={15} />
          {t("samples.exclude")}
        </button>
        <button className="icon-button" type="button" title={t("samples.moveUp")} aria-label={t("samples.moveUp")} disabled={selectedCount !== 1 || state.project.readOnly} onClick={() => moveSelected(-1)}>
          <ArrowUp size={14} />
        </button>
        <button className="icon-button" type="button" title={t("samples.moveDown")} aria-label={t("samples.moveDown")} disabled={selectedCount !== 1 || state.project.readOnly} onClick={() => moveSelected(1)}>
          <ArrowDown size={14} />
        </button>
        <label className="sample-tag-field">
          <Tag size={14} />
          <input value={tag} placeholder={t("samples.tagPlaceholder")} onChange={(event) => setTag(event.target.value)} onKeyDown={(event) => {
            if (event.key === "Enter") applyTag();
          }} />
        </label>
        <button className="secondary-button" type="button" disabled={!selectedCount || !tag.trim() || state.project.readOnly} onClick={applyTag}>
          {t("samples.addTag")}
        </button>
        <label className="sample-group-toggle">
          <input
            type="checkbox"
            checked={groupBySource}
            onChange={(event) => setGroupBySource(event.target.checked)}
          />
          <span>{t("samples.groupBySource")}</span>
        </label>
        <span className="sample-selection-count">{t("samples.selectedCount", { count: selectedCount })}</span>
        <button className="icon-button danger" type="button" title={t("samples.remove")} aria-label={t("samples.remove")} disabled={!selectedCount || state.project.readOnly} onClick={removeSelected}>
          <Trash2 size={14} />
        </button>
      </div>

      {dropNotice ? <p className="help-copy samples-drop-notice">{dropNotice}</p> : null}

      {samples.length ? (
        <div className="samples-workspace">
          <div className="sample-table" role="table" aria-label={t("samples.title")}>
            <div className="sample-table-head" role="row">
              <span role="columnheader">{t("samples.select")}</span>
              <span role="columnheader">{t("samples.sample")}</span>
              <span role="columnheader">{t("samples.source")}</span>
              <span role="columnheader">{t("samples.frame")}</span>
              <span role="columnheader">{t("samples.time")}</span>
              <span role="columnheader">{t("samples.state")}</span>
              <span role="columnheader">{t("samples.tags")}</span>
            </div>
            {sampleGroups.map((group) => (
              <Fragment key={group.sourceId}>
                {group.label ? (
                  <div className="sample-group-row" role="row">
                    <span role="cell">{group.label}</span>
                  </div>
                ) : null}
                {group.samples.map((sample) => {
                  const source = state.sourcesById[sample.sourceId];
                  const sourceChanged = sampleSourceChanged(sample, source);
                  return (
                <div
                  className={`sample-table-row ${focusedId === sample.id ? "focused" : ""}`}
                  role="row"
                  key={sample.id}
                  onClick={() => setFocusedId(sample.id)}
                >
                  <span role="cell">
                    <input
                      type="checkbox"
                      checked={selectedIds.has(sample.id)}
                      aria-label={t("samples.selectSample", { name: sample.label ?? sample.id })}
                      onClick={(event) => event.stopPropagation()}
                      onChange={() => toggleSelected(sample.id)}
                    />
                  </span>
                  <span className="sample-identity" role="cell">
                    {source?.kind === "video" ? <Film size={15} /> : <FileImage size={15} />}
                    <span>
                      <strong>{sample.label ?? sample.id}</strong>
                      <small>
                        {source?.kind === "still"
                          ? dimensionText(source.width, source.height)
                          : source
                            ? dimensionText(source.width, source.height)
                            : "-"}
                      </small>
                    </span>
                  </span>
                  <span className="sample-source" role="cell" title={source?.path}>
                    {source?.label ?? source?.path ?? t("samples.sourceMissing")}
                  </span>
                  <span role="cell" className="sample-frame">
                    {source?.kind === "video" && sample.frameIndex != null
                      ? sample.frameIndex
                      : "—"}
                  </span>
                  <span role="cell" className="sample-time">
                    {source?.kind === "video" && sample.frameIndex != null
                      ? formatSeconds(sample.timestampSeconds)
                      : "—"}
                  </span>
                  <span
                    role="cell"
                    className={sourceChanged ? "stale" : sample.included ? "included" : "excluded"}
                  >
                    {sourceChanged
                      ? t("samples.sourceChanged")
                      : sample.included
                        ? t("samples.included")
                        : t("samples.excluded")}
                  </span>
                  <span className="sample-tags" role="cell">
                    {sample.tags.length ? sample.tags.map((value) => <em key={value}>{value}</em>) : "-"}
                  </span>
                </div>
                  );
                })}
              </Fragment>
            ))}
          </div>

          <aside className="sample-preview-pane">
            <div className="pane-heading">
              <strong>{t("samples.preview")}</strong>
            </div>
            <div className="sample-preview">
              {previewUrl ? <img src={previewUrl} alt={focusedSample?.label ?? t("samples.preview")} draggable={false} /> : null}
              {!previewUrl ? (
                <div className="viewport-empty">
                  {previewBusy ? <LoaderCircle className="spin" size={24} /> : <FileImage size={24} />}
                  <span>{previewError || t("samples.selectPreview")}</span>
                </div>
              ) : null}
            </div>
            {focusedSample && focusedSource ? (
              <dl className="sample-details">
                <div><dt>{t("samples.source")}</dt><dd>{focusedSource.label ?? focusedSource.path}</dd></div>
                <div><dt>{t("media.dimensions")}</dt><dd>{dimensionText(focusedSource.width, focusedSource.height)}</dd></div>
                {focusedSample.frameIndex != null ? <div><dt>{t("media.frameNumber")}</dt><dd>{focusedSample.frameIndex}</dd></div> : null}
                {focusedSample.timestampSeconds != null ? <div><dt>{t("media.timestamp")}</dt><dd>{formatSeconds(focusedSample.timestampSeconds)}</dd></div> : null}
              </dl>
            ) : null}
          </aside>
        </div>
      ) : (
        <div className="workspace-empty sample-empty">
          <FileImage size={28} />
          <strong>{t("samples.emptyTitle")}</strong>
          <span>{t("samples.emptyBody")}</span>
          <span className="help-copy">{t("samples.dropHint")}</span>
        </div>
      )}

      {dropActive ? (
        <div className="drop-overlay">
          <FolderPlus size={30} />
          <strong>{t("samples.dropNow")}</strong>
        </div>
      ) : null}
    </div>
  );
}

function normalizeOrder(samples: Record<string, Sample>): Record<string, Sample> {
  return Object.fromEntries(
    Object.values(samples)
      .sort((a, b) => a.order - b.order)
      .map((sample, order) => [sample.id, { ...sample, order }]),
  );
}

function sampleSourceChanged(
  sample: Sample | null | undefined,
  source: ProjectState["sourcesById"][string] | null | undefined,
): boolean {
  return Boolean(
    sample?.sourceFingerprint &&
      source?.fingerprint &&
      sample.sourceFingerprint !== source.fingerprint,
  );
}

function groupSamples(
  samples: Sample[],
  state: ProjectState,
  enabled: boolean,
): Array<{ sourceId: string; label: string | null; samples: Sample[] }> {
  if (!enabled) return [{ sourceId: "all", label: null, samples }];
  const groups = new Map<string, Sample[]>();
  for (const sample of samples) {
    const group = groups.get(sample.sourceId) ?? [];
    group.push(sample);
    groups.set(sample.sourceId, group);
  }
  return Array.from(groups, ([sourceId, groupedSamples]) => {
    const source = state.sourcesById[sourceId];
    return {
      sourceId,
      label: source?.label ?? source?.path ?? sourceId,
      samples: groupedSamples,
    };
  });
}

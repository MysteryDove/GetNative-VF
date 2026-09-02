import { useState } from "react";
import { Plus } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import type { KernelRef } from "../engine/protocol";
import { kernelDisplayName } from "../engine/displayNames";
import {
  addBicubicGridToScanList,
  addKernelToScanList,
  bicubicRefFromDraft,
  lanczosRefsFromDraft,
  lanczosTapsRange,
  removeKernelFromScanList,
  KERNEL_FAMILY_ORDER,
  type KernelDraft,
} from "../engine/kernelDraft";
import { kernelSignature } from "../engine/heightDraft";

function AddKernelsButton({ t, onClick }: { t: Translator; onClick: () => void }) {
  return (
    <button className="kernel-add-button" type="button" onClick={onClick}>
      <Plus size={13} strokeWidth={2.4} />
      {t("analyze.k.scanList.add")}
    </button>
  );
}

function kernelChipLabel(t: Translator, kernel: KernelRef): string {
  const name = kernelDisplayName(t, kernel.id);
  const params = Object.entries(kernel.parameters);
  if (!params.length) return name;
  return `${name} (${params.map(([key, value]) => `${key}=${value}`).join(", ")})`;
}

/**
 * The hand-built scan list: candidate chips with a remove affordance.
 * Entries are keyed by signature so they can be removed mid-list.
 */
export function KernelScanList({
  t,
  draft,
  work,
  onDraftChange,
}: {
  t: Translator;
  draft: KernelDraft;
  work: number;
  onDraftChange: (updater: (current: KernelDraft) => KernelDraft) => void;
}) {
  function handleRemoveKernel(index: number) {
    onDraftChange((current) => removeKernelFromScanList(current, index));
  }

  if (!draft.scanList.length) {
    return (
      <div className="analyze-table-host">
        <h3>{t("analyze.k.scanList.title")}</h3>
        <p className="empty-copy">{t("analyze.k.scanList.empty")}</p>
      </div>
    );
  }

  const renderedKernels = draft.scanList
    .map((kernel, index) => ({ kernel, index }))
    .sort((a, b) => {
      const familyA = KERNEL_FAMILY_ORDER.indexOf(a.kernel.id as typeof KERNEL_FAMILY_ORDER[number]);
      const familyB = KERNEL_FAMILY_ORDER.indexOf(b.kernel.id as typeof KERNEL_FAMILY_ORDER[number]);
      const familyOrder = (familyA < 0 ? KERNEL_FAMILY_ORDER.length : familyA) -
        (familyB < 0 ? KERNEL_FAMILY_ORDER.length : familyB);
      if (familyOrder !== 0) return familyOrder;
      return kernelChipLabel(t, a.kernel).localeCompare(kernelChipLabel(t, b.kernel), undefined, {
        numeric: true,
        sensitivity: "base",
      });
    });

  return (
    <div className="analyze-table-host">
      <h3>{t("analyze.k.scanList.title")}</h3>
      <div className="candidate-preview" role="table" aria-label={t("analyze.k.scanList.title")}>
        <div className="candidate-preview-meta">
          {t("analyze.k.candidateCount", { count: String(draft.scanList.length) })}
          {work > 0 ? ` · ${t("analyze.workEstimate", { count: String(work) })}` : ""}
        </div>
        <div className="candidate-chips">
          {renderedKernels.map(({ kernel, index }) => {
            const signature = kernelSignature(kernel);
            return (
              <span className="candidate-chip" key={signature}>
                {kernelChipLabel(t, kernel)}
                <button
                  type="button"
                  className="chip-remove"
                  aria-label={t("analyze.k.scanList.remove")}
                  title={t("analyze.k.scanList.remove")}
                  onClick={() => handleRemoveKernel(index)}
                >
                  ×
                </button>
              </span>
            );
          })}
        </div>
        <p className="help-copy">{t("analyze.k.sequenceExact")}</p>
      </div>
    </div>
  );
}

/** The "add to scan list" fieldset: family chips plus per-family parameter inputs. */
export function KernelScanListBuilder({
  t,
  draft,
  capabilities,
  onDraftChange,
}: {
  t: Translator;
  draft: KernelDraft;
  capabilities: EngineEnvelope | null;
  onDraftChange: (updater: (current: KernelDraft) => KernelDraft) => void;
}) {
  const [addNotice, setAddNotice] = useState("");
  // Hoisted: one range lookup per render feeds both the chip count and values.
  const tapsRange = lanczosTapsRange(capabilities);

  function patch(partial: Partial<KernelDraft>) {
    setAddNotice("");
    onDraftChange((current) => ({ ...current, ...partial }));
  }

  function handleAddKernels(refs: KernelRef[]) {
    if (!refs.length) {
      setAddNotice(t("analyze.k.scanList.invalidParams"));
      return;
    }
    let addedAny = false;
    onDraftChange((current) => {
      let next = current;
      for (const ref of refs) {
        const result = addKernelToScanList(next, ref);
        next = result.draft;
        addedAny = addedAny || result.added;
      }
      return next;
    });
    setAddNotice(addedAny ? "" : t("analyze.k.scanList.duplicate"));
  }

  function handleAddFamily() {
    if (draft.addFamily === "bicubic") {
      const ref = bicubicRefFromDraft(draft);
      if (!ref) {
        setAddNotice(t("analyze.k.scanList.invalidParams"));
        return;
      }
      handleAddKernels([ref]);
      return;
    }
    if (draft.addFamily === "lanczos") {
      const refs = lanczosRefsFromDraft(draft);
      if (!refs.length) {
        setAddNotice(t("analyze.k.scanList.invalidParams"));
        return;
      }
      handleAddKernels(refs);
      return;
    }
    handleAddKernels([{ id: draft.addFamily, parameters: {} }]);
  }

  function handleAddBicubicGrid() {
    const result = addBicubicGridToScanList(draft);
    if (!result.ok) {
      setAddNotice(t("analyze.k.scanList.invalidParams"));
      return;
    }
    onDraftChange(() => result.draft);
    setAddNotice(
      t("analyze.k.scanList.gridAdded", {
        added: String(result.added),
        skipped: String(result.skipped),
      }),
    );
  }

  const parameterized = draft.addFamily === "bicubic" || draft.addFamily === "lanczos";

  return (
    <fieldset className="metric-fieldset">
      <legend>{t("analyze.k.scanList.addSection")}</legend>
      <div className="kernel-family-grid">
        {KERNEL_FAMILY_ORDER.map((family) => (
          <button
            key={family}
            type="button"
            className={`candidate-chip ${draft.addFamily === family ? "selected" : ""}`}
            onClick={() => patch({ addFamily: family })}
          >
            {kernelDisplayName(t, family)}
          </button>
        ))}
        {!parameterized ? <AddKernelsButton t={t} onClick={handleAddFamily} /> : null}
      </div>

      {draft.addFamily === "bicubic" ? (
        <>
          <div className="kernel-add-row">
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
            <AddKernelsButton t={t} onClick={handleAddFamily} />
          </div>
          <details className="grid-range">
            <summary>{t("analyze.k.scanList.gridRanges")}</summary>
            <div className="grid-range-body">
              {(["b", "c"] as const).map((axis) => (
                <div className="grid-range-row" key={axis}>
                  <span className="grid-range-axis">{axis}</span>
                  {(
                    [
                      [`${axis}Start`, t("analyze.start")],
                      [`${axis}Stop`, t("analyze.stop")],
                      [`${axis}Step`, t("analyze.step")],
                    ] as const
                  ).map(([key, label]) => (
                    <label key={key}>
                      <span>{label}</span>
                      <input
                        value={draft[key]}
                        onChange={(event) => patch({ [key]: event.target.value })}
                      />
                    </label>
                  ))}
                </div>
              ))}
              <p className="help-copy">{t("analyze.k.gridEndpoints")}</p>
              <button
                className="kernel-add-button"
                type="button"
                onClick={handleAddBicubicGrid}
              >
                <Plus size={13} strokeWidth={2.4} />
                {t("analyze.k.scanList.addGrid")}
              </button>
            </div>
          </details>
        </>
      ) : null}

      {draft.addFamily === "lanczos" ? (
        <>
          <span className="block-label">{t("analyze.k.lanczosTaps")}</span>
          <div className="kernel-group-chips">
            {Array.from(
              { length: tapsRange.max - tapsRange.min + 1 },
              (_, i) => tapsRange.min + i,
            ).map((taps) => (
              <button
                key={taps}
                type="button"
                className={`candidate-chip ${draft.lanczosTapsSelection.includes(taps) ? "selected" : ""}`}
                onClick={() =>
                  patch({
                    lanczosTapsSelection: draft.lanczosTapsSelection.includes(taps)
                      ? draft.lanczosTapsSelection.filter((value) => value !== taps)
                      : [...draft.lanczosTapsSelection, taps],
                  })
                }
              >
                {taps}
              </button>
            ))}
          </div>
          <div className="kernel-family-grid">
            <AddKernelsButton t={t} onClick={handleAddFamily} />
          </div>
        </>
      ) : null}

      {addNotice ? <p className="help-copy warning-copy">{addNotice}</p> : null}
    </fieldset>
  );
}

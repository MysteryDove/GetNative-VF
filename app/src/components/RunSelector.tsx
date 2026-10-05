import { useState } from "react";
import { Trash2 } from "lucide-react";
import type { Translator } from "../i18n";
import type { RunSelectorOption } from "../engine/runSummary";
import type { FilterSelection } from "../engine/multiFilter";
import { Modal } from "./Modal";
import { MultiFilterGroup } from "./MultiFilterGroup";

/**
 * Runs filter shared by the test pages: "All" plus one entry per Run command,
 * each labelled with what it measured. Several entries can be selected
 * together. Right-clicking an entry deletes that
 * run, and the trailing button deletes every run listed here (the page's own
 * filters apply, so hidden runs are untouched) — both after a confirmation
 * that names what will go, and never while a run is still queued or running.
 */
export function RunSelector({
  t,
  options,
  value,
  onChange,
  onDeleteRuns,
  deleteAllLabel,
}: {
  t: Translator;
  options: RunSelectorOption[];
  /** Selected option values; empty shows every run. */
  value: FilterSelection;
  onChange: (value: FilterSelection) => void;
  onDeleteRuns: (runIds: string[]) => void;
  /** e.g. "Delete listed Resolution Test runs". */
  deleteAllLabel: string;
}) {
  const [pending, setPending] = useState<RunSelectorOption | "all" | null>(null);
  const anyActive = options.some((option) => option.active);
  const runCount = options.reduce((sum, option) => sum + option.runIds.length, 0);

  function confirm() {
    if (pending === "all") {
      onDeleteRuns(options.flatMap((option) => option.runIds));
      onChange([]);
    } else if (pending) {
      onDeleteRuns(pending.runIds);
      if (value.includes(pending.value)) {
        onChange(value.filter((entry) => entry !== pending.value));
      }
    }
    setPending(null);
  }

  return (
    <MultiFilterGroup
      label={t("results.filters.runs")}
      allLabel={t("results.filters.all")}
      selected={value}
      onChange={onChange}
      options={options.map((option) => ({
        value: option.value,
        title: `${option.label}\n${t(option.active ? "results.deleteActiveHint" : "results.runContextHint")}`,
        label: (
          <>
            {option.name}
            {option.summary ? (
              <small className="run-filter-summary">{option.summary}</small>
            ) : null}
          </>
        ),
      }))}
      onOptionContextMenu={(optionValue) => {
        const option = options.find((entry) => entry.value === optionValue);
        if (option && !option.active) setPending(option);
      }}
      trailing={
        <>
          <button
            type="button"
            className="icon-button danger run-selector-clear"
            disabled={options.length === 0 || anyActive}
            title={anyActive ? t("results.clearAllActiveHint") : deleteAllLabel}
            aria-label={deleteAllLabel}
            onClick={() => setPending("all")}
          >
            <Trash2 size={14} />
          </button>
      {pending ? (
        <Modal
          onClose={() => setPending(null)}
          title={pending === "all" ? deleteAllLabel : t("results.deleteRun")}
          closeLabel={t("common.close")}
          actions={
            <>
              <button className="secondary-button" type="button" autoFocus onClick={() => setPending(null)}>
                {t("common.cancel")}
              </button>
              <button className="secondary-button danger-button" type="button" onClick={confirm}>
                <Trash2 size={14} />
                {pending === "all" ? deleteAllLabel : t("results.deleteRun")}
              </button>
            </>
          }
        >
          <p className="confirm-dialog-copy">
            {pending === "all"
              ? t("results.deleteListedConfirm", { count: String(runCount) })
              : t("results.deleteSelectionConfirm", { label: pending.label })}
          </p>
        </Modal>
      ) : null}
        </>
      }
    />
  );
}

import { useState } from "react";
import { Trash2 } from "lucide-react";
import type { Translator } from "../i18n";
import type { RunSelectorOption } from "../engine/runSummary";
import { Modal } from "./Modal";

/**
 * Runs filter shared by the test pages: "All" plus one entry per Run command,
 * each labelled with what it measured. Right-clicking an entry deletes that
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
  /** "all" or an option value. */
  value: string;
  onChange: (value: string) => void;
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
      onChange("all");
    } else if (pending) {
      onDeleteRuns(pending.runIds);
      if (value === pending.value) onChange("all");
    }
    setPending(null);
  }

  return (
    <div className="results-filter-group" role="radiogroup" aria-label={t("results.filters.runs")}>
      <span className="results-filter-label">{t("results.filters.runs")}</span>
      <div className="button-radio">
        <button
          type="button"
          role="radio"
          aria-checked={value === "all"}
          className={value === "all" ? "active" : ""}
          onClick={() => onChange("all")}
        >
          {t("results.filters.all")}
        </button>
        {options.map((option) => (
          <button
            key={option.value}
            type="button"
            role="radio"
            aria-checked={value === option.value}
            className={value === option.value ? "active" : ""}
            title={`${option.label}\n${t(option.active ? "results.deleteActiveHint" : "results.runContextHint")}`}
            onClick={() => onChange(option.value)}
            onContextMenu={(event) => {
              event.preventDefault();
              if (!option.active) setPending(option);
            }}
          >
            {option.name}
            {option.summary ? (
              <small className="run-filter-summary">{option.summary}</small>
            ) : null}
          </button>
        ))}
      </div>
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
    </div>
  );
}

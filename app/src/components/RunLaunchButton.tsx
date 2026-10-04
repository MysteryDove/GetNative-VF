import { Play } from "lucide-react";
import type { JSX } from "react";
import type { Translator } from "../i18n";

/**
 * Shared run-launch block used by Analyze/KernelAnalyze/Verify pages: a
 * primary-button with a Play icon that flips to a working label while
 * submitting, plus an optional help-copy line explaining why a run is blocked.
 */
export function RunLaunchButton(props: {
  t: Translator;
  disabled: boolean;
  submitting: boolean;
  label: string;
  blockedReason?: string | null;
  /** Size of the pending run (candidates, work), shown above the button. */
  summary?: string | null;
  onClick: () => void;
}): JSX.Element {
  const { t, disabled, submitting, label, blockedReason, summary, onClick } = props;
  return (
    <div className="analyze-run-block">
      {summary ? <span className="analyze-run-summary">{summary}</span> : null}
      <button className="primary-button" type="button" disabled={disabled} onClick={onClick}>
        <Play size={15} />
        {submitting ? t("diagnostics.working") : label}
      </button>
      {blockedReason ? <p className="help-copy">{blockedReason}</p> : null}
    </div>
  );
}

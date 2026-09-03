import type { ReactNode } from "react";
import { ChevronRight } from "lucide-react";
import type { Translator } from "../i18n";
import type { MetricSpec } from "../engine/protocol";

export function metricSpecSummary(metric: MetricSpec): string {
  return `${metric.cropLeft}/${metric.cropRight}/${metric.cropTop}/${metric.cropBottom} · ${metric.pixelExclusionThreshold} · p=${metric.pNorm}`;
}

export function metricsEqual(left: MetricSpec, right: MetricSpec): boolean {
  return (
    left.cropLeft === right.cropLeft
    && left.cropRight === right.cropRight
    && left.cropTop === right.cropTop
    && left.cropBottom === right.cropBottom
    && left.pixelExclusionThreshold === right.pixelExclusionThreshold
    && left.pNorm === right.pNorm
  );
}

export function MetricSpecSection({
  t,
  open,
  onOpenChange,
  summary,
  children,
}: {
  t: Translator;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  summary?: string;
  children: ReactNode;
}) {
  return (
    <div className="metric-fieldset">
      <button
        type="button"
        className="metric-fieldset-toggle"
        aria-expanded={open}
        onClick={() => onOpenChange(!open)}
      >
        <ChevronRight size={14} className={`metric-fieldset-chevron${open ? " open" : ""}`} />
        <span>{t("analyze.metricSpec")}</span>
        {!open && summary ? <span className="metric-fieldset-summary">{summary}</span> : null}
      </button>
      <div className={`collapsible${open ? " open" : ""}`} aria-hidden={!open} inert={!open}>
        <div className="collapsible-inner">{children}</div>
      </div>
    </div>
  );
}

/**
 * Shared MetricSpec editor (crop ×4 + pixelExclusion + pNorm inputs) used by
 * the height and kernel analyze panes. The pNorm "unsupported" warning stays
 * at the call sites — its condition differs per pane.
 */
export function MetricEditor({
  t,
  metric,
  pNormMaximum,
  onChange,
  disabled = false,
}: {
  t: Translator;
  metric: MetricSpec;
  pNormMaximum: number;
  onChange: (metric: MetricSpec) => void;
  disabled?: boolean;
}) {
  return (
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
              inputMode="numeric"
              value={metric[key]}
              disabled={disabled}
              onChange={(event) =>
                onChange({ ...metric, [key]: Number(event.target.value) })
              }
            />
          </label>
        ))}
        <label className="block">
          <span title={t("analyze.pixelExclusion")}>{t("analyze.pixelExclusion")}</span>
          <input
            inputMode="decimal"
            value={metric.pixelExclusionThreshold}
            disabled={disabled}
            onChange={(event) =>
              onChange({ ...metric, pixelExclusionThreshold: Number(event.target.value) })
            }
          />
        </label>
        <label className="block">
          <span>{t("analyze.pNorm")}</span>
          <input
            inputMode="numeric"
            max={pNormMaximum}
            value={metric.pNorm}
            disabled={disabled}
            onChange={(event) => onChange({ ...metric, pNorm: Number(event.target.value) })}
          />
        </label>
    </div>
  );
}

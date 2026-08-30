import type { Translator } from "../i18n";
import type { MetricSpec } from "../engine/protocol";

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
              inputMode="numeric"
              value={metric[key]}
              disabled={disabled}
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
    </>
  );
}

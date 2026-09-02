import { SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName } from "../engine/displayNames";
import {
  invalidKernelBlur,
  kernelSignature,
  missingFractionalBaseAxis,
  type HeightDraft,
} from "../engine/heightDraft";
import type { BaseMode, KernelRef, SearchPreset } from "../engine/protocol";
import { backendOptionLabel } from "../engine/backendSelection";
import { MetricEditor, MetricSpecSection, metricSpecSummary } from "./MetricEditor";
import { MenuSelect } from "./MenuSelect";
import { RunLaunchButton } from "./RunLaunchButton";

const LANCZOS_COMPARE_TAPS = [3, 4] as const;

function compareKernelTaps(kernelId: string): Array<number | null> {
  return kernelId === "lanczos" ? [...LANCZOS_COMPARE_TAPS] : [null];
}

function isPrimaryCompareOption(draft: HeightDraft, kernelId: string, taps: number | null): boolean {
  if (kernelId !== draft.kernelId) return false;
  if (kernelId === "lanczos") return taps === Number(draft.kernelParameters.taps ?? 3);
  return true;
}

/**
 * Height-scan parameter form (right-hand pane): grid preset/axis/range,
 * kernel selection + compare set, backend, metric spec, launch.
 */
export function HeightParamsPanel({
  t,
  draft,
  capabilities,
  backends,
  resolvedBackend,
  pNormMaximum,
  canRun,
  submitting,
  runBlockedReason,
  onPatch,
  onSetPreset,
  onRun,
  metricSpecOpen,
  onMetricSpecOpenChange,
}: {
  t: Translator;
  draft: HeightDraft;
  capabilities: EngineEnvelope | null;
  backends: HeightDraft["backendPreference"][];
  resolvedBackend: string;
  pNormMaximum: number;
  canRun: boolean;
  submitting: boolean;
  runBlockedReason: string | null;
  onPatch: (partial: Partial<HeightDraft>) => void;
  onSetPreset: (preset: SearchPreset) => void;
  onRun: () => void;
  metricSpecOpen: boolean;
  onMetricSpecOpenChange: (open: boolean) => void;
}) {
  const kernelOptions = capabilities?.payload.kernels ?? [];
  const missingBaseAxis = missingFractionalBaseAxis(draft);
  const baseModes: BaseMode[] = ["integer", "odd", "even"];
  const baseModeField = (axis: "height" | "width") => axis === "height" ? "baseHeightMode" : "baseWidthMode";
  const baseValueField = (axis: "height" | "width") => axis === "height" ? "baseHeight" : "baseWidth";
  const renderBaseMode = (axis: "height" | "width") => {
    const mode = axis === "height" ? draft.baseHeightMode : draft.baseWidthMode;
    const label = axis === "height" ? t("analyze.baseHeight") : t("analyze.baseWidth");
    return (
      <label className="block" key={axis}>
        <span>{label}</span>
        <select
          value={mode}
          aria-label={label}
          onChange={(event) =>
            onPatch({
              [baseModeField(axis)]: event.target.value as BaseMode,
              [baseValueField(axis)]: "",
            })
          }
        >
          {baseModes.map((item) => (
            <option key={item} value={item}>
              {t(`analyze.baseMode.${item}`)}
            </option>
          ))}
        </select>
      </label>
    );
  };
  const showBaseHeight = draft.axisMode !== "w_only";
  const showBaseWidth = draft.axisMode !== "h_only";
  const blurField = (
    <label className="block">
      <span>{t("analyze.blur")}</span>
      <input
        inputMode="decimal"
        title={t("analyze.blurHint")}
        aria-invalid={invalidKernelBlur(draft.kernelParameters) || undefined}
        value={String(draft.kernelParameters.blur ?? 1)}
        onChange={(event) =>
          onPatch({
            kernelParameters: {
              ...draft.kernelParameters,
              blur: event.target.value,
            },
          })
        }
      />
    </label>
  );

  return (
    <aside className="analyze-params pane">
      <h3>
        <SlidersHorizontal size={15} />
        {t("analyze.paramsTitle")}
      </h3>

      <div className="block">
        <span>{t("analyze.preset")}</span>
        <div className="button-radio" role="radiogroup" aria-label={t("analyze.preset")}>
          <button
            type="button"
            role="radio"
            aria-checked={draft.preset === "integer_coarse"}
            className={draft.preset === "integer_coarse" ? "active" : ""}
            onClick={() => onSetPreset("integer_coarse")}
          >
            {t("analyze.preset.integerCoarse")}
          </button>
          <button
            type="button"
            role="radio"
            aria-checked={draft.preset === "fractional_refine"}
            className={draft.preset === "fractional_refine" ? "active" : ""}
            onClick={() => onSetPreset("fractional_refine")}
          >
            {t("analyze.preset.fractionalRefine")}
          </button>
        </div>
      </div>

      <div className="block">
        <span>{t("analyze.axis")}</span>
        <div className="button-radio" role="radiogroup" aria-label={t("analyze.axis")}>
          {(
            [
              ["h_plus_w", "analyze.axis.hPlusW"],
              ["h_only", "analyze.axis.hOnly"],
              ["w_only", "analyze.axis.wOnly"],
            ] as const
          ).map(([mode, key]) => (
            <button
              key={mode}
              type="button"
              role="radio"
              aria-checked={draft.axisMode === mode}
              className={draft.axisMode === mode ? "active" : ""}
              onClick={() => onPatch({ axisMode: mode })}
            >
              {t(key)}
            </button>
          ))}
        </div>
      </div>

      {draft.preset === "fractional_refine" ? (
        <div className="range-grid">
          <label className="block">
            <span>{t("analyze.refineSelected")}</span>
            <input
              value={draft.refineSelected}
              onChange={(event) => onPatch({ refineSelected: event.target.value })}
            />
          </label>
          <label className="block">
            <span>{t("analyze.refineHalfSpan")}</span>
            <input
              value={draft.refineHalfSpan}
              onChange={(event) => onPatch({ refineHalfSpan: event.target.value })}
            />
          </label>
          <label className="block">
            <span>{t("analyze.step")}</span>
            {/* text + inputMode: WebKitGTK number spinners freeze the Linux UI */}
            <input
              inputMode="decimal"
              value={draft.step}
              onChange={(event) => onPatch({ step: event.target.value })}
            />
          </label>
        </div>
      ) : (
        <div className="range-grid">
          <label className="block">
            <span>{t("analyze.start")}</span>
            <input
              value={draft.start}
              onChange={(event) => onPatch({ start: event.target.value })}
            />
          </label>
          <label className="block">
            <span>{t("analyze.stop")}</span>
            <input
              value={draft.stop}
              onChange={(event) => onPatch({ stop: event.target.value })}
            />
          </label>
          <label className="block">
            <span>{t("analyze.step")}</span>
            <input
              inputMode="decimal"
              value={draft.step}
              onChange={(event) => onPatch({ step: event.target.value })}
            />
          </label>
        </div>
      )}

      {showBaseHeight && showBaseWidth ? (
        <div className="metric-grid">
          {renderBaseMode("height")}
          {renderBaseMode("width")}
        </div>
      ) : (
        <>
          {showBaseHeight ? renderBaseMode("height") : null}
          {showBaseWidth ? renderBaseMode("width") : null}
        </>
      )}
      {missingBaseAxis ? (
        <p className="help-copy warning-copy" role="alert">
          {t("analyze.fractionalBaseRequired", {
            base: t(missingBaseAxis === "width" ? "analyze.baseWidth" : "analyze.baseHeight"),
          })}
        </p>
      ) : null}

      <label className="block">
        <span>{t("analyze.fixedKernel")}</span>
        <MenuSelect
          value={draft.kernelId}
          disabled={kernelOptions.length === 0}
          ariaLabel={t("analyze.fixedKernel")}
          options={
            kernelOptions.length === 0
              ? [{ value: draft.kernelId, label: draft.kernelId }]
              : kernelOptions.map((kernel) => ({
                  value: kernel.id,
                  label: kernelDisplayName(t, kernel.id),
                }))
          }
          onChange={(kernelId) => {
            const blur = draft.kernelParameters.blur;
            const withBlur = (
              parameters: HeightDraft["kernelParameters"],
            ): HeightDraft["kernelParameters"] =>
              blur === undefined ? parameters : { ...parameters, blur };
            onPatch({
              kernelId,
              kernelParameters:
                kernelId === "bicubic"
                  ? withBlur({ b: 0, c: 0.5 })
                  : kernelId === "lanczos"
                    ? withBlur({ taps: 3 })
                    : withBlur({}),
              compareKernels: draft.compareKernels.filter((item) => {
                if (item.id !== kernelId) return true;
                if (kernelId === "lanczos") {
                  return Number(item.parameters.taps) !== 3;
                }
                return false;
              }),
            });
          }}
        />
      </label>

      {draft.kernelId === "bicubic" ? (
        <div className="range-grid">
          {(["b", "c"] as const).map((parameter) => (
            <label className="block" key={parameter}>
              <span>{`Bicubic ${parameter}`}</span>
              <input
                inputMode="decimal"
                value={String(draft.kernelParameters[parameter] ?? (parameter === "b" ? 0 : 0.5))}
                onChange={(event) =>
                  onPatch({
                    kernelParameters: {
                      ...draft.kernelParameters,
                      [parameter]: event.target.value,
                    },
                  })
                }
              />
            </label>
          ))}
          {blurField}
        </div>
      ) : draft.kernelId === "lanczos" ? (
        <div className="metric-grid">
          <label className="block">
            <span>{t("analyze.lanczosTaps")}</span>
            <input
              inputMode="numeric"
              value="3"
              readOnly
              aria-readonly="true"
            />
          </label>
          {blurField}
        </div>
      ) : (
        blurField
      )}
      {invalidKernelBlur(draft.kernelParameters) ? (
        <p className="help-copy warning-copy" role="alert">
          {t("analyze.blurInvalid")}
        </p>
      ) : null}

      {kernelOptions.length > 1 ? (
        <div className="block">
          <span>{t("analyze.compareKernels")}</span>
          <div className="kernel-compare-list" role="group" aria-label={t("analyze.compareKernels")}>
            {kernelOptions.flatMap((kernel) => {
              // Capability parameters describe a family, not a runnable
              // candidate. Generate the required explicit parameters here.
              return compareKernelTaps(kernel.id).flatMap((taps) => {
                if (isPrimaryCompareOption(draft, kernel.id, taps)) return [];
                let parameters: KernelRef["parameters"] = {};
                if (kernel.id === "lanczos" && taps != null) {
                  parameters = { taps };
                } else if (kernel.id === "bicubic") {
                  parameters = { b: 0, c: 0.5 };
                }
                const candidate: KernelRef = {
                  id: kernel.id,
                  parameters,
                };
                const signature = kernelSignature(candidate);
                const checked = draft.compareKernels.some(
                  (item) => kernelSignature(item) === signature,
                );
                const name = kernelDisplayName(t, kernel.id);
                const label = taps != null ? `${name} ${taps}` : name;
                return (
                  <button
                    key={taps != null ? `${kernel.id}@${taps}` : kernel.id}
                    className={checked ? "kernel-compare-option active" : "kernel-compare-option"}
                    type="button"
                    aria-pressed={checked}
                    title={label}
                    onClick={() =>
                      onPatch({
                        compareKernels: checked
                          ? draft.compareKernels.filter(
                              (item) => kernelSignature(item) !== signature,
                            )
                          : [...draft.compareKernels, candidate],
                      })
                    }
                  >
                    {label}
                  </button>
                );
              });
            })}
          </div>
        </div>
      ) : null}

      <label className="block">
        <span>{t("analyze.backend")}</span>
        <MenuSelect
          value={draft.backendPreference}
          ariaLabel={t("analyze.backend")}
          title={backendOptionLabel(
            t,
            draft.backendPreference,
            capabilities,
            draft.metric.pNorm,
            draft.axisMode,
          )}
          options={backends.map((backend) => ({
            value: backend,
            label: backendOptionLabel(
              t,
              backend,
              capabilities,
              draft.metric.pNorm,
              draft.axisMode,
            ),
          }))}
          onChange={(backend) =>
            onPatch({
              backendPreference: backend as HeightDraft["backendPreference"],
            })
          }
        />
      </label>

      <MetricSpecSection
        t={t}
        open={metricSpecOpen}
        onOpenChange={onMetricSpecOpenChange}
        summary={metricSpecSummary(draft.metric)}
      >
        <MetricEditor
          t={t}
          metric={draft.metric}
          pNormMaximum={pNormMaximum}
          onChange={(metric) => onPatch({ metric })}
        />
        {draft.metric.pNorm > pNormMaximum ? (
          <span className="help-copy warning-copy">
            {t("analyze.pNormUnsupported", { backend: resolvedBackend })}
          </span>
        ) : null}
      </MetricSpecSection>

      <RunLaunchButton
        t={t}
        disabled={!canRun}
        submitting={submitting}
        label={t("analyze.runHeight")}
        blockedReason={runBlockedReason}
        onClick={onRun}
      />
    </aside>
  );
}

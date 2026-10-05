import { useState } from "react";
import { SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName } from "../engine/displayNames";
import {
  invalidKernelBlur,
  invalidKernelParameterNames,
  isIntegerScan,
  kernelSignature,
  missingFractionalBaseAxis,
  roundIntegerText,
  type HeightDraft,
  type estimateHeightWork,
} from "../engine/heightDraft";
import {
  TRANSFER_CURVES,
  type BaseMode,
  type KernelRef,
  type SearchPreset,
  type TransferCurve,
} from "../engine/protocol";
import { backendOptionLabel } from "../engine/backendSelection";
import { lanczosTapsRange } from "../engine/kernelDraft";
import { MetricEditor, MetricSpecSection, metricSpecSummary } from "./MetricEditor";
import { MenuSelect } from "./MenuSelect";
import { RunLaunchButton } from "./RunLaunchButton";

const LANCZOS_COMPARE_TAPS = [3, 4] as const;

/** Step sizes that cover the usual getnative scans; anything else is "custom". */
const FRACTIONAL_STEPS = ["1", "0.5", "0.1", "0.05", "0.01"] as const;
const INTEGER_STEPS = ["1", "2", "4", "8"] as const;
const CUSTOM_STEP = "custom";

/** Step as a pull-down of common px values with a free-form fallback. */
function StepField({
  t,
  value,
  integer,
  onChange,
}: {
  t: Translator;
  value: string;
  /** Integer scan: whole-pixel steps only; typed decimals round on blur. */
  integer: boolean;
  onChange: (step: string) => void;
}) {
  const steps: readonly string[] = integer ? INTEGER_STEPS : FRACTIONAL_STEPS;
  const isCommon = steps.includes(value.trim());
  const [custom, setCustom] = useState(!isCommon);
  const showInput = custom || !isCommon;
  return (
    <label className="block">
      <span>{t("analyze.step")}</span>
      <select
        aria-label={t("analyze.step")}
        value={showInput ? CUSTOM_STEP : value.trim()}
        onChange={(event) => {
          if (event.target.value === CUSTOM_STEP) {
            setCustom(true);
            return;
          }
          setCustom(false);
          onChange(event.target.value);
        }}
      >
        {steps.map((step) => (
          <option key={step} value={step}>{`${step} ${t("common.px")}`}</option>
        ))}
        <option value={CUSTOM_STEP}>{t("analyze.stepCustom")}</option>
      </select>
      {showInput ? (
        // text + inputMode: WebKitGTK number spinners freeze the Linux UI
        <input
          inputMode={integer ? "numeric" : "decimal"}
          aria-label={t("analyze.step")}
          value={value}
          onChange={(event) => onChange(event.target.value)}
          onBlur={() => {
            if (integer) onChange(roundIntegerText(value, 1));
          }}
        />
      ) : null}
    </label>
  );
}

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
  work,
  metricSpecOpen,
  onMetricSpecOpenChange,
  resolvedBases,
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
  work: ReturnType<typeof estimateHeightWork>;
  metricSpecOpen: boolean;
  onMetricSpecOpenChange: (open: boolean) => void;
  /** Base values the scan will send, per axis; null when unresolved. */
  resolvedBases: { height: string | null; width: string | null } | null;
}) {
  const kernelOptions = capabilities?.payload.kernels ?? [];
  const missingBaseAxis = missingFractionalBaseAxis(draft);
  const invalidParameters = invalidKernelParameterNames(draft.kernelParameters);
  const scansWidth = draft.axisMode === "w_only";
  const transfer: TransferCurve = draft.transfer ?? "none";
  const primaryTaps = Number(draft.kernelParameters.taps ?? 3);
  const tapsRange = lanczosTapsRange(capabilities);
  const tapsOptions = Array.from(
    { length: tapsRange.max - tapsRange.min + 1 },
    (_, index) => tapsRange.min + index,
  );
  if (!tapsOptions.includes(primaryTaps)) tapsOptions.push(primaryTaps);
  const integerScan = isIntegerScan(draft);
  const baseModes: BaseMode[] = ["integer", "odd", "even"];
  const baseModeField = (axis: "height" | "width") => axis === "height" ? "baseHeightMode" : "baseWidthMode";
  const baseValueField = (axis: "height" | "width") => axis === "height" ? "baseHeight" : "baseWidth";
  // H+W with a parity base height and no width parity: the width canvas takes
  // the source width's parity, so "integer" would misname it.
  const widthFollowsHeight = draft.axisMode === "h_plus_w"
    && (draft.baseHeightMode !== "integer" || Boolean(draft.baseHeight.trim()));
  const renderBaseMode = (axis: "height" | "width") => {
    const mode = axis === "height" ? draft.baseHeightMode : draft.baseWidthMode;
    const label = axis === "height" ? t("analyze.baseHeight") : t("analyze.baseWidth");
    // The scanned axis needs a parity canvas; only the H+W width may follow.
    const isScannedAxis = axis === (draft.axisMode === "w_only" ? "width" : "height");
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
          {baseModes.filter((item) => item !== "integer" || !isScannedAxis).map((item) => (
            <option key={item} value={item}>
              {item === "integer" && axis === "width" && widthFollowsHeight
                ? t("analyze.baseMode.followHeight")
                : t(`analyze.baseMode.${item}`)}
            </option>
          ))}
        </select>
        {resolvedBases?.[axis] ? (
          <small className="base-resolved" title={t("analyze.baseResolvedHint")}>
            {t("analyze.baseResolved", { value: resolvedBases[axis] as string })}
          </small>
        ) : null}
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

      <span className="block-label">
        {t(scansWidth ? "analyze.rangeWidth" : "analyze.rangeHeight")}
      </span>
      <div className="range-grid">
        <label className="block">
          <span>{t("analyze.start")}</span>
          <input
            value={draft.start}
            inputMode={integerScan ? "numeric" : "decimal"}
            onChange={(event) => onPatch({ start: event.target.value })}
            onBlur={() => {
              if (integerScan) onPatch({ start: roundIntegerText(draft.start) });
            }}
          />
        </label>
        <label className="block">
          <span>{t("analyze.stop")}</span>
          <input
            value={draft.stop}
            inputMode={integerScan ? "numeric" : "decimal"}
            onChange={(event) => onPatch({ stop: event.target.value })}
            onBlur={() => {
              if (integerScan) onPatch({ stop: roundIntegerText(draft.stop) });
            }}
          />
        </label>
        <StepField
          t={t}
          value={draft.step}
          integer={integerScan}
          onChange={(step) => onPatch({ step })}
        />
      </div>

      {integerScan ? null : showBaseHeight && showBaseWidth ? (
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
                title={t("analyze.fractionHint")}
                aria-invalid={invalidParameters.includes(parameter) || undefined}
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
            <select
              aria-label={t("analyze.lanczosTaps")}
              value={String(primaryTaps)}
              onChange={(event) => {
                const taps = Number(event.target.value);
                onPatch({
                  kernelParameters: { ...draft.kernelParameters, taps },
                  // The chosen taps becomes the fixed kernel, so it leaves the
                  // compare set instead of running twice.
                  compareKernels: draft.compareKernels.filter(
                    (item) => !(item.id === "lanczos" && Number(item.parameters.taps) === taps),
                  ),
                });
              }}
            >
              {tapsOptions.map((taps) => (
                <option key={taps} value={taps}>{taps}</option>
              ))}
            </select>
          </label>
          {blurField}
        </div>
      ) : (
        blurField
      )}
      {invalidParameters.length ? (
        <p className="help-copy warning-copy" role="alert">
          {t("analyze.kernelParamInvalid", { name: invalidParameters.join(", ") })}
        </p>
      ) : invalidKernelBlur(draft.kernelParameters) ? (
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

      {capabilities?.payload.features?.analysis_transfer ? (
        <>
          <label className="block">
            <span>{t("analyze.transfer")}</span>
            <select
              aria-label={t("analyze.transfer")}
              value={transfer}
              onChange={(event) => onPatch({ transfer: event.target.value as TransferCurve })}
            >
              {TRANSFER_CURVES.map((curve) => (
                <option key={curve} value={curve}>{t(`analyze.transfer.${curve}`)}</option>
              ))}
            </select>
          </label>
          <p className={`help-copy${transfer !== "none" ? " warning-copy" : ""}`}>
            {t(transfer === "none" ? "analyze.transferHint" : "analyze.transferActive")}
          </p>
        </>
      ) : null}

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
        summary={
          work.ok
            ? `${t("analyze.candidateCount", { count: String(work.candidateCount) })} · ${t("analyze.workEstimate", { count: String(work.estimate) })}`
            : null
        }
        onClick={onRun}
      />
    </aside>
  );
}

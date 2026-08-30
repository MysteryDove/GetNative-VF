import { Check, RotateCcw, SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import {
  invalidKernelBlur,
  kernelSignature,
  missingFractionalBaseAxis,
  type HeightDraft,
} from "../engine/heightDraft";
import type { BaseMode, KernelRef, SearchPreset } from "../engine/protocol";
import { backendOptionLabel } from "../engine/backendSelection";
import { MetricEditor } from "./MetricEditor";
import { RunLaunchButton } from "./RunLaunchButton";

/**
 * Height-scan parameter form (right-hand pane): grid preset/axis/range,
 * kernel selection + compare set, profile, backend, metric spec, launch.
 */
export function HeightParamsPanel({
  t,
  draft,
  capabilities,
  backends,
  kernelCount,
  resolvedBackend,
  pNormMaximum,
  canRun,
  submitting,
  runBlockedReason,
  onPatch,
  onSetPreset,
  onResetProfileDefaults,
  onRun,
}: {
  t: Translator;
  draft: HeightDraft;
  capabilities: EngineEnvelope | null;
  backends: HeightDraft["backendPreference"][];
  kernelCount: number;
  resolvedBackend: string;
  pNormMaximum: number;
  canRun: boolean;
  submitting: boolean;
  runBlockedReason: string | null;
  onPatch: (partial: Partial<HeightDraft>) => void;
  onSetPreset: (preset: SearchPreset) => void;
  onResetProfileDefaults: () => void;
  onRun: () => void;
}) {
  const kernelOptions = capabilities?.payload.kernels ?? [];
  const profileOptions = capabilities?.payload.profiles ?? [];
  const missingBaseAxis = missingFractionalBaseAxis(draft);
  const baseModes: BaseMode[] = ["integer", "odd", "even"];
  const baseModeField = (axis: "height" | "width") => axis === "height" ? "baseHeightMode" : "baseWidthMode";
  const baseValueField = (axis: "height" | "width") => axis === "height" ? "baseHeight" : "baseWidth";
  const renderBaseMode = (axis: "height" | "width") => {
    const mode = axis === "height" ? draft.baseHeightMode : draft.baseWidthMode;
    const label = axis === "height" ? t("analyze.baseHeight") : t("analyze.baseWidth");
    return (
      <div className="block" key={axis}>
        <span>{label}</span>
        <div className="button-radio base-mode-radio" role="radiogroup" aria-label={label}>
          {baseModes.map((item) => (
            <button
              key={item}
              type="button"
              role="radio"
              aria-checked={mode === item}
              className={mode === item ? "active" : ""}
              onClick={() => onPatch({
                [baseModeField(axis)]: item,
                [baseValueField(axis)]: "",
              })}
            >
              {t(`analyze.baseMode.${item}`)}
            </button>
          ))}
        </div>
      </div>
    );
  };

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
        <>
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
        </>
      ) : (
        <>
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
        </>
      )}

      <label className="block">
        <span>{t("analyze.endpointRule")}</span>
        <select
          value={draft.endpointRule}
          onChange={(event) =>
            onPatch({ endpointRule: event.target.value as HeightDraft["endpointRule"] })
          }
        >
          <option value="inclusive">{t("analyze.endpoint.inclusive")}</option>
          <option value="exclusive_stop">{t("analyze.endpoint.exclusive")}</option>
        </select>
      </label>

      {draft.axisMode !== "w_only" ? renderBaseMode("height") : null}
      {draft.axisMode !== "h_only" ? renderBaseMode("width") : null}
      <p className="help-copy">{t("analyze.baseParityHint")}</p>
      {missingBaseAxis ? (
        <p className="help-copy warning-copy" role="alert">
          {t("analyze.fractionalBaseRequired", {
            base: t(missingBaseAxis === "width" ? "analyze.baseWidth" : "analyze.baseHeight"),
          })}
        </p>
      ) : null}

      <label className="block">
        <span>{t("analyze.fixedKernel")}</span>
        <select
          value={draft.kernelId}
          disabled={kernelOptions.length === 0}
          onChange={(event) => {
            const blur = draft.kernelParameters.blur;
            const withBlur = (
              parameters: HeightDraft["kernelParameters"],
            ): HeightDraft["kernelParameters"] =>
              blur === undefined ? parameters : { ...parameters, blur };
            onPatch({
              kernelId: event.target.value,
              kernelParameters:
                event.target.value === "bicubic"
                  ? withBlur({ b: 0, c: 0.5 })
                  : event.target.value === "lanczos"
                    ? withBlur({ taps: 3 })
                    : withBlur({}),
              compareKernels: draft.compareKernels.filter(
                (item) => item.id !== event.target.value,
              ),
            });
          }}
        >
          {kernelOptions.length === 0 ? (
            <option value={draft.kernelId}>{draft.kernelId}</option>
          ) : (
            kernelOptions.map((kernel) => (
              <option key={kernel.id} value={kernel.id}>
                {kernelDisplayName(t, kernel.id)}
              </option>
            ))
          )}
        </select>
      </label>

      {draft.kernelId === "bicubic" ? (
        <div className="metric-grid">
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
        </div>
      ) : null}
      {draft.kernelId === "lanczos" ? (
        <label className="block">
          <span>{t("analyze.lanczosTaps")}</span>
          <input
            inputMode="numeric"
            value="3"
            readOnly
            aria-readonly="true"
          />
        </label>
      ) : null}

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
      {invalidKernelBlur(draft.kernelParameters) ? (
        <p className="help-copy warning-copy" role="alert">
          {t("analyze.blurInvalid")}
        </p>
      ) : null}

      {kernelOptions.length > 1 ? (
        <fieldset className="metric-fieldset kernel-compare-fieldset">
          <legend>{t("analyze.compareKernels")}</legend>
          <div className="kernel-compare-list">
            {kernelOptions
              .filter((kernel) => kernel.id !== draft.kernelId)
              .flatMap((kernel) => {
                // Capability parameters describe a family, not a runnable
                // candidate. Generate the required explicit parameters here.
                const variants: Array<number | null> = kernel.id === "lanczos" ? [3] : [null];
                return variants.map((taps) => {
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
                  return (
                    <button
                      key={taps != null ? `${kernel.id}@${taps}` : kernel.id}
                      className={checked ? "kernel-compare-option active" : "kernel-compare-option"}
                      type="button"
                      aria-pressed={checked}
                      title={taps != null ? `${name} ${taps}` : name}
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
                      <Check className="kernel-compare-check" size={12} aria-hidden="true" />
                      <span>{taps != null ? `${name} ${taps}` : name}</span>
                    </button>
                  );
                });
              })}
          </div>
          {draft.compareKernels.length > 0 ? (
            <p className="help-copy">
              {t("analyze.compareKernelsHelp", { count: String(kernelCount) })}
            </p>
          ) : null}
        </fieldset>
      ) : null}

      <label className="block">
        <span>{t("diagnostics.profile")}</span>
        <select
          value={draft.profileId}
          onChange={(event) => onPatch({ profileId: event.target.value })}
        >
          {profileOptions.length === 0 ? (
            <option value={draft.profileId}>{draft.profileId}</option>
          ) : (
            profileOptions.map((profile) => (
              <option key={profile.id} value={profile.id}>
                {profileDisplayName(t, profile.id)}
              </option>
            ))
          )}
        </select>
      </label>
      <button
        className="secondary-button profile-defaults-button"
        type="button"
        title={t("analyze.applyProfileDefaults")}
        onClick={onResetProfileDefaults}
      >
        <RotateCcw size={14} />
        {t("analyze.applyProfileDefaults")}
      </button>

      <label className="block">
        <span>{t("analyze.backend")}</span>
        <select
          className="backend-select"
          value={draft.backendPreference}
          title={backendOptionLabel(
            t,
            draft.backendPreference,
            capabilities,
            draft.metric.pNorm,
            draft.axisMode,
          )}
          onChange={(event) =>
            onPatch({
              backendPreference: event.target.value as HeightDraft["backendPreference"],
            })
          }
        >
          {backends.map((backend) => (
            <option key={backend} value={backend}>
              {backendOptionLabel(
                t,
                backend,
                capabilities,
                draft.metric.pNorm,
                draft.axisMode,
              )}
            </option>
          ))}
        </select>
      </label>

      <fieldset className="metric-fieldset">
        <legend>{t("analyze.metricSpec")}</legend>
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
      </fieldset>

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

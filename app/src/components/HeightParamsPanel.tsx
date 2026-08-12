import { RotateCcw, SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import { kernelSignature, type HeightDraft } from "../engine/heightDraft";
import type { KernelRef, SearchPreset } from "../engine/protocol";
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
            <input
              type="number"
              min="0.01"
              step="0.01"
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
              type="number"
              min="0.01"
              step="0.01"
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

      <label className="block">
        <span>{t("analyze.baseHeight")}</span>
        <input
          value={draft.baseHeight}
          onChange={(event) => onPatch({ baseHeight: event.target.value })}
          placeholder={t("analyze.optional")}
        />
      </label>
      <label className="block">
        <span>{t("analyze.baseWidth")}</span>
        <input
          value={draft.baseWidth}
          onChange={(event) => onPatch({ baseWidth: event.target.value })}
          placeholder={t("analyze.optional")}
        />
      </label>
      <p className="help-copy">{t("analyze.baseParityHint")}</p>

      <label className="block">
        <span>{t("analyze.fixedKernel")}</span>
        <select
          value={draft.kernelId}
          disabled={kernelOptions.length === 0}
          onChange={(event) => {
            onPatch({
              kernelId: event.target.value,
              kernelParameters:
                event.target.value === "bicubic"
                  ? { b: 0, c: 0.5 }
                  : event.target.value === "lanczos"
                    ? { taps: 3 }
                    : {},
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
                type="number"
                step="any"
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
            type="number"
            min={1}
            max={15}
            step={1}
            value={String(draft.kernelParameters.taps ?? 3)}
            onChange={(event) =>
              onPatch({
                kernelParameters: { ...draft.kernelParameters, taps: event.target.value },
              })
            }
          />
        </label>
      ) : null}

      {kernelOptions.length > 1 ? (
        <fieldset className="metric-fieldset kernel-compare-fieldset">
          <legend>{t("analyze.compareKernels")}</legend>
          <div className="kernel-compare-list">
            {kernelOptions
              .filter((kernel) => kernel.id !== draft.kernelId)
              .flatMap((kernel) => {
                // Kernels with a taps parameter (lanczos) are offered per-taps.
                const variants: Array<number | null> =
                  "taps" in kernel.parameters ? [2, 3, 4, 5, 6] : [null];
                return variants.map((taps) => {
                  const candidate: KernelRef = {
                    id: kernel.id,
                    parameters:
                      taps != null
                        ? { ...kernel.parameters, taps }
                        : { ...kernel.parameters },
                  };
                  const signature = kernelSignature(candidate);
                  const checked = draft.compareKernels.some(
                    (item) => kernelSignature(item) === signature,
                  );
                  const name = kernelDisplayName(t, kernel.id);
                  return (
                    <label
                      key={taps != null ? `${kernel.id}@${taps}` : kernel.id}
                      className="checkbox-row"
                    >
                      <input
                        type="checkbox"
                        checked={checked}
                        onChange={(event) =>
                          onPatch({
                            compareKernels: event.target.checked
                              ? [...draft.compareKernels, candidate]
                              : draft.compareKernels.filter(
                                  (item) => kernelSignature(item) !== signature,
                                ),
                          })
                        }
                      />
                      <span>{taps != null ? `${name} ${taps}` : name}</span>
                    </label>
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
        className="secondary-button"
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

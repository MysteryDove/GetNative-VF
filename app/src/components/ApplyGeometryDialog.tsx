import { useMemo, useState } from "react";
import type { Translator } from "../i18n";
import type { AxisMode, BaseMode, GeometrySnapshot } from "../engine/protocol";
import { baseForMode, resolveGeometryValues } from "../engine/geometry";
import { Modal } from "./Modal";

export type ApplyGeometryValues = {
  srcHeight?: number | null;
  srcWidth?: number | null;
  baseHeight: number | null;
  baseWidth: number | null;
  baseHeightMode?: BaseMode;
  baseWidthMode?: BaseMode;
  /** True when the measuring Run's kernel should become the Recipe kernel. */
  applyKernel?: boolean;
  /** Axis mode to store; H+W when a single-axis result is combined with the other axis. */
  axisMode?: AxisMode;
};

function parseField(text: string): number | null | "invalid" {
  const trimmed = text.trim();
  if (!trimmed) return null;
  const value = Number(trimmed);
  return Number.isFinite(value) && value > 0 ? value : "invalid";
}

function modeLabel(t: Translator, mode: BaseMode): string {
  return t(`analyze.baseMode.${mode}`);
}

export function ApplyGeometryDialog({
  t,
  busy,
  axisMode,
  sourceWidth,
  sourceHeight,
  initialSrcWidth,
  initialSrcHeight,
  initialBaseHeightMode = "integer",
  initialBaseWidthMode = "integer",
  kernelLabel,
  fromRun = false,
  otherAxisPrefilled = false,
  onCancel,
  onConfirm,
}: {
  t: Translator;
  busy: boolean;
  axisMode: AxisMode;
  sourceWidth: number;
  sourceHeight: number;
  initialSrcWidth?: number | null;
  initialSrcHeight?: number | null;
  /** Base modes of the scan that measured the selection; carried over as-is. */
  initialBaseHeightMode?: BaseMode;
  initialBaseWidthMode?: BaseMode;
  /** Kernel of the measuring Run; offered as the Recipe kernel when set. */
  kernelLabel?: string | null;
  /** True when the selection is tied to a Run whose settings prefill the dialog. */
  fromRun?: boolean;
  /**
   * Single-axis scans only: true when the other axis's initial src value comes
   * from the user's last selection on that axis, so combining both is offered
   * pre-checked (the non-proportional / anamorphic descale workflow).
   */
  otherAxisPrefilled?: boolean;
  onCancel: () => void;
  onConfirm: (values: ApplyGeometryValues) => void;
}) {
  const singleAxis = axisMode !== "h_plus_w";
  // A single-axis result can be applied together with the other axis as one
  // independent-axes H+W geometry, without switching the scan back to H+W.
  const [combine, setCombine] = useState(singleAxis && otherAxisPrefilled);
  const effectiveAxis: AxisMode = singleAxis && combine ? "h_plus_w" : axisMode;
  const initialHeight = initialSrcHeight ?? sourceHeight;
  const initialWidth = initialSrcWidth ??
    (axisMode === "h_plus_w" ? sourceWidth * initialHeight / sourceHeight : sourceWidth);
  const [heightText, setHeightText] = useState(String(initialHeight));
  const [widthText, setWidthText] = useState(String(initialWidth));
  const [heightMode, setHeightMode] = useState<BaseMode>(initialBaseHeightMode);
  const [widthMode, setWidthMode] = useState<BaseMode>(initialBaseWidthMode);
  const [applyKernel, setApplyKernel] = useState(true);
  const [error, setError] = useState("");

  // Only an axis that is actually being set carries a parity base.
  const appliedHeightMode: BaseMode = effectiveAxis === "w_only" ? "integer" : heightMode;
  const appliedWidthMode: BaseMode = effectiveAxis === "h_only" ? "integer" : widthMode;
  const parsedHeight = parseField(heightText);
  const parsedWidth = parseField(widthText);
  const preview = useMemo<GeometrySnapshot | null>(() => {
    if (parsedHeight === "invalid" || parsedWidth === "invalid") return null;
    const srcHeight = effectiveAxis === "w_only" ? sourceHeight : parsedHeight ?? sourceHeight;
    const srcWidth = effectiveAxis === "h_only" ? sourceWidth : parsedWidth ?? sourceWidth;
    if (srcHeight == null || srcWidth == null) return null;
    try {
      return resolveGeometryValues({
        sourceWidth,
        sourceHeight,
        srcWidth,
        srcHeight,
        baseHeight: baseForMode(srcHeight, appliedHeightMode),
        baseWidth: baseForMode(srcWidth, appliedWidthMode),
      });
    } catch {
      return null;
    }
  }, [effectiveAxis, parsedHeight, parsedWidth, sourceWidth, sourceHeight, appliedHeightMode, appliedWidthMode]);

  function handleConfirm() {
    if (parsedHeight === "invalid" || parsedWidth === "invalid") {
      setError(t("analyze.applyDialog.invalid"));
      return;
    }
    const srcHeight = effectiveAxis === "w_only" ? null : parsedHeight;
    const srcWidth = effectiveAxis === "h_only" ? null : parsedWidth;
    const effectiveHeight = effectiveAxis === "w_only" ? sourceHeight : parsedHeight;
    const effectiveWidth = effectiveAxis === "h_only" ? sourceWidth : parsedWidth;
    if (effectiveHeight == null && effectiveWidth == null) {
      setError(t("analyze.applyDialog.invalid"));
      return;
    }
    onConfirm({
      srcHeight,
      srcWidth,
      baseHeight: effectiveHeight == null ? null : baseForMode(effectiveHeight, appliedHeightMode),
      baseWidth: effectiveWidth == null ? null : baseForMode(effectiveWidth, appliedWidthMode),
      baseHeightMode: appliedHeightMode,
      baseWidthMode: appliedWidthMode,
      applyKernel: Boolean(kernelLabel) && applyKernel,
      axisMode: effectiveAxis,
    });
  }

  const modes: BaseMode[] = ["integer", "even", "odd"];
  const field = (axis: "height" | "width") => {
    const isHeight = axis === "height";
    const visible = effectiveAxis === "h_plus_w"
      || (isHeight ? effectiveAxis === "h_only" : effectiveAxis === "w_only");
    if (!visible) return null;
    const text = isHeight ? heightText : widthText;
    const setText = isHeight ? setHeightText : setWidthText;
    const mode = isHeight ? heightMode : widthMode;
    const setMode = isHeight ? setHeightMode : setWidthMode;
    return (
      <div className="geometry-axis" key={axis}>
        <div className="geometry-axis-fields">
          <label className="geometry-field">
            <span>{t(isHeight ? "analyze.srcHeight" : "analyze.srcWidth")}</span>
            <input
              value={text}
              inputMode="decimal"
              onChange={(event) => {
                setText(event.target.value);
                setError("");
              }}
            />
          </label>
          <label className="geometry-field">
            <span>{t("analyze.baseMode")}</span>
            <select value={mode} onChange={(event) => setMode(event.target.value as BaseMode)}>
              {modes.map((item) => <option key={item} value={item}>{modeLabel(t, item)}</option>)}
            </select>
          </label>
        </div>
      </div>
    );
  };

  return (
    <Modal
      onClose={onCancel}
      title={t("analyze.applyToRecipe")}
      closeLabel={t("common.close")}
      actions={
        <>
          <button className="secondary-button" type="button" onClick={onCancel}>{t("common.cancel")}</button>
          <button className="primary-button" type="button" disabled={busy} onClick={handleConfirm}>{t("analyze.applyToRecipe")}</button>
        </>
      }
    >
      <p className="help-copy">
        {t("analyze.applyDialog.hint")}
        {fromRun ? ` ${t("analyze.applyDialog.fromRun")}` : ""}
      </p>
      {singleAxis ? (
        <label className="checkbox-row">
          <input
            type="checkbox"
            checked={combine}
            onChange={(event) => {
              setCombine(event.target.checked);
              setError("");
            }}
          />
          <span>
            {t("analyze.applyDialog.combine", {
              axis: t(axisMode === "h_only" ? "analyze.srcWidth" : "analyze.srcHeight"),
            })}
          </span>
        </label>
      ) : null}
      {singleAxis && combine ? (
        <p className="help-copy">
          {t("analyze.applyDialog.combineHint")}
          {otherAxisPrefilled
            ? ` ${t("analyze.applyDialog.fromOtherAxis", {
                axis: t(axisMode === "h_only" ? "analyze.srcWidth" : "analyze.srcHeight"),
              })}`
            : ""}
        </p>
      ) : null}
      {field("height")}
      {field("width")}
      {preview ? (
        <div className="dense-table">
          <div className="dense-row"><strong>{t("analyze.geometryPreview")}</strong><span>{preview.canvasWidth}×{preview.canvasHeight}</span></div>
          <div className="dense-row"><strong>src</strong><span>{preview.srcWidth}×{preview.srcHeight} @ ({preview.srcLeft}, {preview.srcTop})</span></div>
          <div className="dense-row"><strong>{t("analyze.baseWidth")}</strong><span>{preview.baseWidth ?? "null"}</span></div>
          <div className="dense-row"><strong>{t("analyze.baseHeight")}</strong><span>{preview.baseHeight ?? "null"}</span></div>
        </div>
      ) : null}
      {kernelLabel ? (
        <label className="checkbox-row">
          <input
            type="checkbox"
            checked={applyKernel}
            onChange={(event) => setApplyKernel(event.target.checked)}
          />
          <span>{t("analyze.applyDialog.kernel", { kernel: kernelLabel })}</span>
        </label>
      ) : null}
      {error ? <p className="help-copy warning-copy">{error}</p> : null}
    </Modal>
  );
}

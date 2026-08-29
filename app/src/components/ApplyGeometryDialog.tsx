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
  onCancel: () => void;
  onConfirm: (values: ApplyGeometryValues) => void;
}) {
  const initialHeight = initialSrcHeight ?? sourceHeight;
  const initialWidth = initialSrcWidth ??
    (axisMode === "h_plus_w" ? sourceWidth * initialHeight / sourceHeight : sourceWidth);
  const [heightText, setHeightText] = useState(String(initialHeight));
  const [widthText, setWidthText] = useState(String(initialWidth));
  const [heightMode, setHeightMode] = useState<BaseMode>("integer");
  const [widthMode, setWidthMode] = useState<BaseMode>("integer");
  const [error, setError] = useState("");

  const parsedHeight = parseField(heightText);
  const parsedWidth = parseField(widthText);
  const preview = useMemo<GeometrySnapshot | null>(() => {
    if (parsedHeight === "invalid" || parsedWidth === "invalid") return null;
    const srcHeight = axisMode === "w_only" ? sourceHeight : parsedHeight ?? sourceHeight;
    const srcWidth = axisMode === "h_only" ? sourceWidth : parsedWidth ?? sourceWidth;
    if (srcHeight == null || srcWidth == null) return null;
    try {
      return resolveGeometryValues({
        sourceWidth,
        sourceHeight,
        srcWidth,
        srcHeight,
        baseHeight: baseForMode(srcHeight, heightMode),
        baseWidth: baseForMode(srcWidth, widthMode),
      });
    } catch {
      return null;
    }
  }, [axisMode, parsedHeight, parsedWidth, sourceWidth, sourceHeight, heightMode, widthMode]);

  function handleConfirm() {
    if (parsedHeight === "invalid" || parsedWidth === "invalid") {
      setError(t("analyze.applyDialog.invalid"));
      return;
    }
    const srcHeight = axisMode === "w_only" ? null : parsedHeight;
    const srcWidth = axisMode === "h_only" ? null : parsedWidth;
    const effectiveHeight = axisMode === "w_only" ? sourceHeight : parsedHeight;
    const effectiveWidth = axisMode === "h_only" ? sourceWidth : parsedWidth;
    if (effectiveHeight == null && effectiveWidth == null) {
      setError(t("analyze.applyDialog.invalid"));
      return;
    }
    onConfirm({
      srcHeight,
      srcWidth,
      baseHeight: effectiveHeight == null ? null : baseForMode(effectiveHeight, heightMode),
      baseWidth: effectiveWidth == null ? null : baseForMode(effectiveWidth, widthMode),
      baseHeightMode: heightMode,
      baseWidthMode: widthMode,
    });
  }

  const modes: BaseMode[] = ["integer", "even", "odd"];
  const field = (axis: "height" | "width") => {
    const isHeight = axis === "height";
    const visible = axisMode === "h_plus_w" || (isHeight ? axisMode === "h_only" : axisMode === "w_only");
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
      <p className="help-copy">{t("analyze.applyDialog.hint")}</p>
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
      {error ? <p className="help-copy warning-copy">{error}</p> : null}
    </Modal>
  );
}

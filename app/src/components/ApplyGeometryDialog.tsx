import { useState } from "react";
import type { Translator } from "../i18n";
import { Modal } from "./Modal";

export type ApplyGeometryValues = {
  baseHeight: number | null;
  baseWidth: number | null;
};

function parseField(text: string): number | null | "invalid" {
  const trimmed = text.trim();
  if (!trimmed) return null;
  const value = Number(trimmed);
  return Number.isFinite(value) && value > 0 ? value : "invalid";
}

/**
 * Popup form behind 应用到方案草稿: the user fills base height/width by hand,
 * then the geometry is resolved and applied to the Recipe Draft. An empty
 * side stays automatic (derived proportionally at apply).
 */
export function ApplyGeometryDialog({
  t,
  busy,
  onCancel,
  onConfirm,
}: {
  t: Translator;
  busy: boolean;
  onCancel: () => void;
  onConfirm: (values: ApplyGeometryValues) => void;
}) {
  const [heightText, setHeightText] = useState("");
  const [widthText, setWidthText] = useState("");
  const [error, setError] = useState("");

  function handleConfirm() {
    const baseHeight = parseField(heightText);
    const baseWidth = parseField(widthText);
    if (baseHeight === "invalid" || baseWidth === "invalid" || (baseHeight == null && baseWidth == null)) {
      setError(t("analyze.applyDialog.invalid"));
      return;
    }
    onConfirm({ baseHeight, baseWidth });
  }

  return (
    <Modal
      onClose={onCancel}
      title={t("analyze.applyToRecipe")}
      closeLabel={t("common.close")}
      actions={
        <>
          <button className="secondary-button" type="button" onClick={onCancel}>
            {t("common.cancel")}
          </button>
          <button className="primary-button" type="button" disabled={busy} onClick={handleConfirm}>
            {t("analyze.applyToRecipe")}
          </button>
        </>
      }
    >
      <p className="help-copy">{t("analyze.applyDialog.hint")}</p>
      <label className="block">
        <span>{t("analyze.baseHeight")}</span>
        <input
          value={heightText}
          inputMode="decimal"
          onChange={(event) => {
            setHeightText(event.target.value);
            setError("");
          }}
        />
      </label>
      <label className="block">
        <span>{t("analyze.baseWidth")}</span>
        <input
          value={widthText}
          inputMode="decimal"
          onChange={(event) => {
            setWidthText(event.target.value);
            setError("");
          }}
        />
      </label>
      {error ? <p className="help-copy warning-copy">{error}</p> : null}
    </Modal>
  );
}

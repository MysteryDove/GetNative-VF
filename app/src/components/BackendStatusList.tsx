import { Check, TriangleAlert, X } from "lucide-react";
import type { Translator } from "../i18n";
import { backendDisplayState, type BackendDisplayRow } from "../engine/types";

const backendLabelKey = {
  cpu: "backend.cpu",
  metal: "backend.metal",
  cuda: "backend.cuda",
  vulkan: "backend.vulkan",
} as const;

export function BackendStatusList({
  rows,
  t,
}: {
  rows: BackendDisplayRow[];
  t: Translator;
}) {
  return (
    <div className="backend-list">
      {rows.map((row) => {
        const displayState = backendDisplayState(row);
        const ready = displayState === "ready";
        const partial = displayState === "partial";
        const status = !row.reported
          ? t("backend.reserved")
          : !row.compiled
            ? t("backend.notCompiled")
            : !row.deviceAvailable
              ? t("backend.noDevice")
              : row.analysisCommandAvailable
                ? t("backend.commandReady")
                : t("backend.partial");

        const detail = !row.reported
          ? t("backend.unreported")
          : [
              `${t("status.compiled")}: ${row.compiled ? t("common.yes") : t("common.no")}`,
              `${t("status.device")}: ${row.deviceAvailable ? t("common.yes") : t("common.no")}`,
              `${t("status.command")}: ${row.analysisCommandAvailable ? t("common.yes") : t("common.no")}`,
              row.axes.length ? row.axes.map((axis) => axisLabel(axis, t)).join(" / ") : null,
              // ISA and math mode are engine code values, not localized copy.
              row.selectedIsa
                ? `${row.selectedIsa} / ${row.selectedMathMode ?? "production"}`
                : row.selectedMathMode,
            ]
              .filter(Boolean)
              .join(" · ");

        return (
          <div className="backend-row" key={row.id}>
            <div className={`backend-icon ${ready ? "ready" : partial ? "partial" : "unavailable"}`}>
              {ready ? <Check size={15} /> : partial ? <TriangleAlert size={15} /> : <X size={15} />}
            </div>
            <div>
              <strong>{t(backendLabelKey[row.id])}</strong>
              <span>{status}</span>
              <small>{detail}</small>
            </div>
          </div>
        );
      })}
    </div>
  );
}

function axisLabel(axis: string, t: Translator): string {
  if (axis === "horizontal") return t("diagnostics.axis.horizontal");
  if (axis === "vertical") return t("diagnostics.axis.vertical");
  if (axis === "both" || axis === "two_axis" || axis === "two-axis") {
    return t("diagnostics.axis.twoAxis");
  }
  return axis;
}

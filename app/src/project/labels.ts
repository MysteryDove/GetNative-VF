import type { Translator } from "../i18n";

export function runTypeLabel(value: string, t: Translator): string {
  if (value === "height" || value === "height_analysis") return t("overview.run.resolution");
  if (value === "kernel" || value === "kernel_analysis") return t("overview.run.algorithm");
  if (value === "verification" || value === "verify") return t("overview.run.check");
  return t("overview.run.unknown");
}

export function runStatusLabel(value: string, t: Translator): string {
  if (value === "queued") return t("overview.runStatus.queued");
  if (value === "running") return t("overview.runStatus.running");
  if (value === "completed") return t("overview.runStatus.completed");
  if (value === "failed") return t("overview.runStatus.failed");
  if (value === "cancelled") return t("overview.runStatus.cancelled");
  if (value === "partial") return t("overview.runStatus.partial");
  return t("overview.runStatus.unknown");
}

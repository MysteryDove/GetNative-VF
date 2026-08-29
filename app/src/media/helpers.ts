/** Pure presentation helpers for the Media page (labels, summaries, keys). */

import type { Translator } from "../i18n";
import type { ProjectState, Source } from "../project/types";
import { fileName } from "./importSources";
import { dimensionText } from "./format";
import type { MediaCapabilities } from "./service";

/** Sample label for a picked video frame: "Source · Video stream N #frame". */
export function videoSampleLabel(
  source: Source,
  frameIndex: number | undefined,
  t: Translator,
): string {
  const sourceLabel = source.label ?? fileName(source.path);
  const streamLabel =
    source.videoStreams.length > 1
      ? ` · ${t("media.videoStream")} ${source.selectedStreamIndex ?? "-"}`
      : "";
  return `${sourceLabel}${streamLabel} #${frameIndex ?? "-"}`;
}

/** Identity key for the (source, fingerprint, stream) a preview belongs to. */
export function sourceStreamKey(source: Source): string {
  return [source.id, source.fingerprint ?? "", source.selectedStreamIndex ?? ""].join(":");
}

export function isFingerprintError(detail: string): boolean {
  return detail.includes("media_fingerprint_mismatch")
    || detail.includes("media_fingerprint_error");
}

export function mediaViewState(state: ProjectState): { selectedSourceId?: string } {
  const value = state.uiStateByRoute.media;
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as { selectedSourceId?: string })
    : {};
}

export function sourceSummary(source: Source, t: Translator): string {
  if (source.state === "probing") return t("media.state.probing");
  if (source.state === "missing") return t("media.state.missing");
  if (source.state === "unsupported") return t("media.state.unsupported");
  if (source.state === "error") return t("media.state.error");
  return dimensionText(source.width, source.height);
}

export function previewMessage(
  source: Source,
  capabilities: MediaCapabilities | null,
  t: Translator,
): string {
  if (source.state === "probing") return t("media.state.probing");
  if (source.state === "missing") return t("media.state.missing");
  if (source.state === "unsupported") return source.errorDetail ?? t("media.state.unsupported");
  if (source.kind === "animated") return t("media.animatedNotice");
  if (source.kind === "video" && !capabilities?.video_decode_available) {
    return t("media.videoBackendRequired");
  }
  return source.errorDetail ?? t("media.previewUnavailable");
}

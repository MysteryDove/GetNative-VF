import type { ProjectState } from "./types";

/** Keep the algorithm prefix out of compact source-filter labels. */
function shortFingerprint(fingerprint: string): string {
  const value = fingerprint.trim();
  const separator = value.indexOf(":");
  const digest = separator >= 0 ? value.slice(separator + 1) : value;
  return (digest || value).slice(0, 6);
}

export function sourceFilterLabel(sourceId: string, state: ProjectState): string {
  const source = state.sourcesById[sourceId];
  if (!source) return sourceId;
  const pathLabel = source.path.split(/[\\/]/).pop() || source.path;
  const base = source.label?.trim() || pathLabel || source.id;
  const fingerprint = source.fingerprint?.trim();
  return fingerprint ? `${base} (#${shortFingerprint(fingerprint)})` : base;
}

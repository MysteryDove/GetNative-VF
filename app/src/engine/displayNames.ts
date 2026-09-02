import type { MessageKey, Translator } from "../i18n";

const KERNEL_NAME_KEYS: Record<string, MessageKey> = {
  bilinear: "diagnostics.kernelName.bilinear",
  bicubic: "diagnostics.kernelName.bicubic",
  lanczos: "diagnostics.kernelName.lanczos",
  spline16: "diagnostics.kernelName.spline16",
  spline36: "diagnostics.kernelName.spline36",
  spline64: "diagnostics.kernelName.spline64",
};

export function kernelDisplayName(t: Translator, id: string): string {
  const key = KERNEL_NAME_KEYS[id];
  return key ? t(key) : id;
}

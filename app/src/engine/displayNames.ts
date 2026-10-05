import type { MessageKey, Translator } from "../i18n";
import { formatParameterValue } from "./numberInput";

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

/** `b=1/3, c=1/3` — parameters in stored order; a default blur of 1 is omitted. */
const KERNEL_PARAMETER_KEYS = new Set(["b", "c", "taps", "blur"]);

export function kernelParametersText(
  parameters: Record<string, unknown> | undefined,
  separator = ", ",
): string {
  return Object.entries(parameters ?? {})
    // Runs recorded by earlier builds can carry capability descriptors
    // (`kind`, `finite`, limits) next to the real kernel parameters.
    .filter(([key]) => KERNEL_PARAMETER_KEYS.has(key))
    .filter(([key, value]) => !(key === "blur" && Number(value) === 1))
    .map(([key, value]) => `${key}=${formatParameterValue(value as string | number | boolean)}`)
    .join(separator);
}

/** Full kernel identity for legends and tables, e.g. `Bicubic (b=1/3, c=1/3)`. */
export function kernelRefLabel(
  t: Translator,
  kernel: { id: string; parameters?: Record<string, unknown> },
): string {
  const name = kernelDisplayName(t, kernel.id);
  const text = kernelParametersText(kernel.parameters);
  return text ? `${name} (${text})` : name;
}

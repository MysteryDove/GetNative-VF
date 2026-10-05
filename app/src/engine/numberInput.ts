/**
 * User-facing number entry and display for kernel parameters and candidate
 * values: accepts plain decimals and simple fractions ("1/3"), and renders
 * thirds back as fractions so Bicubic (1/3, 1/3) stays readable.
 */

const DECIMAL = /^-?\d+(\.\d+)?$/;
const FRACTION = /^(-?\d+)\s*\/\s*(\d+)$/;

export type Rational = { numerator: bigint; denominator: bigint };

/** Exact rational for a decimal or `p/q` input; null when malformed. */
export function parseRational(text: string): Rational | null {
  const trimmed = text.trim();
  const fraction = FRACTION.exec(trimmed);
  if (fraction) {
    const denominator = BigInt(fraction[2]);
    if (denominator === 0n) return null;
    return { numerator: BigInt(fraction[1]), denominator };
  }
  if (!DECIMAL.test(trimmed)) return null;
  const negative = trimmed.startsWith("-");
  const [whole, decimals = ""] = (negative ? trimmed.slice(1) : trimmed).split(".");
  const numerator = BigInt(`${whole}${decimals}`) * (negative ? -1n : 1n);
  return { numerator, denominator: 10n ** BigInt(decimals.length) };
}

export function rationalToNumber(value: Rational): number {
  return Number(value.numerator) / Number(value.denominator);
}

/** Decimal or `p/q` input as a finite number; null when malformed. */
export function parseNumberInput(text: string | number | boolean | undefined): number | null {
  if (typeof text === "number") return Number.isFinite(text) ? text : null;
  if (typeof text !== "string") return null;
  const rational = parseRational(text);
  if (!rational) return null;
  const value = rationalToNumber(rational);
  return Number.isFinite(value) ? value : null;
}

/** True when the input is a `p/q` fraction rather than a plain decimal. */
export function isFractionInput(text: string): boolean {
  return FRACTION.test(text.trim());
}

/** Render a parameter value: non-integer thirds as `k/3`, else at most four decimals. */
export function formatParameterValue(value: string | number | boolean): string {
  if (typeof value !== "number" || !Number.isFinite(value) || Number.isInteger(value)) {
    return String(value);
  }
  const thirds = Math.round(value * 3);
  if (Math.abs(value * 3 - thirds) < 1e-9) return `${thirds}/3`;
  // Four places keep labels readable for irrational presets (Robidoux).
  return String(Number(value.toFixed(4)));
}

/** Digits after the decimal point in a decimal string ("0.05" → 2). */
export function decimalPlaces(text: string): number {
  const trimmed = text.trim();
  const dot = trimmed.indexOf(".");
  return dot < 0 ? 0 : trimmed.length - dot - 1;
}

/** Pad a decimal candidate with trailing zeros to `places` ("843.7" → "843.70"). */
export function padDecimals(value: string, places: number): string {
  if (places <= 0 || !DECIMAL.test(value.trim())) return value;
  const current = decimalPlaces(value);
  if (current >= places) return value;
  return `${value.trim()}${current === 0 ? "." : ""}${"0".repeat(places - current)}`;
}

/** True when a decimal string has a non-zero fractional part. */
export function hasFractionalPart(text: string): boolean {
  return /\.\d*[1-9]/.test(text);
}

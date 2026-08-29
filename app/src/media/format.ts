/** Shared display formatting for media dimensions and timestamps. */

export function formatSeconds(value: number | null | undefined): string {
  return value == null ? "-" : `${value.toFixed(3)} s`;
}

export function dimensionText(width?: number | null, height?: number | null): string {
  return width && height ? `${width} x ${height}` : "-";
}

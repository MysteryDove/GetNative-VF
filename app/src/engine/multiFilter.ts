/**
 * Multi-select result filters (Runs, Source). The selection is the list of
 * chosen option values; an empty list means "All".
 */
export type FilterSelection = readonly string[];

export const NO_FILTER: FilterSelection = [];

/** True when `value` passes the filter (nothing selected accepts everything). */
export function filterAccepts(selected: FilterSelection, value: string): boolean {
  return selected.length === 0 || selected.includes(value);
}

/** Add or remove one value, keeping selection order. */
export function toggleFilterValue(selected: FilterSelection, value: string): string[] {
  return selected.includes(value)
    ? selected.filter((entry) => entry !== value)
    : [...selected, value];
}

/**
 * Drop values that no longer exist (deleted Runs, removed Sources). Returns
 * the same array when nothing changed so it is safe to use in a state setter.
 */
export function pruneFilter(selected: FilterSelection, available: Iterable<string>): FilterSelection {
  if (selected.length === 0) return selected;
  const known = available instanceof Set ? available : new Set(available);
  const kept = selected.filter((value) => known.has(value));
  return kept.length === selected.length ? selected : kept;
}

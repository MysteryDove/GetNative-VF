/**
 * Returns a NEW Set with `value` toggled: removed when present, added when
 * absent. The input Set is left untouched (safe for React state updates).
 */
export function toggleSetValue<T>(set: Set<T>, value: T): Set<T> {
  const next = new Set(set);
  if (next.has(value)) next.delete(value);
  else next.add(value);
  return next;
}

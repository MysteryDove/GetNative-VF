/**
 * Pre-paint theme bootstrap. Loaded as an early module from index.html so
 * data-theme is set before the app stylesheet applies — CSP forbids inline
 * scripts, so this lives in its own file. Keep the resolution logic in sync
 * with src/utils/theme.ts (mode mirror key + system fallback).
 */
(() => {
  try {
    const mode = localStorage.getItem("gnvf-theme");
    const dark =
      mode === "dark" ||
      ((mode === null || mode === "system") &&
        window.matchMedia("(prefers-color-scheme: dark)").matches);
    document.documentElement.dataset.theme = dark ? "dark" : "light";
  } catch {
    // Storage/matchMedia failures fall back to the default dark :root theme.
  }
})();

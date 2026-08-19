/**
 * Theme selection and application.
 *
 * The mode ("system" | "light" | "dark") is persisted in the Rust-side
 * preferences.json via the app_set_theme command, and mirrored to
 * localStorage so src/themeBoot.ts can apply it before first paint
 * (the CSP forbids inline bootstrap scripts).
 */
export type ThemeMode = "system" | "light" | "dark";
export type ResolvedTheme = "light" | "dark";

const STORAGE_KEY = "gnvf-theme";
const TRANSITION_MS = 220;

/** Window background colors matching --surface in App.css. */
const NATIVE_BG: Record<ResolvedTheme, [number, number, number]> = {
  dark: [21, 24, 23],
  light: [245, 246, 245],
};

const THEME_COLOR: Record<ResolvedTheme, string> = {
  dark: "#151817",
  light: "#f5f6f5",
};

export function isThemeMode(value: unknown): value is ThemeMode {
  return value === "system" || value === "light" || value === "dark";
}

export function systemTheme(): ResolvedTheme {
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

export function resolveTheme(mode: ThemeMode, system: ResolvedTheme): ResolvedTheme {
  return mode === "system" ? system : mode;
}

/** Mirror the mode into localStorage for the pre-paint bootstrap script. */
export function storeThemeMode(mode: ThemeMode): void {
  try {
    localStorage.setItem(STORAGE_KEY, mode);
  } catch {
    // Private-mode storage failures only cost the flash-free boot.
  }
}

let transitionTimer: ReturnType<typeof setTimeout> | null = null;

/**
 * Apply the resolved theme to the document and the native window chrome.
 * Pass animate=true to cross-fade the color change (see .theme-transition
 * in App.css); the initial application at boot should not animate.
 */
export function applyTheme(resolved: ResolvedTheme, animate: boolean): void {
  const root = document.documentElement;
  if (animate) {
    root.classList.add("theme-transition");
    if (transitionTimer) clearTimeout(transitionTimer);
  }
  root.dataset.theme = resolved;
  document
    .querySelector('meta[name="theme-color"]')
    ?.setAttribute("content", THEME_COLOR[resolved]);
  if (animate) {
    transitionTimer = setTimeout(() => {
      root.classList.remove("theme-transition");
      transitionTimer = null;
    }, TRANSITION_MS);
  }
  void syncNativeTheme(resolved);
}

/** Best-effort sync of the native title bar and WebView background. */
async function syncNativeTheme(resolved: ResolvedTheme): Promise<void> {
  if (!("__TAURI_INTERNALS__" in window)) return;
  try {
    const { getCurrentWindow } = await import("@tauri-apps/api/window");
    const win = getCurrentWindow();
    await win.setTheme(resolved);
    await win.setBackgroundColor(NATIVE_BG[resolved]);
  } catch {
    // Native chrome sync is cosmetic; the in-app theme already applied.
  }
}

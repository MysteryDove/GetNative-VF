import { useEffect, useState, type ReactNode } from "react";
import { Check, FolderOpen, RotateCcw } from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import type { Translator } from "../i18n";
import type { ThemeMode } from "../utils/theme";

export function SettingsPage({
  t,
  language,
  onLanguageChange,
  themeMode,
  onThemeChange,
  axisPlanCacheDir,
  onAxisPlanCacheDirChange,
}: {
  t: Translator;
  language: "zh-CN" | "en";
  onLanguageChange: (locale: "zh-CN" | "en") => void;
  themeMode: ThemeMode;
  onThemeChange: (mode: ThemeMode) => void;
  axisPlanCacheDir: string | null;
  onAxisPlanCacheDirChange: (path: string | null) => Promise<void>;
}) {
  const [cachePath, setCachePath] = useState(axisPlanCacheDir ?? "");
  const [cacheBusy, setCacheBusy] = useState(false);

  useEffect(() => setCachePath(axisPlanCacheDir ?? ""), [axisPlanCacheDir]);

  async function chooseCachePath() {
    try {
      const path = await invoke<string | null>("app_pick_axis_plan_cache_dir");
      if (path) setCachePath(path);
    } catch {
      // The path can still be entered manually if the native chooser fails.
    }
  }

  async function saveCachePath() {
    setCacheBusy(true);
    try {
      await onAxisPlanCacheDirChange(cachePath.trim() || null);
    } finally {
      setCacheBusy(false);
    }
  }

  return (
    <PageFrame title={t("settings.title")}>
      <section className="page-section">
        <label className="language-field block">
          <span>{t("language.label")}</span>
          <select
            value={language}
            onChange={(event) => onLanguageChange(event.target.value as "zh-CN" | "en")}
          >
            <option value="zh-CN">{t("language.zhCN")}</option>
            <option value="en">{t("language.en")}</option>
          </select>
        </label>
        <p className="help-copy">{t("settings.languageHelp")}</p>
      </section>
      <section className="page-section">
        <label className="language-field block">
          <span>{t("settings.themeLabel")}</span>
          <select
            value={themeMode}
            onChange={(event) => onThemeChange(event.target.value as ThemeMode)}
          >
            <option value="system">{t("settings.themeSystem")}</option>
            <option value="light">{t("settings.themeLight")}</option>
            <option value="dark">{t("settings.themeDark")}</option>
          </select>
        </label>
        <p className="help-copy">{t("settings.themeHelp")}</p>
      </section>
      <section className="page-section">
        <h3>{t("settings.cacheTitle")}</h3>
        <label className="cache-path-field">
          <span>{t("settings.cachePath")}</span>
          <div className="cache-path-row">
            <input
              value={cachePath}
              onChange={(event) => setCachePath(event.target.value)}
              placeholder={t("settings.cachePathPlaceholder")}
              spellCheck={false}
              disabled={cacheBusy}
            />
            <button
              className="secondary-button"
              type="button"
              onClick={() => void chooseCachePath()}
              disabled={cacheBusy}
              title={t("settings.cacheBrowse")}
            >
              <FolderOpen size={15} />
              {t("settings.cacheBrowse")}
            </button>
          </div>
        </label>
        <div className="cache-path-actions">
          <button
            className="secondary-button"
            type="button"
            onClick={() => void saveCachePath()}
            disabled={cacheBusy}
          >
            <Check size={15} />
            {t("settings.cacheSave")}
          </button>
          <button
            className="icon-button"
            type="button"
            onClick={() => setCachePath("")}
            disabled={cacheBusy || cachePath.length === 0}
            title={t("settings.cacheReset")}
            aria-label={t("settings.cacheReset")}
          >
            <RotateCcw size={15} />
          </button>
        </div>
        <p className="help-copy">{t("settings.cacheHelp")}</p>
        <p className="help-copy">{t("settings.persistenceHelp")}</p>
      </section>
    </PageFrame>
  );
}

function PageFrame({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="page-panel">
      <div className="page-header">
        <h2>{title}</h2>
      </div>
      {children}
    </div>
  );
}

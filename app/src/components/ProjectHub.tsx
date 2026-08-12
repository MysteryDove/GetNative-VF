import { useState } from "react";
import { Activity, FolderOpen, FolderSearch, LoaderCircle, Plus, X, Zap } from "lucide-react";
import type { Translator } from "../i18n";
import type { ManifestErrorDto, RecentProjectEntry, RecoveryInfo } from "../project/types";
import { ErrorNotice, type UiError } from "./ErrorNotice";

export function ProjectHub({
  t,
  recent,
  recovery,
  busy,
  error,
  onNew,
  onOpen,
  onQuick,
  onOpenRecent,
  onRevealRecent,
  onRemoveRecent,
  onRecover,
  onDiscardRecovery,
  onLanguageChange,
  locale,
}: {
  t: Translator;
  recent: RecentProjectEntry[];
  recovery: RecoveryInfo;
  busy: boolean;
  error: UiError | null;
  onNew: (name: string) => void;
  onOpen: () => void;
  onQuick: () => void;
  onOpenRecent: (path: string) => void;
  onRevealRecent: (path: string) => void;
  onRemoveRecent: (path: string) => void;
  onRecover: () => void;
  onDiscardRecovery: () => void;
  onLanguageChange: (locale: "zh-CN" | "en") => void;
  locale: "zh-CN" | "en";
}) {
  const [naming, setNaming] = useState(false);
  const [projectName, setProjectName] = useState("");

  function submitNewProject(event: React.FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const name = projectName.trim();
    if (!name) return;
    setNaming(false);
    setProjectName("");
    onNew(name);
  }

  return (
    <div className="hub-shell">
      <header className="topbar hub-topbar">
        <div className="brand-block">
          <div className="brand-mark" aria-hidden="true">
            <Activity size={19} strokeWidth={2.2} />
          </div>
          <div>
            <h1>{t("app.name")}</h1>
            <span>{t("app.tagline")}</span>
          </div>
        </div>
        <div className="top-actions">
          <label className="language-field">
            <span>{t("language.label")}</span>
            <select
              value={locale}
              onChange={(event) => onLanguageChange(event.target.value as "zh-CN" | "en")}
            >
              <option value="zh-CN">{t("language.zhCN")}</option>
              <option value="en">{t("language.en")}</option>
            </select>
          </label>
        </div>
      </header>

      <main className="hub-main">
        <section className="hub-panel">
          <div className="hub-heading">
            <div>
              <h2>{t("hub.title")}</h2>
              <span>{t("hub.subtitle")}</span>
            </div>
          </div>

          <div className="hub-actions">
            <button className="run-button" onClick={() => setNaming(true)} disabled={busy}>
              {busy ? <LoaderCircle className="spin" size={16} /> : <Plus size={16} />}
              {t("hub.newProject")}
            </button>
            <button className="secondary-button" onClick={onOpen} disabled={busy}>
              <FolderOpen size={16} />
              {t("hub.openProject")}
            </button>
            <button className="secondary-button" onClick={onQuick} disabled={busy}>
              <Zap size={16} />
              {t("hub.quickAnalysis")}
            </button>
          </div>

          {error ? <ErrorNotice error={error} t={t} /> : null}

          {recovery.present && (
            <div className="recovery-row">
              <div>
                <strong>{t("hub.recoveryTitle")}</strong>
                <span>
                  {recovery.name ?? t("shell.untitled")}
                  {recovery.updated_at ? ` · ${formatTime(recovery.updated_at, locale)}` : ""}
                </span>
              </div>
              <div className="recovery-actions">
                <button className="secondary-button" onClick={onDiscardRecovery} disabled={busy}>
                  {t("hub.recoveryDismiss")}
                </button>
                <button className="secondary-button" onClick={onRecover} disabled={busy}>
                  {t("hub.recoveryAction")}
                </button>
              </div>
            </div>
          )}

          <div className="hub-section-title">
            <Activity size={15} />
            <h3>{t("hub.recentTitle")}</h3>
          </div>

          {recent.length === 0 ? (
            <p className="empty-copy">{t("hub.recentEmpty")}</p>
          ) : (
            <div className="recent-table" role="table" aria-label={t("hub.recentTitle")}>
              <div className="recent-head" role="row">
                <span role="columnheader">{t("hub.column.name")}</span>
                <span role="columnheader">{t("hub.column.opened")}</span>
                <span role="columnheader">{t("hub.column.sources")}</span>
                <span role="columnheader">{t("hub.column.recipe")}</span>
                <span role="columnheader">{t("hub.column.media")}</span>
                <span role="columnheader">{t("hub.column.actions")}</span>
              </div>
              {recent.map((entry) => (
                <div className="recent-row" role="row" key={entry.path}>
                  <span role="cell" title={entry.path}>
                    <strong>{entry.name}</strong>
                    <small>{entry.path}</small>
                  </span>
                  <span role="cell">{formatTime(entry.last_opened_at, locale)}</span>
                  <span role="cell">{entry.source_count}</span>
                  <span role="cell">{entry.active_recipe_name ?? t("hub.noActiveRecipe")}</span>
                  <span role="cell">
                    {entry.has_missing_media ? t("hub.missingMedia") : t("hub.mediaOk")}
                  </span>
                  <span className="recent-actions" role="cell">
                    <button className="link-button" onClick={() => onOpenRecent(entry.path)} disabled={busy}>
                      {t("hub.open")}
                    </button>
                    <button
                      className="icon-button reveal-button"
                      title={t("hub.revealProject")}
                      aria-label={t("hub.revealProject")}
                      onClick={() => onRevealRecent(entry.path)}
                      disabled={busy}
                    >
                      <FolderSearch size={14} />
                    </button>
                    <button
                      className="icon-button"
                      title={t("hub.removeRecent")}
                      aria-label={t("hub.removeRecent")}
                      onClick={() => onRemoveRecent(entry.path)}
                      disabled={busy}
                    >
                      <X size={14} />
                    </button>
                  </span>
                </div>
              ))}
            </div>
          )}
        </section>
      </main>

      {naming ? (
        <div className="modal-backdrop" role="presentation">
          <form className="naming-dialog" role="dialog" aria-modal="true" onSubmit={submitNewProject}>
            <div className="dialog-heading">
              <h2>{t("hub.newProjectTitle")}</h2>
              <button
                className="icon-button"
                type="button"
                title={t("common.close")}
                aria-label={t("common.close")}
                onClick={() => setNaming(false)}
              >
                <X size={15} />
              </button>
            </div>
            <label className="dialog-field">
              <span>{t("hub.projectName")}</span>
              <input
                autoFocus
                value={projectName}
                placeholder={t("hub.projectNamePlaceholder")}
                onChange={(event) => setProjectName(event.target.value)}
              />
            </label>
            <div className="dialog-actions">
              <button className="secondary-button" type="button" onClick={() => setNaming(false)}>
                {t("common.cancel")}
              </button>
              <button className="run-button" type="submit" disabled={!projectName.trim()}>
                <Plus size={15} />
                {t("hub.createProject")}
              </button>
            </div>
          </form>
        </div>
      ) : null}
    </div>
  );
}

export function formatProjectError(error: ManifestErrorDto | null | undefined, t: Translator): string {
  if (!error) return t("hub.error.generic");
  if (error.code === "cancelled") return t("hub.error.cancelled");
  if (error.code === "unsupported_schema") return t("hub.error.unsupportedSchema");
  if (error.code === "missing_name") return t("hub.error.missingName");
  if (error.code === "project_mismatch") return t("hub.error.projectMismatch");
  if (error.code === "invalid_json" || error.code === "invalid_manifest") {
    return t("hub.error.invalid");
  }
  return t("hub.error.generic");
}

function formatTime(value: string, locale: "zh-CN" | "en"): string {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleString(locale);
}

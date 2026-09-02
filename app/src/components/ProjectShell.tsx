import { Check, X } from "lucide-react";
import { BrandMark } from "./BrandMark";
import { memo, useCallback, useEffect, useState } from "react";
import type { Translator } from "../i18n";
import type { EngineEnvelope, EngineState } from "../engine/types";
import type { ProjectRoute, ProjectState } from "../project/types";
import { countById } from "../project/normalize";
import { ProjectNavigator } from "./ProjectNavigator";
import { OverviewPage } from "../pages/OverviewPage";
import { SettingsPage } from "../pages/SettingsPage";
import { ResultsPage } from "../pages/ResultsPage";
import { VerifyPage } from "../pages/VerifyPage";
import { AnalyzePage } from "../pages/AnalyzePage";
import { DiagnosticsPage } from "../pages/DiagnosticsPage";
import { MediaPage } from "../pages/MediaPage";
import { ErrorNotice, type UiError } from "./ErrorNotice";
import { runGroupProgress, type ExecutionState } from "../engine/runReducer";
import type { ExecutionBridge } from "../engine/executeRunGroup";
import type { ThemeMode } from "../utils/theme";
import { actualBackendLabel } from "../engine/backendSelection";

const Overview = memo(OverviewPage);
const Media = memo(MediaPage);
const Analyze = memo(AnalyzePage);
const Verify = memo(VerifyPage);
const Results = memo(ResultsPage);
const Settings = memo(SettingsPage);
const Diagnostics = memo(DiagnosticsPage);

function pageClass(id: ProjectRoute, route: ProjectRoute, visited: ReadonlySet<ProjectRoute>): string {
  return `project-page${route === id ? " is-active" : ""}${visited.has(id) ? " was-visited" : ""}`;
}

function formatRate(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) return "—";
  return value >= 100 ? String(Math.round(value)) : value.toFixed(1);
}

function rateUnit(t: Translator, unit: "candidates" | "frames"): string {
  return unit === "frames" ? t("jobs.framesPerSecond") : t("jobs.candidatesPerSecond");
}

type ProjectPageHostProps = {
  t: Translator;
  state: ProjectState;
  route: ProjectRoute;
  engineState: EngineState;
  enginePath: string;
  engineError: string;
  capabilities: EngineEnvelope | null;
  language: "zh-CN" | "en";
  onLanguageChange: (locale: "zh-CN" | "en") => void;
  themeMode: ThemeMode;
  onThemeChange: (mode: ThemeMode) => void;
  axisPlanCacheDir: string | null;
  onAxisPlanCacheDirChange: (path: string | null) => Promise<void>;
  onNavigate: (route: ProjectRoute) => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  onEngineError: (message: string) => void;
  onGeometrySuccess: () => void;
  executionBridge: ExecutionBridge;
  analyzeAvailable: boolean;
  analyzeSubroute: "height" | "kernel";
  openDiagnostics: () => void;
  openMedia: () => void;
};

/** Isolated from nav collapse so toggling the sidebar does not re-render keep-alive pages. */
const ProjectPageHost = memo(function ProjectPageHost({
  t,
  state,
  route,
  engineState,
  enginePath,
  engineError,
  capabilities,
  language,
  onLanguageChange,
  themeMode,
  onThemeChange,
  axisPlanCacheDir,
  onAxisPlanCacheDirChange,
  onNavigate,
  onProjectChange,
  onEngineError,
  onGeometrySuccess,
  executionBridge,
  analyzeAvailable,
  analyzeSubroute,
  openDiagnostics,
  openMedia,
}: ProjectPageHostProps) {
  const [visited, setVisited] = useState<ReadonlySet<ProjectRoute>>(() => new Set([route]));
  useEffect(() => {
    setVisited((current) => {
      if (current.has(route)) return current;
      const next = new Set(current);
      next.add(route);
      return next;
    });
  }, [route]);
  return (
    <div className="project-page-host">
      {/* Keep every page mounted so route changes preserve local selections and drafts. */}
      <div className={pageClass("overview", route, visited)} aria-hidden={route !== "overview"}>
        <Overview
          t={t}
          state={state}
          analyzeAvailable={analyzeAvailable}
          onNavigate={onNavigate}
          onProjectChange={onProjectChange}
        />
      </div>
      <div className={pageClass("media", route, visited)} aria-hidden={route !== "media"}>
        <Media
          t={t}
          state={state}
          active={route === "media"}
          onProjectChange={onProjectChange}
        />
      </div>
      <div className={pageClass("analyze", route, visited)} aria-hidden={route !== "analyze"}>
        <Analyze
          t={t}
          state={state}
          capabilities={capabilities}
          analyzeAvailable={analyzeAvailable}
          subroute={analyzeSubroute}
          onOpenDiagnostics={openDiagnostics}
          onOpenMedia={openMedia}
          onProjectChange={onProjectChange}
          executionBridge={executionBridge}
        />
      </div>
      <div className={pageClass("verify", route, visited)} aria-hidden={route !== "verify"}>
        <Verify
          t={t}
          state={state}
          capabilities={capabilities}
          analyzeAvailable={analyzeAvailable}
          onNavigate={onNavigate}
          onProjectChange={onProjectChange}
          executionBridge={executionBridge}
        />
      </div>
      <div className={pageClass("results", route, visited)} aria-hidden={route !== "results"}>
        <Results t={t} state={state} onProjectChange={onProjectChange} />
      </div>
      <div className={pageClass("settings", route, visited)} aria-hidden={route !== "settings"}>
        <Settings
          t={t}
          language={language}
          onLanguageChange={onLanguageChange}
          themeMode={themeMode}
          onThemeChange={onThemeChange}
          axisPlanCacheDir={axisPlanCacheDir}
          onAxisPlanCacheDirChange={onAxisPlanCacheDirChange}
        />
      </div>
      <div className={pageClass("diagnostics", route, visited)} aria-hidden={route !== "diagnostics"}>
        <Diagnostics
          t={t}
          engineState={engineState}
          enginePath={enginePath}
          engineError={engineError}
          capabilities={capabilities}
          onEngineError={onEngineError}
          onGeometrySuccess={onGeometrySuccess}
        />
      </div>
    </div>
  );
});

export function ProjectShell({
  t,
  state,
  route,
  engineState,
  enginePath,
  engineError,
  projectError,
  capabilities,
  language,
  onLanguageChange,
  themeMode,
  onThemeChange,
  axisPlanCacheDir,
  onAxisPlanCacheDirChange,
  onNavigate,
  onClose,
  onSave,
  onProjectChange,
  onEngineError,
  onGeometrySuccess,
  busy,
  execution,
  executionBridge,
}: {
  t: Translator;
  state: ProjectState;
  route: ProjectRoute;
  engineState: EngineState;
  enginePath: string;
  engineError: string;
  projectError: UiError | null;
  capabilities: EngineEnvelope | null;
  language: "zh-CN" | "en";
  onLanguageChange: (locale: "zh-CN" | "en") => void;
  themeMode: ThemeMode;
  onThemeChange: (mode: ThemeMode) => void;
  axisPlanCacheDir: string | null;
  onAxisPlanCacheDirChange: (path: string | null) => Promise<void>;
  onNavigate: (route: ProjectRoute) => void;
  onClose: () => void;
  onSave: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  onEngineError: (message: string) => void;
  onGeometrySuccess: () => void;
  busy: boolean;
  execution: ExecutionState;
  executionBridge: ExecutionBridge;
}) {
  const analyzeAvailable = capabilities?.payload.commands.analyze ?? false;
  const [navCollapsed, setNavCollapsed] = useState(
    () => localStorage.getItem("getnative.navCollapsed") === "1",
  );
  // Analyze subroute lives in the shell: the nav sidebar switches between the
  // resolution test and the algorithm test.
  const [analyzeSubroute, setAnalyzeSubroute] = useState<"height" | "kernel">("height");
  const navigateAnalyze = useCallback((subroute: "height" | "kernel") => {
    setAnalyzeSubroute(subroute);
    onNavigate("analyze");
  }, [onNavigate]);
  const toggleNav = useCallback(() => {
    setNavCollapsed((current) => {
      localStorage.setItem("getnative.navCollapsed", current ? "0" : "1");
      return !current;
    });
  }, []);
  const openSettings = useCallback(() => onNavigate("settings"), [onNavigate]);
  const openDiagnostics = useCallback(() => onNavigate("diagnostics"), [onNavigate]);
  const openMedia = useCallback(() => onNavigate("media"), [onNavigate]);
  const counts = {
    sources: countById(state.sourcesById),
    samples: countById(state.samplesById),
    runs: countById(state.runsById),
  };
  const jobGroups = runGroupProgress(execution, 4);
  return (
    <div className={`project-shell ${engineError && route === "diagnostics" ? "has-error" : ""}`}>
      <header className="topbar project-topbar">
        <div className="brand-block">
          <BrandMark />
          <div className="project-title-block">
            <h1 title={state.project.storagePath ?? undefined}>
              {state.project.untitled ? t("shell.untitled") : state.project.name}
            </h1>
            <span>
              {state.project.dirty || state.project.untitled ? t("shell.unsaved") : t("shell.saved")}
              {state.project.readOnly ? ` · ${t("shell.readOnly")}` : ""}
            </span>
          </div>
        </div>

        <div className="top-actions">
          <div
            className={`engine-state ${engineState}`}
            title={enginePath || engineError}
          >
            <span className="status-dot" />
            {engineState === "ready"
              ? t("engine.version", { version: capabilities?.payload.version ?? "" })
              : engineState === "checking"
                ? t("engine.checking")
                : t("engine.missing")}
          </div>
          <button className="secondary-button" onClick={onSave} disabled={busy || state.project.readOnly}>
            <Check size={15} />
            {t("shell.save")}
          </button>
          <button className="secondary-button" onClick={onClose} disabled={busy}>
            <X size={15} />
            {t("shell.closeProject")}
          </button>
        </div>
      </header>

      <div className={`project-body ${navCollapsed ? "nav-collapsed" : ""}`}>
        <ProjectNavigator
          t={t}
          route={route}
          counts={counts}
          collapsed={navCollapsed}
          onNavigate={onNavigate}
          onToggleCollapse={toggleNav}
          onSettings={openSettings}
          onDiagnostics={openDiagnostics}
          analyzeSubroute={analyzeSubroute}
          onAnalyzeSubroute={navigateAnalyze}
        />

        <div className="project-content">
          <div className={`project-page-stack ${projectError ? "has-notice" : ""}`}>
            {projectError ? <ErrorNotice error={projectError} t={t} /> : null}
            <ProjectPageHost
              t={t}
              state={state}
              route={route}
              engineState={engineState}
              enginePath={enginePath}
              engineError={engineError}
              capabilities={capabilities}
              language={language}
              onLanguageChange={onLanguageChange}
              themeMode={themeMode}
              onThemeChange={onThemeChange}
              axisPlanCacheDir={axisPlanCacheDir}
              onAxisPlanCacheDirChange={onAxisPlanCacheDirChange}
              onNavigate={onNavigate}
              onProjectChange={onProjectChange}
              onEngineError={onEngineError}
              onGeometrySuccess={onGeometrySuccess}
              executionBridge={executionBridge}
              analyzeAvailable={analyzeAvailable}
              analyzeSubroute={analyzeSubroute}
              openDiagnostics={openDiagnostics}
              openMedia={openMedia}
            />
          </div>

          <div className="jobs-tray" aria-label={t("jobs.title")}>
            <strong>{t("jobs.title")}</strong>
            {jobGroups.length === 0 ? (
              <span>{t("jobs.empty")}</span>
            ) : (
              <ul className="jobs-tray-list">
                {jobGroups.map((group) => (
                  <li key={group.id}>
                    <span className={`job-phase ${group.phase}`}>{t(`jobs.phase.${group.phase}`)}</span>
                    <span className="job-label">
                      {state.runGroupsById[group.id]?.label ?? group.id}
                    </span>
                    {group.backend ? (
                      <span
                        className="job-backend"
                        title={actualBackendLabel(t, group.backend, group.device)}
                      >
                        {actualBackendLabel(t, group.backend, group.device)}
                      </span>
                    ) : null}
                    {group.total > 0 ? (
                      <span className="job-progress-count">
                        {group.completed}/{group.total}
                      </span>
                    ) : null}
                    {group.total > 0 || group.phase === "queued" || group.phase === "running" ? (
                      <span
                        className={`job-progress-track ${group.total > 0 ? "determinate" : "indeterminate"}`}
                        role="progressbar"
                        aria-label={t("jobs.progress")}
                        aria-valuemin={0}
                        aria-valuemax={group.total > 0 ? group.total : undefined}
                        aria-valuenow={group.total > 0 ? Math.min(group.completed, group.total) : undefined}
                      >
                        <span
                          className="job-progress-fill"
                          style={
                            group.total > 0
                              ? { width: `${Math.min(100, Math.max(0, (group.completed / group.total) * 100))}%` }
                              : undefined
                          }
                        />
                      </span>
                    ) : null}
                    {group.phase === "running" && (group.fpsCurrent != null || group.fpsAvg != null) ? (
                      <span className="job-fps">
                        {formatRate(group.fpsCurrent ?? group.fpsAvg)} {rateUnit(t, group.rateUnit)}
                        {group.fpsCurrent != null && group.fpsAvg != null ? (
                          <> · {t("jobs.fpsAvg")} {formatRate(group.fpsAvg)}</>
                        ) : null}
                      </span>
                    ) : null}
                    {group.phase === "completed" && group.fpsAvg != null ? (
                      <span className="job-fps">
                        {t("jobs.fpsAvg")} {formatRate(group.fpsAvg)} {rateUnit(t, group.rateUnit)}
                      </span>
                    ) : null}
                    {group.activeJobIds.length > 0 ? (
                      <button
                        className="link-button job-cancel"
                        type="button"
                        disabled={group.cancelRequested}
                        onClick={() =>
                          group.activeJobIds.forEach((jobId) => executionBridge.cancel(jobId))
                        }
                      >
                        {group.cancelRequested ? t("jobs.cancelling") : t("jobs.cancel")}
                      </button>
                    ) : null}
                  </li>
                ))}
              </ul>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

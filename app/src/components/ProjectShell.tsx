import { Activity, Check, X } from "lucide-react";
import { useState } from "react";
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
import { SamplesPage } from "../pages/SamplesPage";
import { ErrorNotice, type UiError } from "./ErrorNotice";
import { runGroupProgress, type ExecutionState } from "../engine/runReducer";
import type { ExecutionBridge } from "../engine/executeRunGroup";
import { actualBackendLabel } from "../engine/backendSelection";

function formatRate(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) return "—";
  return value >= 100 ? String(Math.round(value)) : value.toFixed(1);
}

function rateUnit(t: Translator, unit: "candidates" | "frames"): string {
  return unit === "frames" ? t("jobs.framesPerSecond") : t("jobs.candidatesPerSecond");
}

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
  const toggleNav = () => {
    setNavCollapsed((current) => {
      localStorage.setItem("getnative.navCollapsed", current ? "0" : "1");
      return !current;
    });
  };
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
          <div className="brand-mark" aria-hidden="true">
            <Activity size={19} strokeWidth={2.2} />
          </div>
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
          onSettings={() => onNavigate("settings")}
          onDiagnostics={() => onNavigate("diagnostics")}
        />

        <div className="project-content">
          <div className={`project-page-stack ${projectError ? "has-notice" : ""}`}>
            {projectError ? <ErrorNotice error={projectError} t={t} /> : null}
            <div className="project-page-host">
              {route === "overview" && (
                <OverviewPage
                  t={t}
                  state={state}
                  analyzeAvailable={analyzeAvailable}
                  onNavigate={onNavigate}
                  onProjectChange={onProjectChange}
                />
              )}
              {route === "media" && (
                <MediaPage t={t} state={state} onProjectChange={onProjectChange} />
              )}
              {route === "samples" && (
                <SamplesPage
                  t={t}
                  state={state}
                  onProjectChange={onProjectChange}
                  onStartHeightAnalysis={() => onNavigate("analyze")}
                />
              )}
              {route === "analyze" && (
                <AnalyzePage
                  t={t}
                  state={state}
                  capabilities={capabilities}
                  analyzeAvailable={analyzeAvailable}
                  onOpenDiagnostics={() => onNavigate("diagnostics")}
                  onOpenSamples={() => onNavigate("samples")}
                  onProjectChange={onProjectChange}
                  executionBridge={executionBridge}
                />
              )}
              {route === "verify" && (
                <VerifyPage
                  t={t}
                  state={state}
                  capabilities={capabilities}
                  analyzeAvailable={analyzeAvailable}
                  onNavigate={onNavigate}
                  onProjectChange={onProjectChange}
                  executionBridge={executionBridge}
                />
              )}
              {route === "results" && (
                <ResultsPage t={t} state={state} onProjectChange={onProjectChange} />
              )}
              {route === "settings" && (
                <SettingsPage
                  t={t}
                  language={language}
                  onLanguageChange={onLanguageChange}
                  axisPlanCacheDir={axisPlanCacheDir}
                  onAxisPlanCacheDirChange={onAxisPlanCacheDirChange}
                />
              )}
              {route === "diagnostics" && (
                <DiagnosticsPage
                  t={t}
                  engineState={engineState}
                  enginePath={enginePath}
                  engineError={engineError}
                  capabilities={capabilities}
                  onEngineError={onEngineError}
                  onGeometrySuccess={onGeometrySuccess}
                />
              )}
            </div>
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
                      <span className="job-progress">
                        {group.completed}/{group.total}
                      </span>
                    ) : null}
                    {group.phase === "running" && group.fpsCurrent != null ? (
                      <span className="job-fps">
                        {formatRate(group.fpsCurrent)} {rateUnit(t, group.rateUnit)} · {t("jobs.fpsAvg")} {formatRate(group.fpsAvg)}
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

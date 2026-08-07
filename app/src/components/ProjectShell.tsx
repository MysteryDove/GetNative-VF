import { Activity, Check, X } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope, EngineState } from "../engine/types";
import type { ProjectRoute, ProjectState } from "../project/types";
import { countById } from "../project/normalize";
import { ProjectNavigator } from "./ProjectNavigator";
import { OverviewPage } from "../pages/OverviewPage";
import { SettingsPage } from "../pages/BlockedPages";
import { ResultsPage } from "../pages/ResultsPage";
import { VerifyPage } from "../pages/VerifyPage";
import { AnalyzePage } from "../pages/AnalyzePage";
import { DiagnosticsPage } from "../pages/DiagnosticsPage";
import { MediaPage } from "../pages/MediaPage";
import { SamplesPage } from "../pages/SamplesPage";
import { ErrorNotice, type UiError } from "./ErrorNotice";
import { recentJobs, type ExecutionState } from "../engine/runReducer";

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
  onNavigate,
  onClose,
  onSave,
  onProjectChange,
  onEngineError,
  onGeometrySuccess,
  busy,
  execution,
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
  onNavigate: (route: ProjectRoute) => void;
  onClose: () => void;
  onSave: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  onEngineError: (message: string) => void;
  onGeometrySuccess: () => void;
  busy: boolean;
  execution: ExecutionState;
}) {
  const analyzeAvailable = capabilities?.payload.commands.analyze ?? false;
  const counts = {
    sources: countById(state.sourcesById),
    samples: countById(state.samplesById),
    runs: countById(state.runsById),
  };
  const jobs = recentJobs(execution, 6);

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

      <div className="project-body">
        <ProjectNavigator
          t={t}
          route={route}
          counts={counts}
          onNavigate={onNavigate}
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
                />
              )}
              {route === "verify" && (
                <VerifyPage
                  t={t}
                  state={state}
                  capabilities={capabilities}
                  analyzeAvailable={analyzeAvailable}
                  onNavigate={onNavigate}
                />
              )}
              {route === "results" && <ResultsPage t={t} state={state} />}
              {route === "settings" && (
                <SettingsPage t={t} language={language} onLanguageChange={onLanguageChange} />
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
            {jobs.length === 0 ? (
              <span>{t("jobs.empty")}</span>
            ) : (
              <ul className="jobs-tray-list">
                {jobs.map((job) => (
                  <li key={job.id}>
                    <span className={`job-phase ${job.phase}`}>{t(`jobs.phase.${job.phase}`)}</span>
                    <span className="job-label">{job.label}</span>
                    {job.total > 0 ? (
                      <span className="job-progress">
                        {job.completed}/{job.total}
                      </span>
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

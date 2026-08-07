import { useCallback, useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  createTranslator,
  DEFAULT_LOCALE,
  isLocaleCode,
  type LocaleCode,
  type Translator,
} from "./i18n";
import { formatProjectError, ProjectHub } from "./components/ProjectHub";
import { ProjectShell } from "./components/ProjectShell";
import type { UiError } from "./components/ErrorNotice";
import type { EngineEnvelope, EngineState } from "./engine/types";
import { emptyExecutionState, type ExecutionState } from "./engine/runReducer";
import { applyOpenResult, createTauriProjectStorage } from "./project/storage";
import { restoredProjectRoute } from "./project/normalize";
import type {
  ProjectRoute,
  ProjectCommandResult,
  ProjectState,
  RecentProjectEntry,
  RecoveryInfo,
} from "./project/types";
import "./App.css";

type AppPreferences = {
  language: LocaleCode;
};

const storage = createTauriProjectStorage();

function App() {
  const [locale, setLocale] = useState<LocaleCode>(DEFAULT_LOCALE);
  const [prefsReady, setPrefsReady] = useState(false);
  const [project, setProject] = useState<ProjectState | null>(null);
  const [route, setRoute] = useState<ProjectRoute>("overview");
  const [recent, setRecent] = useState<RecentProjectEntry[]>([]);
  const [recovery, setRecovery] = useState<RecoveryInfo>({
    present: false,
    path: null,
    name: null,
    updated_at: null,
  });
  const [busy, setBusy] = useState(false);
  const [hubError, setHubError] = useState<UiError | null>(null);
  const [projectError, setProjectError] = useState<UiError | null>(null);

  const [engineState, setEngineState] = useState<EngineState>("checking");
  const [enginePath, setEnginePath] = useState("");
  const [engineError, setEngineError] = useState("");
  const [capabilities, setCapabilities] = useState<EngineEnvelope | null>(null);
  const [execution] = useState<ExecutionState>(() => emptyExecutionState());

  const t = useMemo(() => createTranslator(locale), [locale]);

  const refreshHub = useCallback(async () => {
    const [recentList, recoveryInfo] = await Promise.all([
      storage.listRecent(),
      storage.recoveryInfo(),
    ]);
    setRecent(recentList);
    setRecovery(recoveryInfo);
  }, []);

  useEffect(() => {
    invoke<AppPreferences>("app_get_preferences")
      .then((prefs) => {
        if (isLocaleCode(prefs.language)) {
          setLocale(prefs.language);
        }
      })
      .catch((error: unknown) => {
        setLocale(DEFAULT_LOCALE);
        setHubError({
          summary: createTranslator(DEFAULT_LOCALE)("app.error.languageLoad"),
          detail: String(error),
        });
      })
      .finally(() => setPrefsReady(true));
  }, []);

  useEffect(() => {
    invoke<EngineEnvelope>("engine_capabilities")
      .then((result) => {
        setCapabilities(result);
        setEnginePath(result.path);
        setEngineState("ready");
        setEngineError("");
      })
      .catch((error: unknown) => {
        setEngineState("missing");
        setEngineError(String(error));
      });
  }, []);

  useEffect(() => {
    if (!prefsReady || project) return;
    refreshHub().catch((error: unknown) => {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    });
  }, [prefsReady, project, refreshHub, t]);

  useEffect(() => {
    if (!project?.project.dirty || project.project.readOnly || busy) return;
    const snapshot = project;
    const timer = window.setTimeout(() => {
      storage
        .autosave(snapshot)
        .then((result) => {
          const applied = applyOpenResult(result);
          if ("error" in applied) {
            setProjectError({
              summary: t("project.error.autosave"),
              detail: applied.error.message,
            });
            return;
          }
          setProject((current) => (current === snapshot ? applied.state : current));
        })
        .catch((error: unknown) => {
          setProjectError({ summary: t("project.error.autosave"), detail: String(error) });
        });
    }, 700);
    return () => window.clearTimeout(timer);
  }, [busy, project, t]);

  async function changeLanguage(next: LocaleCode) {
    setLocale(next);
    try {
      await invoke<AppPreferences>("app_set_language", {
        request: { language: next },
      });
    } catch (error) {
      const uiError = {
        summary: createTranslator(next)("app.error.languageSave"),
        detail: String(error),
      };
      if (project) {
        setProjectError(uiError);
      } else {
        setHubError(uiError);
      }
    }
  }

  async function openFromResult(result: Awaited<ReturnType<typeof storage.open>>, nextRoute: ProjectRoute) {
    const applied = applyOpenResult(result);
    if ("error" in applied) {
      if (applied.error.code === "cancelled") {
        setHubError(null);
        return;
      }
      setHubError({
        summary: formatProjectError(applied.error, t),
        detail: applied.error.message,
      });
      return;
    }
    setProject(applied.state);
    setProjectError(projectWarnings(result, t));
    setRoute(restoredProjectRoute(applied.state, nextRoute));
    setHubError(null);
    try {
      await refreshHub();
    } catch (error) {
      setProjectError(projectWarnings(result, t, error));
    }
  }

  async function handleNew(name: string) {
    setBusy(true);
    setHubError(null);
    try {
      const result = await storage.createNamed(name);
      await openFromResult(result, "overview");
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleOpen(path?: string) {
    setBusy(true);
    setHubError(null);
    try {
      const result = await storage.open(path);
      await openFromResult(result, "overview");
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleQuick() {
    setBusy(true);
    setHubError(null);
    try {
      const result = await storage.createUntitled();
      await openFromResult(result, "media");
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleRecover() {
    setBusy(true);
    setHubError(null);
    try {
      const result = await storage.recover();
      await openFromResult(result, "media");
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleDiscardRecovery() {
    setBusy(true);
    setHubError(null);
    try {
      setRecovery(await storage.discardRecovery());
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleRemoveRecent(path: string) {
    setBusy(true);
    try {
      const next = await storage.removeRecent(path);
      setRecent(next);
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleRevealRecent(path: string) {
    setBusy(true);
    setHubError(null);
    try {
      await storage.reveal(path);
    } catch (error) {
      setHubError({ summary: t("hub.error.generic"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  async function handleSave() {
    if (!project) return;
    setBusy(true);
    try {
      const result = await storage.save(project, {
        dialogName: project.project.untitled ? t("shell.untitled") : undefined,
      });
      const applied = applyOpenResult(result);
      if ("error" in applied) {
        if (applied.error.code !== "cancelled") {
          setProjectError({
            summary: formatProjectError(applied.error, t),
            detail: applied.error.message,
          });
        }
        return;
      }
      setProject(applied.state);
      setProjectError(projectWarnings(result, t));
      try {
        await refreshHub();
      } catch (error) {
        setProjectError(projectWarnings(result, t, error));
      }
    } catch (error) {
      setProjectError({ summary: t("project.error.save"), detail: String(error) });
    } finally {
      setBusy(false);
    }
  }

  function handleNavigate(nextRoute: ProjectRoute) {
    if (nextRoute === route) return;
    setRoute(nextRoute);
    setProject((current) =>
      current
        ? {
            ...current,
            project: {
              ...current.project,
              dirty: current.project.readOnly ? current.project.dirty : true,
            },
            uiStateByRoute: {
              ...current.uiStateByRoute,
              shell: { lastRoute: nextRoute },
            },
          }
        : current,
    );
  }

  function handleProjectChange(updater: (state: ProjectState) => ProjectState) {
    setProject((current) => {
      if (!current || current.project.readOnly) return current;
      const next = updater(current);
      return next === current
        ? current
        : { ...next, project: { ...next.project, dirty: true } };
    });
  }

  if (!prefsReady) {
    return <div className="boot-screen" />;
  }

  if (!project) {
    return (
      <ProjectHub
        t={t}
        recent={recent}
        recovery={recovery}
        busy={busy}
        error={hubError}
        onNew={handleNew}
        onOpen={() => handleOpen()}
        onQuick={handleQuick}
        onOpenRecent={(path) => handleOpen(path)}
        onRevealRecent={handleRevealRecent}
        onRemoveRecent={handleRemoveRecent}
        onRecover={handleRecover}
        onDiscardRecovery={handleDiscardRecovery}
        language={locale}
        onLanguageChange={changeLanguage}
        locale={locale}
      />
    );
  }

  return (
    <ProjectShell
      t={t}
      state={project}
      route={route}
      engineState={engineState}
      enginePath={enginePath}
      engineError={engineError}
      projectError={projectError}
      capabilities={capabilities}
      language={locale}
      onLanguageChange={changeLanguage}
      onNavigate={handleNavigate}
      onClose={() => {
        setProject(null);
        setProjectError(null);
        setRoute("overview");
        refreshHub().catch(() => undefined);
      }}
      onSave={handleSave}
      onProjectChange={handleProjectChange}
      onEngineError={setEngineError}
      onGeometrySuccess={() => {
        setEngineState("ready");
        setEngineError("");
      }}
      busy={busy}
      execution={execution}
    />
  );
}

function projectWarnings(
  result: ProjectCommandResult,
  t: Translator,
  refreshError?: unknown,
): UiError | null {
  const details = result.warnings.map((warning) => warning.message);
  if (refreshError !== undefined) {
    details.push(refreshError instanceof Error ? refreshError.message : String(refreshError));
  }
  const uniqueDetails = [...new Set(details.filter(Boolean))];
  if (!uniqueDetails.length) return null;
  return {
    summary: t("project.warning.maintenance"),
    detail: uniqueDetails.join("\n"),
  };
}

export default App;

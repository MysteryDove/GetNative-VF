import type { Translator } from "../i18n";
import { countById, readiness } from "../project/normalize";
import { runStatusLabel, runTypeLabel } from "../project/labels";
import type { ProjectRoute, ProjectState } from "../project/types";
import { RecipeManager } from "../components/RecipeManager";

export function OverviewPage({
  t,
  state,
  analyzeAvailable,
  onNavigate,
  onProjectChange,
}: {
  t: Translator;
  state: ProjectState;
  analyzeAvailable: boolean;
  onNavigate: (route: ProjectRoute) => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
}) {
  const flags = readiness(state, analyzeAvailable);
  const steps = [
    { key: "media", label: t("overview.step.media"), done: flags.hasMedia && !flags.hasMissingMedia },
    { key: "samples", label: t("overview.step.samples"), done: flags.hasSamples },
    { key: "height", label: t("overview.step.height"), done: false, blocked: !analyzeAvailable },
    { key: "kernel", label: t("overview.step.kernel"), done: false, blocked: !analyzeAvailable },
    { key: "recipe", label: t("overview.step.recipe"), done: flags.hasActiveRecipe },
    { key: "verify", label: t("overview.step.verify"), done: false, blocked: !flags.verifyReady },
  ];

  const blockers: Array<{ message: string; route: ProjectRoute }> = [];
  if (!flags.hasMedia) {
    blockers.push({ message: t("overview.blocker.importMedia"), route: "media" });
  } else if (flags.hasMissingMedia) {
    blockers.push({ message: t("overview.blocker.missingMedia"), route: "media" });
  }
  if (!flags.hasSamples) {
    blockers.push({ message: t("overview.blocker.noSamples"), route: "samples" });
  }
  if (!analyzeAvailable) {
    blockers.push({ message: t("overview.blocker.heightUnavailable"), route: "analyze" });
    blockers.push({ message: t("overview.blocker.kernelUnavailable"), route: "analyze" });
  }
  if (!flags.hasActiveRecipe) {
    blockers.push({ message: t("overview.blocker.noRecipe"), route: "analyze" });
  }
  if (!analyzeAvailable) {
    blockers.push({ message: t("overview.blocker.verifyUnavailable"), route: "verify" });
  }

  const runs = Object.values(state.runsById);

  return (
    <div className="page-panel">
      <div className="page-header">
        <div>
          <h2>{t("overview.title")}</h2>
          <span>{state.project.untitled ? t("shell.untitled") : state.project.name}</span>
        </div>
      </div>

      <div className="stat-strip">
        <Stat label={t("overview.stats.sources")} value={countById(state.sourcesById)} />
        <Stat label={t("overview.stats.samples")} value={countById(state.samplesById)} />
        <Stat label={t("overview.stats.recipes")} value={countById(state.recipesById)} />
        <Stat label={t("overview.stats.runs")} value={countById(state.runsById)} />
      </div>

      <section className="page-section">
        <h3>{t("overview.readiness")}</h3>
        <div className="readiness-strip" aria-label={t("overview.readiness")}>
          {steps.map((step, index) => (
            <div
              key={step.key}
              className={`readiness-step ${step.done ? "done" : ""} ${step.blocked ? "blocked" : ""}`}
            >
              <span className="step-index">{index + 1}</span>
              <span>{step.label}</span>
            </div>
          ))}
        </div>
      </section>

      <section className="page-section">
        <h3>{t("overview.blockers")}</h3>
        {blockers.length === 0 ? (
          <p className="empty-copy">{t("overview.noBlockers")}</p>
        ) : (
          <ul className="blocker-list">
            {blockers.map((blocker) => (
              <li key={blocker.message}>
                <span>{blocker.message}</span>
                <button className="link-button" onClick={() => onNavigate(blocker.route)}>
                  {t("overview.primaryAction")}
                </button>
              </li>
            ))}
          </ul>
        )}
      </section>

      <section className="page-section">
        <h3>{t("overview.recipes")}</h3>
        <RecipeManager t={t} state={state} onProjectChange={onProjectChange} />
      </section>

      <section className="page-section">
        <h3>{t("overview.recentRuns")}</h3>
        {runs.length === 0 ? (
          <p className="empty-copy">{t("overview.runsEmpty")}</p>
        ) : (
          <div className="dense-table">
            {runs.map((run) => (
              <div className="dense-row" key={run.id}>
                <strong>{runTypeLabel(run.runType, t)}</strong>
                <span>{runStatusLabel(run.status, t)}</span>
              </div>
            ))}
          </div>
        )}
      </section>
    </div>
  );
}

function Stat({ label, value }: { label: string; value: number }) {
  return (
    <div className="stat-cell">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

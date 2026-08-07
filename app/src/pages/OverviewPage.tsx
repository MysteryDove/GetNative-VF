import type { Translator } from "../i18n";
import { countById, readiness } from "../project/normalize";
import type { ProjectRoute, ProjectState } from "../project/types";

export function OverviewPage({
  t,
  state,
  analyzeAvailable,
  onNavigate,
}: {
  t: Translator;
  state: ProjectState;
  analyzeAvailable: boolean;
  onNavigate: (route: ProjectRoute) => void;
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

  const recipes = Object.values(state.recipesById);
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
        {recipes.length === 0 ? (
          <p className="empty-copy">{t("overview.recipesEmpty")}</p>
        ) : (
          <div className="dense-table">
            {recipes.map((recipe) => {
              const parts = [t("recipe.revision", { revision: recipe.revision })];
              if (recipe.locked) parts.push(t("recipe.locked"));
              if (state.project.activeRecipeId === recipe.id) parts.push(t("recipe.active"));
              return (
                <div className="dense-row" key={recipe.id}>
                  <strong>{recipe.name}</strong>
                  <span>{parts.join(" · ")}</span>
                </div>
              );
            })}
          </div>
        )}
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

function runTypeLabel(value: string, t: Translator): string {
  if (value === "height" || value === "height_analysis") return t("overview.run.resolution");
  if (value === "kernel" || value === "kernel_analysis") return t("overview.run.algorithm");
  if (value === "verification" || value === "verify") return t("overview.run.check");
  return t("overview.run.unknown");
}

function runStatusLabel(value: string, t: Translator): string {
  if (value === "queued") return t("overview.runStatus.queued");
  if (value === "running") return t("overview.runStatus.running");
  if (value === "completed") return t("overview.runStatus.completed");
  if (value === "failed") return t("overview.runStatus.failed");
  if (value === "cancelled") return t("overview.runStatus.cancelled");
  if (value === "partial") return t("overview.runStatus.partial");
  return t("overview.runStatus.unknown");
}

function Stat({ label, value }: { label: string; value: number }) {
  return (
    <div className="stat-cell">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

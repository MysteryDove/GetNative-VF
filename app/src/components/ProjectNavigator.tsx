import {
  Activity,
  Cpu,
  Gauge,
  Play,
  SlidersHorizontal,
  Zap,
} from "lucide-react";
import type { Translator } from "../i18n";
import type { ProjectRoute } from "../project/types";

const items: Array<{
  route: ProjectRoute;
  icon: typeof Activity;
  labelKey: Parameters<Translator>[0];
  countKey?: "sources" | "samples" | "runs";
}> = [
  { route: "overview", icon: Gauge, labelKey: "nav.overview" },
  { route: "media", icon: Play, labelKey: "nav.media", countKey: "sources" },
  { route: "samples", icon: Activity, labelKey: "nav.samples", countKey: "samples" },
  { route: "analyze", icon: SlidersHorizontal, labelKey: "nav.analyze" },
  { route: "verify", icon: Zap, labelKey: "nav.verify" },
  { route: "results", icon: Cpu, labelKey: "nav.results", countKey: "runs" },
];

export function ProjectNavigator({
  t,
  route,
  counts,
  onNavigate,
  onSettings,
  onDiagnostics,
}: {
  t: Translator;
  route: ProjectRoute;
  counts: { sources: number; samples: number; runs: number };
  onNavigate: (route: ProjectRoute) => void;
  onSettings: () => void;
  onDiagnostics: () => void;
}) {
  return (
    <nav className="project-nav" aria-label={t("app.name")}>
      <div className="project-nav-main">
        {items.map((item) => {
          const Icon = item.icon;
          const active = route === item.route;
          return (
            <button
              key={item.route}
              className={`nav-item ${active ? "active" : ""}`}
              onClick={() => onNavigate(item.route)}
              aria-current={active ? "page" : undefined}
              aria-label={t(item.labelKey)}
              title={t(item.labelKey)}
            >
              <Icon size={16} />
              <span>{t(item.labelKey)}</span>
              {item.countKey ? <small>{counts[item.countKey]}</small> : null}
            </button>
          );
        })}
      </div>
      <div className="project-nav-footer">
        <button
          className={`nav-item ${route === "settings" ? "active" : ""}`}
          onClick={onSettings}
          aria-label={t("nav.settings")}
          title={t("nav.settings")}
        >
          <SlidersHorizontal size={16} />
          <span>{t("nav.settings")}</span>
        </button>
        <button
          className={`nav-item ${route === "diagnostics" ? "active" : ""}`}
          onClick={onDiagnostics}
          aria-label={t("nav.diagnostics")}
          title={t("nav.diagnostics")}
        >
          <Cpu size={16} />
          <span>{t("nav.diagnostics")}</span>
        </button>
      </div>
    </nav>
  );
}

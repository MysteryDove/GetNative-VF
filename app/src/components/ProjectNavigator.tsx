import {
  Activity,
  ChevronDown,
  Cpu,
  Gauge,
  PanelLeftClose,
  PanelLeftOpen,
  Play,
  SlidersHorizontal,
  Zap,
} from "lucide-react";
import { useState } from "react";
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
  collapsed,
  onNavigate,
  onToggleCollapse,
  onSettings,
  onDiagnostics,
  analyzeSubroute,
  onAnalyzeSubroute,
}: {
  t: Translator;
  route: ProjectRoute;
  counts: { sources: number; samples: number; runs: number };
  collapsed: boolean;
  onNavigate: (route: ProjectRoute) => void;
  onToggleCollapse: () => void;
  onSettings: () => void;
  onDiagnostics: () => void;
  analyzeSubroute: "height" | "kernel";
  onAnalyzeSubroute: (subroute: "height" | "kernel") => void;
}) {
  const CollapseIcon = collapsed ? PanelLeftOpen : PanelLeftClose;
  const collapseLabel = t(collapsed ? "nav.expand" : "nav.collapse");
  // The analyze group starts expanded when the analyze route is active.
  const [analyzeOpen, setAnalyzeOpen] = useState(() => route === "analyze");
  const analyzeToggleLabel = t(analyzeOpen ? "nav.group.collapse" : "nav.group.expand");
  return (
    <nav className={`project-nav ${collapsed ? "collapsed" : ""}`} aria-label={t("app.name")}>
      <div className="project-nav-main">
        {items.map((item) => {
          const Icon = item.icon;
          const active = route === item.route;
          if (item.route === "analyze") {
            return (
              <div className="nav-group" key={item.route}>
                <div className="nav-group-row">
                  <button
                    className={`nav-item ${active ? "active" : ""}`}
                    onClick={() => {
                      // Clicking Analyze always lands on the resolution test.
                      setAnalyzeOpen(true);
                      onAnalyzeSubroute("height");
                    }}
                    aria-current={active ? "page" : undefined}
                    aria-label={t(item.labelKey)}
                    title={t(item.labelKey)}
                  >
                    <Icon size={16} />
                    <span>{t(item.labelKey)}</span>
                  </button>
                  {!collapsed ? (
                    <button
                      type="button"
                      className="nav-group-toggle"
                      onClick={() => setAnalyzeOpen((open) => !open)}
                      aria-expanded={analyzeOpen}
                      aria-label={analyzeToggleLabel}
                      title={analyzeToggleLabel}
                    >
                      <ChevronDown size={14} className={analyzeOpen ? "open" : ""} />
                    </button>
                  ) : null}
                </div>
                {analyzeOpen && !collapsed ? (
                  <div className="nav-subitems">
                    <button
                      type="button"
                      className={`nav-subitem ${active && analyzeSubroute === "height" ? "active" : ""}`}
                      onClick={() => onAnalyzeSubroute("height")}
                    >
                      {t("analyze.height")}
                    </button>
                    <button
                      type="button"
                      className={`nav-subitem ${active && analyzeSubroute === "kernel" ? "active" : ""}`}
                      onClick={() => onAnalyzeSubroute("kernel")}
                    >
                      {t("analyze.kernel")}
                    </button>
                  </div>
                ) : null}
              </div>
            );
          }
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
          className="nav-item nav-collapse-toggle"
          onClick={onToggleCollapse}
          aria-label={collapseLabel}
          aria-expanded={!collapsed}
          title={collapseLabel}
        >
          <CollapseIcon size={16} />
          <span>{collapseLabel}</span>
        </button>
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

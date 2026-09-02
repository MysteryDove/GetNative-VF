import {
  Cpu,
  Gauge,
  PanelLeftClose,
  PanelLeftOpen,
  Play,
  Search,
  SlidersHorizontal,
} from "lucide-react";
import type { Translator } from "../i18n";
import type { ProjectRoute } from "../project/types";

const items: Array<{
  route: ProjectRoute;
  analyzeSubroute?: "height" | "kernel";
  icon?: typeof Gauge;
  glyph?: string;
  labelKey: Parameters<Translator>[0];
  countKey?: "sources" | "samples" | "runs";
}> = [
  { route: "overview", icon: Gauge, labelKey: "nav.overview" },
  { route: "media", icon: Play, labelKey: "nav.media", countKey: "sources" },
  { route: "analyze", analyzeSubroute: "height", glyph: "R", labelKey: "analyze.height" },
  { route: "analyze", analyzeSubroute: "kernel", glyph: "A", labelKey: "analyze.kernel" },
  { route: "verify", icon: Search, labelKey: "nav.verify" },
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
  return (
    <nav className={`project-nav ${collapsed ? "collapsed" : ""}`} aria-label={t("app.name")}>
      <div className="project-nav-main">
        {items.map((item) => {
          const Icon = item.icon;
          const active =
            route === item.route &&
            (item.analyzeSubroute == null || analyzeSubroute === item.analyzeSubroute);
          return (
            <button
              key={`${item.route}:${item.analyzeSubroute ?? ""}`}
              className={`nav-item ${active ? "active" : ""}`}
              onClick={() => {
                if (item.analyzeSubroute) onAnalyzeSubroute(item.analyzeSubroute);
                else onNavigate(item.route);
              }}
              aria-current={active ? "page" : undefined}
              aria-label={t(item.labelKey)}
              title={t(item.labelKey)}
            >
              {item.glyph ? (
                <em className="nav-letter" aria-hidden="true">{item.glyph}</em>
              ) : Icon ? (
                <Icon size={16} />
              ) : null}
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

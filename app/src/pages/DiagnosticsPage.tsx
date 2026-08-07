import { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  Check,
  Cpu,
  Gauge,
  LoaderCircle,
  Play,
  SlidersHorizontal,
  X,
  Zap,
} from "lucide-react";
import type { Translator } from "../i18n";
import { BackendStatusList } from "../components/BackendStatusList";
import { ErrorNotice } from "../components/ErrorNotice";
import {
  buildBackendRows,
  type EngineEnvelope,
  type EngineState,
  type Geometry,
  type GeometryEnvelope,
  type GeometryMode,
  type KernelCapability,
} from "../engine/types";
import { getMediaCapabilities, type MediaCapabilities } from "../media/service";

function requiredNumeric(value: string, label: string, t: Translator): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    throw new Error(t("diagnostics.invalidNumber", { field: label }));
  }
  return parsed;
}

function optionalNumeric(value: string, label: string, t: Translator): number | null {
  return value.trim() === "" ? null : requiredNumeric(value, label, t);
}

export function DiagnosticsPage({
  t,
  engineState,
  enginePath,
  engineError,
  capabilities,
  onEngineError,
  onGeometrySuccess,
}: {
  t: Translator;
  engineState: EngineState;
  enginePath: string;
  engineError: string;
  capabilities: EngineEnvelope | null;
  onEngineError: (message: string) => void;
  onGeometrySuccess: () => void;
}) {
  const [geometryMode, setGeometryMode] = useState<GeometryMode>("standard");
  const [profile, setProfile] = useState("muf-d278cd3");
  const [sourceWidth, setSourceWidth] = useState("1488");
  const [sourceHeight, setSourceHeight] = useState("837");
  const [activeWidth, setActiveWidth] = useState("1488");
  const [activeHeight, setActiveHeight] = useState("837");
  const [baseWidth, setBaseWidth] = useState("");
  const [baseHeight, setBaseHeight] = useState("838");
  const [geometry, setGeometry] = useState<Geometry | null>(null);
  const [busy, setBusy] = useState(false);
  const [localError, setLocalError] = useState("");
  const [mediaCapabilities, setMediaCapabilities] = useState<MediaCapabilities | null>(null);

  useEffect(() => {
    getMediaCapabilities().then(setMediaCapabilities).catch(() => setMediaCapabilities(null));
  }, []);

  const analysisAvailable = capabilities?.payload.commands.analyze ?? false;
  const profiles = capabilities?.payload.profiles ?? [];
  const kernels = capabilities?.payload.kernels ?? [];
  const backendRows = useMemo(
    () => buildBackendRows(capabilities?.payload.backends),
    [capabilities],
  );
  const canvasRatio = useMemo(() => {
    const width = geometry?.width ?? Number(sourceWidth);
    const height = geometry?.height ?? Number(sourceHeight);
    return Number.isFinite(width) && Number.isFinite(height) && width > 0 && height > 0
      ? `${width} / ${height}`
      : "16 / 9";
  }, [geometry, sourceWidth, sourceHeight]);

  async function previewGeometry() {
    setBusy(true);
    setLocalError("");
    try {
      const result = await invoke<GeometryEnvelope>("engine_geometry", {
        request: {
          profile,
          mode: geometryMode,
          sourceWidth: requiredNumeric(sourceWidth, t("diagnostics.sourceW"), t),
          sourceHeight: requiredNumeric(sourceHeight, t("diagnostics.sourceH"), t),
          activeWidth: requiredNumeric(activeWidth, t("diagnostics.activeW"), t),
          activeHeight: requiredNumeric(activeHeight, t("diagnostics.activeH"), t),
          baseHeight: optionalNumeric(baseHeight, t("diagnostics.baseH"), t),
          baseWidth: optionalNumeric(baseWidth, t("diagnostics.baseW"), t),
        },
      });
      setGeometry(result.payload.geometry);
      onGeometrySuccess();
    } catch (error) {
      const message = String(error);
      setLocalError(message);
      onEngineError(message);
    } finally {
      setBusy(false);
    }
  }

  const error = localError || engineError;

  return (
    <div className={`diagnostics-page ${error ? "has-error" : ""}`}>
      <div className="page-header diagnostics-header">
        <div>
          <h2>{t("diagnostics.title")}</h2>
          <span>{t("diagnostics.subtitle")}</span>
        </div>
        <div className="top-actions">
          <div className={`command-badge ${analysisAvailable ? "available" : "unavailable"}`}>
            {analysisAvailable ? <Check size={14} /> : <X size={14} />}
            {analysisAvailable ? t("engine.analyzeAvailable") : t("engine.analyzeUnavailable")}
          </div>
          <button
            className="run-button"
            onClick={previewGeometry}
            disabled={busy || engineState !== "ready"}
          >
            {busy ? <LoaderCircle className="spin" size={16} /> : <Play size={16} fill="currentColor" />}
            {busy ? t("diagnostics.working") : t("diagnostics.runPreview")}
          </button>
        </div>
      </div>

      {error ? (
        <ErrorNotice error={{ summary: t("diagnostics.errorSummary"), detail: error }} t={t} />
      ) : null}

      <main className="workspace capability-workspace">
        <aside className="parameters-panel">
          <section className="panel-section">
            <div className="section-heading">
              <SlidersHorizontal size={17} />
              <h2>{t("diagnostics.geometry")}</h2>
            </div>
            <label className="select-field first-field">
              <span>{t("diagnostics.profile")}</span>
              <div>
                <select value={profile} onChange={(event) => setProfile(event.target.value)}>
                  {profiles.map((item) => (
                    <option value={item.id} key={item.id}>
                      {profileLabel(item.id, t)}
                    </option>
                  ))}
                </select>
              </div>
            </label>
            <div className="axis-switch" aria-label={t("diagnostics.mode")}>
              {(["standard", "pro"] as GeometryMode[]).map((item) => (
                <button
                  className={geometryMode === item ? "active" : ""}
                  key={item}
                  onClick={() => setGeometryMode(item)}
                >
                  {item === "standard" ? t("diagnostics.mode.standard") : t("diagnostics.mode.pro")}
                </button>
              ))}
            </div>
          </section>

          <section className="panel-section">
            <div className="section-heading">
              <Gauge size={17} />
              <h2>{t("diagnostics.dimensions")}</h2>
            </div>
            <div className="two-column-fields no-top-margin">
              <Field label={t("diagnostics.sourceW")} value={sourceWidth} onChange={setSourceWidth} suffix={t("common.px")} />
              <Field label={t("diagnostics.sourceH")} value={sourceHeight} onChange={setSourceHeight} suffix={t("common.px")} />
            </div>
            <div className="two-column-fields">
              <Field label={t("diagnostics.activeW")} value={activeWidth} onChange={setActiveWidth} suffix={t("common.px")} />
              <Field label={t("diagnostics.activeH")} value={activeHeight} onChange={setActiveHeight} suffix={t("common.px")} />
            </div>
            <div className="two-column-fields">
              <Field label={t("diagnostics.baseW")} value={baseWidth} onChange={setBaseWidth} suffix={t("common.px")} />
              <Field label={t("diagnostics.baseH")} value={baseHeight} onChange={setBaseHeight} suffix={t("common.px")} />
            </div>
          </section>
        </aside>

        <section className="analysis-panel geometry-panel">
          <div className="analysis-toolbar">
            <div>
              <h2>{t("diagnostics.preview")}</h2>
              <span>
                {profileLabel(profile, t)} / {geometryModeLabel(geometryMode, t)}
              </span>
            </div>
          </div>

          <div className="geometry-preview-region">
            <div className="geometry-canvas" style={{ aspectRatio: canvasRatio }}>
              <span>
                {geometry
                  ? `${geometry.width} x ${geometry.height}`
                  : t("diagnostics.noGeometry")}
              </span>
            </div>
            <div className="geometry-readout">
              <GeometryRow
                label={t("diagnostics.canvas")}
                value={geometry ? `${geometry.width} x ${geometry.height}` : "-"}
              />
              <GeometryRow
                label={t("diagnostics.active")}
                value={geometry ? `${geometry.src_width} x ${geometry.src_height}` : "-"}
              />
              <GeometryRow
                label={t("diagnostics.offset")}
                value={geometry ? `${geometry.src_left}, ${geometry.src_top}` : "-"}
              />
              <GeometryRow
                label={t("diagnostics.parity")}
                value={
                  geometry
                    ? `${geometry.width % 2 ? t("diagnostics.odd") : t("diagnostics.even")} / ${geometry.height % 2 ? t("diagnostics.odd") : t("diagnostics.even")}`
                    : "-"
                }
              />
            </div>
          </div>

          <div className="jobs-bar">
            <div className="job-icon">
              <Cpu size={17} />
            </div>
            <div className="job-copy">
              <strong>{geometry ? t("diagnostics.jobDone") : t("diagnostics.jobReady")}</strong>
              <span>{engineState === "ready" ? enginePath : t("engine.missing")}</span>
            </div>
            <div className="job-progress">
              <span style={{ width: geometry ? "100%" : "0%" }} />
            </div>
            <span className="job-time">
              {geometry ? t("diagnostics.jobComplete") : t("diagnostics.jobIdle")}
            </span>
          </div>
        </section>

        <aside className="results-panel capabilities-panel">
          <div className="results-heading">
            <div>
              <h2>{t("diagnostics.capabilities")}</h2>
              <span>{t("diagnostics.schema", { version: capabilities?.payload.schema_version ?? "-" })}</span>
            </div>
            <Zap size={18} />
          </div>

          <BackendStatusList rows={backendRows} t={t} />

          <div className="kernel-section media-tool-section">
            <h3>{t("diagnostics.mediaTools")}</h3>
            {(["ffprobe", "ffmpeg"] as const).map((name) => {
              const tool = mediaCapabilities?.[name];
              return (
                <div className="kernel-row" key={name} title={tool?.path ?? undefined}>
                  <span>{name}</span>
                  <strong className={tool?.available ? "tool-ready" : "tool-unavailable"}>
                    {tool?.available
                      ? t("diagnostics.mediaToolReady", { source: tool.source })
                      : t("diagnostics.mediaToolUnavailable")}
                  </strong>
                </div>
              );
            })}
          </div>

          <div className="kernel-section">
            <h3>{t("diagnostics.kernels")}</h3>
            {kernels.map((kernel) => (
              <div className="kernel-row" key={kernel.id}>
                <span>{kernelLabel(kernel.id, t)}</span>
                <strong>{kernelSummary(kernel, t)}</strong>
              </div>
            ))}
          </div>
        </aside>
      </main>
    </div>
  );
}

function profileLabel(id: string, t: Translator): string {
  if (id === "muf-d278cd3") return t("profile.muf-d278cd3");
  if (id === "getfnative-44c8d0f") return t("profile.getfnative-44c8d0f");
  if (id === "modern") return t("profile.modern");
  return id;
}

function geometryModeLabel(mode: GeometryMode, t: Translator): string {
  return mode === "standard" ? t("diagnostics.mode.standard") : t("diagnostics.mode.pro");
}

function kernelLabel(id: string, t: Translator): string {
  if (id === "bilinear") return t("diagnostics.kernelName.bilinear");
  if (id === "bicubic") return t("diagnostics.kernelName.bicubic");
  if (id === "lanczos") return t("diagnostics.kernelName.lanczos");
  if (id === "spline16") return t("diagnostics.kernelName.spline16");
  if (id === "spline36") return t("diagnostics.kernelName.spline36");
  if (id === "spline64") return t("diagnostics.kernelName.spline64");
  return id;
}

function kernelSummary(kernel: KernelCapability, t: Translator): string {
  if (kernel.id === "bicubic") return t("diagnostics.kernel.bicubic");
  if (kernel.id === "lanczos") {
    return t("diagnostics.kernel.lanczos", {
      guiMin: String(kernel.parameters.gui_min),
      guiMax: String(kernel.parameters.gui_max),
      coreMin: String(kernel.parameters.core_min),
      coreMax: String(kernel.parameters.core_max),
    });
  }
  return t("diagnostics.kernel.fixed");
}

function Field({
  label,
  value,
  suffix,
  onChange,
}: {
  label: string;
  value: string;
  suffix?: string;
  onChange: (value: string) => void;
}) {
  return (
    <label className="number-field">
      <span>{label}</span>
      <div>
        <input value={value} inputMode="decimal" onChange={(event) => onChange(event.target.value)} />
        {suffix && <small>{suffix}</small>}
      </div>
    </label>
  );
}

function GeometryRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="geometry-row">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

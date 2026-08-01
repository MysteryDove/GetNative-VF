import { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  Activity,
  AlertTriangle,
  Check,
  Cpu,
  Gauge,
  LoaderCircle,
  Play,
  SlidersHorizontal,
  X,
  Zap,
} from "lucide-react";
import "./App.css";

type GeometryMode = "standard" | "pro";
type EngineState = "checking" | "ready" | "missing";

type Geometry = {
  width: number;
  height: number;
  src_left: number;
  src_top: number;
  src_width: number;
  src_height: number;
};

type KernelCapability = {
  id: string;
  parameters: Record<string, string | number | boolean>;
};

type BackendCapability = {
  id: string;
  compiled: boolean;
  device_available: boolean;
  analysis_command_available: boolean;
  axes: string[];
  p_norms: { minimum: number; maximum: number } | null;
  max_half_bandwidth: number | null;
  max_forward_width: number | null;
  device?: string;
  reason?: string;
  compiled_isa?: string[];
  available_isa?: string[];
  selected_isa?: string;
  math_modes?: string[];
  selected_math_mode?: string;
  selection_reason?: string;
};

type EngineEnvelope = {
  path: string;
  payload: {
    schema_version: number;
    engine: string;
    version: string;
    commands: {
      capabilities: boolean;
      geometry: boolean;
      analyze: boolean;
    };
    kernels: KernelCapability[];
    backends: BackendCapability[];
    profiles: Array<{ id: string; default_crop: number }>;
  };
};

type GeometryEnvelope = {
  path: string;
  payload: {
    geometry: Geometry;
    profile: string;
    mode: GeometryMode;
  };
};

const profileLabels: Record<string, string> = {
  "muf-d278cd3": "MUF compatibility",
  "getfnative-44c8d0f": "GetFnative compatibility",
  modern: "Modern deterministic",
};

function requiredNumeric(value: string, label: string): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    throw new Error(`${label} must be finite`);
  }
  return parsed;
}

function optionalNumeric(value: string, label: string): number | null {
  return value.trim() === "" ? null : requiredNumeric(value, label);
}

function App() {
  const [geometryMode, setGeometryMode] = useState<GeometryMode>("standard");
  const [profile, setProfile] = useState("muf-d278cd3");
  const [sourceWidth, setSourceWidth] = useState("1488");
  const [sourceHeight, setSourceHeight] = useState("837");
  const [activeWidth, setActiveWidth] = useState("1488");
  const [activeHeight, setActiveHeight] = useState("837");
  const [baseWidth, setBaseWidth] = useState("");
  const [baseHeight, setBaseHeight] = useState("838");
  const [engineState, setEngineState] = useState<EngineState>("checking");
  const [enginePath, setEnginePath] = useState("");
  const [engineError, setEngineError] = useState("");
  const [capabilities, setCapabilities] = useState<EngineEnvelope | null>(null);
  const [geometry, setGeometry] = useState<Geometry | null>(null);
  const [busy, setBusy] = useState(false);

  const analysisAvailable = capabilities?.payload.commands.analyze ?? false;
  const profiles = capabilities?.payload.profiles ?? [];
  const backends = capabilities?.payload.backends ?? [];
  const kernels = capabilities?.payload.kernels ?? [];
  const canvasRatio = useMemo(() => {
    const width = geometry?.width ?? Number(sourceWidth);
    const height = geometry?.height ?? Number(sourceHeight);
    return Number.isFinite(width) && Number.isFinite(height) && width > 0 && height > 0
      ? `${width} / ${height}`
      : "16 / 9";
  }, [geometry, sourceWidth, sourceHeight]);

  useEffect(() => {
    invoke<EngineEnvelope>("engine_capabilities")
      .then((result) => {
        setCapabilities(result);
        setEnginePath(result.path);
        setEngineState("ready");
      })
      .catch((error: unknown) => {
        setEngineState("missing");
        setEngineError(String(error));
      });
  }, []);

  async function previewGeometry() {
    setBusy(true);
    setEngineError("");
    try {
      const result = await invoke<GeometryEnvelope>("engine_geometry", {
        request: {
          profile,
          mode: geometryMode,
          sourceWidth: requiredNumeric(sourceWidth, "Source width"),
          sourceHeight: requiredNumeric(sourceHeight, "Source height"),
          activeWidth: requiredNumeric(activeWidth, "Active width"),
          activeHeight: requiredNumeric(activeHeight, "Active height"),
          baseHeight: optionalNumeric(baseHeight, "Base height"),
          baseWidth: optionalNumeric(baseWidth, "Base width"),
        },
      });
      setGeometry(result.payload.geometry);
      setEngineState("ready");
    } catch (error) {
      setEngineError(String(error));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={`app-shell ${engineError ? "has-error" : ""}`}>
      <header className="topbar">
        <div className="brand-block">
          <div className="brand-mark" aria-hidden="true">
            <Activity size={19} strokeWidth={2.2} />
          </div>
          <div>
            <h1>GetNative VF</h1>
            <span>Geometry workbench</span>
          </div>
        </div>

        <div className="mode-switch" role="tablist" aria-label="Workbench mode">
          <button className="active" role="tab" aria-selected="true">Geometry</button>
          <button role="tab" aria-selected="false" disabled={!analysisAvailable}>Analysis</button>
        </div>

        <div className="top-actions">
          <div className={`engine-state ${engineState}`} title={enginePath || engineError}>
            <span className="status-dot" />
            {engineState === "ready"
              ? `Engine ${capabilities?.payload.version ?? ""}`
              : engineState}
          </div>
          <button
            className="run-button"
            onClick={previewGeometry}
            disabled={busy || engineState !== "ready"}
          >
            {busy ? <LoaderCircle className="spin" size={16} /> : <Play size={16} fill="currentColor" />}
            {busy ? "Working" : "Preview"}
          </button>
        </div>
      </header>

      {engineError && (
        <div className="error-strip" role="alert">
          <AlertTriangle size={16} />
          <span>{engineError}</span>
        </div>
      )}

      <main className="workspace capability-workspace">
        <aside className="parameters-panel">
          <section className="panel-section">
            <div className="section-heading">
              <SlidersHorizontal size={17} />
              <h2>Geometry</h2>
            </div>
            <label className="select-field first-field">
              <span>Profile</span>
              <div>
                <select value={profile} onChange={(event) => setProfile(event.target.value)}>
                  {profiles.map((item) => (
                    <option value={item.id} key={item.id}>
                      {profileLabels[item.id] ?? item.id}
                    </option>
                  ))}
                </select>
              </div>
            </label>
            <div className="axis-switch" aria-label="Geometry mode">
              {(["standard", "pro"] as GeometryMode[]).map((item) => (
                <button
                  className={geometryMode === item ? "active" : ""}
                  key={item}
                  onClick={() => setGeometryMode(item)}
                >
                  {item === "standard" ? "Standard" : "Pro"}
                </button>
              ))}
            </div>
          </section>

          <section className="panel-section">
            <div className="section-heading">
              <Gauge size={17} />
              <h2>Dimensions</h2>
            </div>
            <div className="two-column-fields no-top-margin">
              <Field label="Source W" value={sourceWidth} onChange={setSourceWidth} suffix="px" />
              <Field label="Source H" value={sourceHeight} onChange={setSourceHeight} suffix="px" />
            </div>
            <div className="two-column-fields">
              <Field label="Active W" value={activeWidth} onChange={setActiveWidth} suffix="px" />
              <Field label="Active H" value={activeHeight} onChange={setActiveHeight} suffix="px" />
            </div>
            <div className="two-column-fields">
              <Field label="Base W" value={baseWidth} onChange={setBaseWidth} suffix="px" />
              <Field label="Base H" value={baseHeight} onChange={setBaseHeight} suffix="px" />
            </div>
          </section>
        </aside>

        <section className="analysis-panel geometry-panel">
          <div className="analysis-toolbar">
            <div>
              <h2>Geometry preview</h2>
              <span>{profileLabels[profile] ?? profile} / {geometryMode}</span>
            </div>
            <div className={`command-badge ${analysisAvailable ? "available" : "unavailable"}`}>
              {analysisAvailable ? <Check size={14} /> : <X size={14} />}
              Analysis command {analysisAvailable ? "available" : "unavailable"}
            </div>
          </div>

          <div className="geometry-preview-region">
            <div className="geometry-canvas" style={{ aspectRatio: canvasRatio }}>
              <span>{geometry ? `${geometry.width} x ${geometry.height}` : "No geometry result"}</span>
            </div>
            <div className="geometry-readout">
              <GeometryRow label="Canvas" value={geometry ? `${geometry.width} x ${geometry.height}` : "-"} />
              <GeometryRow label="Active" value={geometry ? `${geometry.src_width} x ${geometry.src_height}` : "-"} />
              <GeometryRow label="Offset" value={geometry ? `${geometry.src_left}, ${geometry.src_top}` : "-"} />
              <GeometryRow
                label="Parity"
                value={geometry ? `${geometry.width % 2 ? "odd" : "even"} / ${geometry.height % 2 ? "odd" : "even"}` : "-"}
              />
            </div>
          </div>

          <div className="jobs-bar">
            <div className="job-icon"><Cpu size={17} /></div>
            <div className="job-copy">
              <strong>{geometry ? "Geometry preview complete" : "Ready for geometry preview"}</strong>
              <span>{engineState === "ready" ? enginePath : "Engine unavailable"}</span>
            </div>
            <div className="job-progress"><span style={{ width: geometry ? "100%" : "0%" }} /></div>
            <span className="job-time">{geometry ? "done" : "idle"}</span>
          </div>
        </section>

        <aside className="results-panel capabilities-panel">
          <div className="results-heading">
            <div>
              <h2>Capabilities</h2>
              <span>Engine schema {capabilities?.payload.schema_version ?? "-"}</span>
            </div>
            <Zap size={18} />
          </div>

          <div className="backend-list">
            {backends.map((backend) => <BackendRow backend={backend} key={backend.id} />)}
          </div>

          <div className="kernel-section">
            <h3>Kernel contract</h3>
            {kernels.map((kernel) => (
              <div className="kernel-row" key={kernel.id}>
                <span>{kernel.id}</span>
                <strong>{kernelSummary(kernel)}</strong>
              </div>
            ))}
          </div>
        </aside>
      </main>
    </div>
  );
}

function kernelSummary(kernel: KernelCapability): string {
  if (kernel.id === "bicubic") return "finite B / C";
  if (kernel.id === "lanczos") {
    return `${kernel.parameters.gui_min}-${kernel.parameters.gui_max} GUI / ${kernel.parameters.core_min}-${kernel.parameters.core_max} core`;
  }
  return "fixed";
}

function BackendRow({ backend }: { backend: BackendCapability }) {
  const ready = backend.compiled && backend.device_available;
  const pNorm = backend.p_norms
    ? backend.p_norms.maximum === 4_294_967_295
      ? `p >= ${backend.p_norms.minimum}`
      : `p${backend.p_norms.minimum}`
    : "";
  const status = !backend.compiled
    ? "Not compiled"
    : !backend.device_available
      ? "No device"
      : backend.analysis_command_available
        ? "Analysis ready"
        : "Device ready";
  const shape = backend.max_half_bandwidth && backend.max_forward_width
    ? ` | shape ${backend.max_half_bandwidth}/${backend.max_forward_width}`
    : "";
  const executionMode = backend.selected_isa
    ? ` | ${backend.selected_isa} / ${backend.selected_math_mode ?? "production"}`
    : backend.selected_math_mode
      ? ` | ${backend.selected_math_mode}`
      : "";
  return (
    <div className="backend-row">
      <div className={`backend-icon ${ready ? "ready" : "unavailable"}`}>
        {ready ? <Check size={15} /> : <X size={15} />}
      </div>
      <div>
        <strong>{backend.id.toUpperCase()}</strong>
        <span>{status}</span>
        <small>
          {backend.axes.length
              ? `${backend.analysis_command_available ? "command ready" : "no analyze command"} | ${backend.axes.join(" / ")} | ${pNorm}${shape}${executionMode}`
            : backend.reason}
        </small>
      </div>
    </div>
  );
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

export default App;

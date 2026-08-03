use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;
use tauri::{AppHandle, Manager};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct GeometryRequest {
    profile: String,
    mode: String,
    source_width: f64,
    source_height: f64,
    active_width: f64,
    active_height: f64,
    base_height: Option<f64>,
    base_width: Option<f64>,
}

#[derive(Deserialize)]
struct CommandCapabilities {
    capabilities: bool,
    geometry: bool,
    analyze: bool,
}

#[derive(Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum KernelParameters {
    None,
    BicubicBc {
        finite: bool,
    },
    IntegerTaps {
        gui_min: u32,
        gui_max: u32,
        core_min: u32,
        core_max: u32,
    },
}

#[derive(Deserialize)]
struct KernelCapability {
    id: String,
    parameters: KernelParameters,
}

#[derive(Deserialize)]
struct PNormRange {
    minimum: u64,
    maximum: u64,
}

#[derive(Deserialize)]
struct BackendCapability {
    id: String,
    compiled: bool,
    device_available: bool,
    analysis_command_available: bool,
    axes: Vec<String>,
    p_norms: Option<PNormRange>,
    max_half_bandwidth: Option<u32>,
    max_forward_width: Option<u32>,
    #[serde(default)]
    compiled_isa: Option<Vec<String>>,
    #[serde(default)]
    available_isa: Option<Vec<String>>,
    #[serde(default)]
    selected_isa: Option<String>,
    #[serde(default)]
    math_modes: Option<Vec<String>>,
    #[serde(default)]
    selected_math_mode: Option<String>,
    #[serde(default)]
    selection_reason: Option<String>,
    #[serde(default)]
    reason: Option<String>,
}

#[derive(Deserialize)]
struct ProfileCapability {
    id: String,
    default_crop: u32,
}

#[derive(Deserialize)]
struct EngineCapabilities {
    schema_version: u32,
    engine: String,
    commands: CommandCapabilities,
    kernels: Vec<KernelCapability>,
    backends: Vec<BackendCapability>,
    profiles: Vec<ProfileCapability>,
}

fn engine_candidates(app: &AppHandle) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let engine_name = format!("getnative-engine{}", env::consts::EXE_SUFFIX);
    if let Some(path) = env::var_os("GETNATIVE_ENGINE_PATH") {
        paths.push(PathBuf::from(path));
    }
    if let Ok(resource_dir) = app.path().resource_dir() {
        paths.push(resource_dir.join("bin").join(&engine_name));
    }
    if let Ok(current_dir) = env::current_dir() {
        for build_root in [
            current_dir.join("../build/engine"),
            current_dir.join("build/engine"),
        ] {
            paths.push(build_root.join(&engine_name));
            paths.push(build_root.join("Debug").join(&engine_name));
            paths.push(build_root.join("Release").join(&engine_name));
        }
    }
    paths
}

fn find_engine(app: &AppHandle) -> Result<PathBuf, String> {
    let candidates = engine_candidates(app);
    candidates
        .iter()
        .find(|path| path.is_file())
        .cloned()
        .ok_or_else(|| {
            let searched = candidates
                .iter()
                .map(|path| path.display().to_string())
                .collect::<Vec<_>>()
                .join(", ");
            format!("getnative-engine was not found; searched: {searched}")
        })
}

fn run_engine(path: &Path, args: &[String]) -> Result<Value, String> {
    let output = Command::new(path)
        .args(args)
        .output()
        .map_err(|error| format!("failed to start getnative-engine: {error}"))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_owned();
        return Err(if stderr.is_empty() {
            format!("getnative-engine exited with {}", output.status)
        } else {
            stderr
        });
    }

    serde_json::from_slice(&output.stdout)
        .map_err(|error| format!("getnative-engine returned invalid JSON: {error}"))
}

fn validate_capabilities(payload: &Value) -> Result<(), String> {
    let capabilities: EngineCapabilities =
        serde_json::from_value(payload.clone()).map_err(|error| {
            format!("getnative-engine returned an invalid capability schema: {error}")
        })?;
    if capabilities.schema_version != 2 || capabilities.engine != "getnative-engine" {
        return Err("getnative-engine returned an unsupported capability schema".to_owned());
    }
    if !capabilities.commands.capabilities
        || !capabilities.commands.geometry
        || capabilities.commands.analyze
    {
        return Err("getnative-engine command availability is inconsistent".to_owned());
    }

    let kernel_ids = capabilities
        .kernels
        .iter()
        .map(|kernel| kernel.id.as_str())
        .collect::<Vec<_>>();
    if kernel_ids
        != [
            "bilinear", "bicubic", "lanczos", "spline16", "spline36", "spline64",
        ]
    {
        return Err("getnative-engine returned an unexpected kernel contract".to_owned());
    }
    for kernel in &capabilities.kernels {
        match (&*kernel.id, &kernel.parameters) {
            ("bilinear" | "spline16" | "spline36" | "spline64", KernelParameters::None) => {}
            ("bicubic", KernelParameters::BicubicBc { finite: true }) => {}
            (
                "lanczos",
                KernelParameters::IntegerTaps {
                    gui_min: 1,
                    gui_max: 8,
                    core_min: 1,
                    core_max: 15,
                },
            ) => {}
            _ => return Err("getnative-engine returned invalid kernel parameters".to_owned()),
        }
    }

    let mut backends = HashMap::new();
    for backend in &capabilities.backends {
        if !matches!(backend.id.as_str(), "cpu" | "metal" | "cuda") {
            return Err("getnative-engine returned an unknown backend id".to_owned());
        }
        if backends.insert(backend.id.as_str(), backend).is_some() {
            return Err("getnative-engine returned a duplicate backend id".to_owned());
        }
    }
    if backends.len() != 3 {
        return Err("getnative-engine did not return all required backends".to_owned());
    }
    let cpu = backends
        .get("cpu")
        .ok_or_else(|| "getnative-engine did not return the CPU backend".to_owned())?;
    if !cpu.compiled
        || !cpu.device_available
        || cpu.analysis_command_available
        || cpu.axes != ["horizontal", "vertical", "both"]
        || !matches!(
            cpu.p_norms,
            Some(PNormRange {
                minimum: 1,
                maximum: 4_294_967_295
            })
        )
        || cpu.max_half_bandwidth != Some(29)
        || cpu.max_forward_width != Some(30)
        || cpu.compiled_isa.as_deref().is_none_or(|values| {
            values.first().map(String::as_str) != Some("scalar")
                || values
                    .iter()
                    .any(|value| !matches!(value.as_str(), "scalar" | "sse2" | "avx2" | "avx512"))
        })
        || cpu.available_isa.as_deref().is_none_or(|values| {
            values.first().map(String::as_str) != Some("scalar")
                || values
                    .iter()
                    .any(|value| !matches!(value.as_str(), "scalar" | "sse2" | "avx2" | "avx512"))
        })
        || !matches!(
            cpu.selected_isa.as_deref(),
            Some("scalar" | "sse2" | "avx2" | "avx512")
        )
        || cpu
            .math_modes
            .as_deref()
            .is_none_or(|values| values.len() != 1 || values[0] != "production")
        || cpu.selected_math_mode.as_deref() != Some("production")
        || cpu.selection_reason.as_deref().is_none_or(str::is_empty)
    {
        return Err("getnative-engine returned invalid CPU capabilities".to_owned());
    }

    for id in ["metal", "cuda"] {
        let backend = backends
            .get(id)
            .ok_or_else(|| format!("getnative-engine did not return the {id} backend"))?;
        let valid_shape = if backend.compiled {
            backend.axes == ["horizontal", "vertical", "both"]
                && matches!(
                    backend.p_norms,
                    Some(PNormRange {
                        minimum: 1,
                        maximum: 1
                    })
                )
                && backend.max_half_bandwidth == Some(15)
                && backend.max_forward_width == Some(16)
                && (backend.device_available
                    || backend
                        .reason
                        .as_deref()
                        .is_some_and(|reason| !reason.is_empty()))
        } else {
            !backend.device_available
                && backend.axes.is_empty()
                && backend.p_norms.is_none()
                && backend.max_half_bandwidth.is_none()
                && backend.max_forward_width.is_none()
                && backend.reason.as_deref() == Some("not compiled")
        };
        // GPU backends use a single production math path; no multi-mode surface.
        let valid_math_mode = match id {
            "cuda" => backend.math_modes.is_none() && backend.selected_math_mode.is_none(),
            _ => true,
        };
        if backend.analysis_command_available
            || backend.device_available && !backend.compiled
            || !valid_shape
            || !valid_math_mode
        {
            return Err(format!(
                "getnative-engine returned invalid {id} capabilities"
            ));
        }
    }
    let profile_contract = capabilities
        .profiles
        .iter()
        .map(|profile| (profile.id.as_str(), profile.default_crop))
        .collect::<Vec<_>>();
    if profile_contract
        != [
            ("muf-d278cd3", 5),
            ("getfnative-44c8d0f", 10),
            ("modern", 5),
        ]
    {
        return Err("getnative-engine returned an unexpected profile contract".to_owned());
    }
    Ok(())
}

const INT64_EXCLUSIVE_UPPER_BOUND: f64 = 9_223_372_036_854_775_808.0;

fn checked_geometry_integer(value: f64, label: &str) -> Result<String, String> {
    if !value.is_finite()
        || value <= 0.0
        || value >= INT64_EXCLUSIVE_UPPER_BOUND
        || value.fract() != 0.0
    {
        return Err(format!(
            "{label} must be a positive integer within the engine range"
        ));
    }
    Ok(format!("{value:.0}"))
}

fn checked_geometry_number(value: f64, label: &str) -> Result<String, String> {
    if !value.is_finite() || value <= 0.0 || value >= INT64_EXCLUSIVE_UPPER_BOUND {
        return Err(format!(
            "{label} must be positive and within the engine range"
        ));
    }
    Ok(value.to_string())
}

#[tauri::command]
fn engine_capabilities(app: AppHandle) -> Result<Value, String> {
    let path = find_engine(&app)?;
    let payload = run_engine(&path, &["capabilities".to_owned()])?;
    validate_capabilities(&payload)?;
    Ok(json!({ "path": path, "payload": payload }))
}

#[tauri::command]
fn engine_geometry(app: AppHandle, request: GeometryRequest) -> Result<Value, String> {
    let args = geometry_args(request)?;
    let path = find_engine(&app)?;
    let payload = run_engine(&path, &args)?;
    Ok(json!({ "path": path, "payload": payload }))
}

fn geometry_args(request: GeometryRequest) -> Result<Vec<String>, String> {
    if !matches!(
        request.profile.as_str(),
        "muf-d278cd3" | "getfnative-44c8d0f" | "modern"
    ) {
        return Err("unknown compatibility profile".to_owned());
    }
    if !matches!(request.mode.as_str(), "standard" | "pro") {
        return Err("unknown geometry mode".to_owned());
    }

    let mut args = vec![
        "geometry".to_owned(),
        "--profile".to_owned(),
        request.profile,
        "--mode".to_owned(),
        request.mode.clone(),
        "--active-width".to_owned(),
        checked_geometry_number(request.active_width, "activeWidth")?,
        "--active-height".to_owned(),
        checked_geometry_number(request.active_height, "activeHeight")?,
    ];

    if request.mode == "standard" {
        args.extend([
            "--source-width".to_owned(),
            checked_geometry_integer(request.source_width, "sourceWidth")?,
            "--source-height".to_owned(),
            checked_geometry_integer(request.source_height, "sourceHeight")?,
        ]);
        if let Some(value) = request.base_height {
            args.extend([
                "--base-height".to_owned(),
                checked_geometry_integer(value, "baseHeight")?,
            ]);
        }
    } else {
        if let Some(value) = request.base_height {
            args.extend([
                "--base-height".to_owned(),
                checked_geometry_integer(value, "baseHeight")?,
            ]);
        }
        if let Some(value) = request.base_width {
            args.extend([
                "--base-width".to_owned(),
                checked_geometry_integer(value, "baseWidth")?,
            ]);
        }
    }

    Ok(args)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            engine_capabilities,
            engine_geometry
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::{
        checked_geometry_integer, checked_geometry_number, geometry_args, validate_capabilities,
        GeometryRequest,
    };
    use serde_json::json;

    fn valid_capabilities() -> serde_json::Value {
        json!({
            "schema_version": 2,
            "engine": "getnative-engine",
            "commands": {"capabilities": true, "geometry": true, "analyze": false},
            "kernels": [
                {"id": "bilinear", "parameters": {"kind": "none"}},
                {"id": "bicubic", "parameters": {"kind": "bicubic_bc", "finite": true}},
                {"id": "lanczos", "parameters": {"kind": "integer_taps", "gui_min": 1, "gui_max": 8, "core_min": 1, "core_max": 15}},
                {"id": "spline16", "parameters": {"kind": "none"}},
                {"id": "spline36", "parameters": {"kind": "none"}},
                {"id": "spline64", "parameters": {"kind": "none"}}
            ],
            "backends": [
                {"id": "cpu", "compiled": true, "device_available": true, "analysis_command_available": false, "axes": ["horizontal", "vertical", "both"], "p_norms": {"minimum": 1, "maximum": 4294967295_u64}, "max_half_bandwidth": 29, "max_forward_width": 30, "compiled_isa": ["scalar", "sse2", "avx2", "avx512"], "available_isa": ["scalar", "sse2", "avx2"], "selected_isa": "avx2", "math_modes": ["production"], "selected_math_mode": "production", "selection_reason": "avx512 not benchmark-approved"},
                {"id": "metal", "compiled": true, "device_available": true, "analysis_command_available": false, "axes": ["horizontal", "vertical", "both"], "p_norms": {"minimum": 1, "maximum": 1}, "max_half_bandwidth": 15, "max_forward_width": 16},
                {"id": "cuda", "compiled": false, "device_available": false, "analysis_command_available": false, "axes": [], "p_norms": null, "max_half_bandwidth": null, "max_forward_width": null, "reason": "not compiled"}
            ],
            "profiles": [
                {"id": "muf-d278cd3", "default_crop": 5},
                {"id": "getfnative-44c8d0f", "default_crop": 10},
                {"id": "modern", "default_crop": 5}
            ]
        })
    }

    #[test]
    fn geometry_arguments_preserve_fractions_within_the_engine_range() {
        assert_eq!(
            checked_geometry_number(1488.5, "activeWidth").unwrap(),
            "1488.5"
        );
        assert_eq!(
            checked_geometry_integer(1920.0, "sourceWidth").unwrap(),
            "1920"
        );
    }

    #[test]
    fn geometry_arguments_reject_invalid_or_unrepresentable_values() {
        let int64_upper_bound = 2_f64.powi(63);
        for value in [int64_upper_bound, 1e19, f64::NAN, f64::INFINITY, 0.0, -1.0] {
            assert!(checked_geometry_number(value, "activeWidth").is_err());
        }
        assert!(checked_geometry_integer(int64_upper_bound, "baseWidth").is_err());
        assert!(checked_geometry_integer(1488.5, "baseWidth").is_err());
    }

    fn geometry_request(mode: &str) -> GeometryRequest {
        GeometryRequest {
            profile: "modern".to_owned(),
            mode: mode.to_owned(),
            source_width: 1920.0,
            source_height: 1080.0,
            active_width: 1488.5,
            active_height: 837.25,
            base_height: Some(838.0),
            base_width: Some(1490.0),
        }
    }

    #[test]
    fn standard_geometry_ignores_base_width_but_validates_source_dimensions() {
        let mut request = geometry_request("standard");
        request.base_width = Some(f64::NAN);
        let args = geometry_args(request).unwrap();
        assert!(!args.iter().any(|argument| argument == "--base-width"));
        assert!(args.iter().any(|argument| argument == "--source-width"));

        let mut invalid_source = geometry_request("standard");
        invalid_source.source_width = f64::NAN;
        assert!(geometry_args(invalid_source).is_err());
    }

    #[test]
    fn pro_geometry_ignores_source_dimensions_but_validates_base_width() {
        let mut request = geometry_request("pro");
        request.source_width = f64::NAN;
        request.source_height = f64::INFINITY;
        let args = geometry_args(request).unwrap();
        assert!(!args.iter().any(|argument| argument == "--source-width"));
        assert!(!args.iter().any(|argument| argument == "--source-height"));
        assert!(args.iter().any(|argument| argument == "--base-width"));

        let mut invalid_base = geometry_request("pro");
        invalid_base.base_width = Some(f64::NAN);
        assert!(geometry_args(invalid_base).is_err());
    }

    #[test]
    fn capability_schema_distinguishes_build_device_and_command_availability() {
        assert!(validate_capabilities(&valid_capabilities()).is_ok());
    }

    #[test]
    fn capability_schema_rejects_false_analysis_and_inconsistent_gpu_claims() {
        let mut analysis = valid_capabilities();
        analysis["commands"]["analyze"] = json!(true);
        assert!(validate_capabilities(&analysis).is_err());

        let mut cuda = valid_capabilities();
        cuda["backends"][2]["compiled"] = json!(true);
        assert!(validate_capabilities(&cuda).is_err());

        let mut profiles = valid_capabilities();
        profiles["profiles"] = json!([]);
        assert!(validate_capabilities(&profiles).is_err());
    }

    #[test]
    fn capability_schema_maps_backends_by_id_and_rejects_duplicates_or_missing_ids() {
        let mut reordered = valid_capabilities();
        reordered["backends"].as_array_mut().unwrap().reverse();
        assert!(validate_capabilities(&reordered).is_ok());

        let mut no_device = valid_capabilities();
        no_device["backends"][2] = json!({
            "id": "cuda", "compiled": true, "device_available": false,
            "analysis_command_available": false,
            "axes": ["horizontal", "vertical", "both"],
            "p_norms": {"minimum": 1, "maximum": 1},
            "max_half_bandwidth": 15, "max_forward_width": 16,
            "reason": "CUDA driver is unavailable"
        });
        assert!(validate_capabilities(&no_device).is_ok());

        let mut invalid_math_mode = no_device.clone();
        invalid_math_mode["backends"][2]["math_modes"] =
            json!(["strict", "relaxed-fma", "fast-math"]);
        invalid_math_mode["backends"][2]["selected_math_mode"] = json!("strict");
        assert!(validate_capabilities(&invalid_math_mode).is_err());

        let mut duplicate = valid_capabilities();
        duplicate["backends"][1]["id"] = json!("cuda");
        assert!(validate_capabilities(&duplicate).is_err());

        let mut obsolete = valid_capabilities();
        obsolete["backends"][1]["id"] = json!("unsupported-backend");
        assert!(validate_capabilities(&obsolete).is_err());

        let mut missing = valid_capabilities();
        missing["backends"].as_array_mut().unwrap().pop();
        assert!(validate_capabilities(&missing).is_err());
    }
}

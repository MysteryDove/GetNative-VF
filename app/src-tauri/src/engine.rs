use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;
use tauri::{AppHandle, Manager};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GeometryRequest {
    pub profile: String,
    pub mode: String,
    pub source_width: f64,
    pub source_height: f64,
    pub active_width: f64,
    pub active_height: f64,
    pub base_height: Option<f64>,
    pub base_width: Option<f64>,
}

#[derive(Deserialize)]
struct CommandCapabilities {
    capabilities: bool,
    geometry: bool,
    analyze: bool,
    #[serde(default)]
    media_index_begin: bool,
    #[serde(default)]
    media_frame_window: bool,
    #[serde(default)]
    media_preview_begin: bool,
    #[serde(default)]
    media_asset_batch_begin: bool,
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

/// CUDA p_norm ceiling, shared with `worker::protocol` backend validation.
pub(crate) const CUDA_MAXIMUM_P_NORM: u64 = 4;

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
    device: Option<String>,
    #[serde(default)]
    device_type: Option<String>,
    #[serde(default)]
    auto_priority: Option<u32>,
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
    grid_semantics: String,
    default_grid: ProfileGridCapability,
    default_axis_mode: String,
    default_crop: u32,
    default_threshold: f64,
    threshold_comparison: String,
    default_kernel: ProfileKernelCapability,
}

#[derive(Deserialize)]
struct ProfileGridCapability {
    start: String,
    stop: String,
    step: String,
    endpoint_rule: String,
}

#[derive(Deserialize)]
struct ProfileKernelCapability {
    id: String,
    b: f64,
    c: f64,
    taps: u32,
}

#[derive(Deserialize, Default)]
struct FeatureCapabilities {
    #[serde(default)]
    verify_engine_decode: bool,
}

#[derive(Deserialize)]
struct DecodeBackendCapability {
    id: String,
    compiled: bool,
    runtime_device: bool,
    codecs: Vec<String>,
    zero_copy: bool,
    #[serde(default)]
    reason: Option<String>,
}

#[derive(Deserialize)]
struct MediaCapability {
    available: bool,
    ffmpeg_abi: Option<String>,
    index_version: Option<u32>,
    index_format: String,
}

#[derive(Deserialize)]
struct EngineCapabilities {
    schema_version: u32,
    engine: String,
    commands: CommandCapabilities,
    kernels: Vec<KernelCapability>,
    backends: Vec<BackendCapability>,
    profiles: Vec<ProfileCapability>,
    #[serde(default)]
    features: FeatureCapabilities,
    #[serde(default)]
    decode_backends: Vec<DecodeBackendCapability>,
    #[serde(default)]
    media: Option<MediaCapability>,
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

pub(crate) fn find_engine(app: &AppHandle) -> Result<PathBuf, String> {
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

/// Launch the console engine without allocating a visible Windows console.
pub(crate) fn engine_command(path: &Path) -> Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        let mut command = Command::new(path);
        command.creation_flags(CREATE_NO_WINDOW);
        command
    }
    #[cfg(not(windows))]
    {
        Command::new(path)
    }
}

fn run_engine(path: &Path, args: &[String]) -> Result<Value, String> {
    let output = engine_command(path)
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

pub(crate) fn validate_capabilities(payload: &Value) -> Result<(), String> {
    let capabilities: EngineCapabilities =
        serde_json::from_value(payload.clone()).map_err(|error| {
            format!("getnative-engine returned an invalid capability schema: {error}")
        })?;
    if capabilities.schema_version != 2 || capabilities.engine != "getnative-engine" {
        return Err("getnative-engine returned an unsupported capability schema".to_owned());
    }
    if !capabilities.decode_backends.is_empty() {
        let expected = ["software", "nvdec", "vulkan_video"];
        if capabilities.decode_backends.len() != expected.len()
            || capabilities
                .decode_backends
                .iter()
                .zip(expected)
                .any(|(backend, id)| {
                    backend.id != id
                        || backend.runtime_device && !backend.compiled
                        || backend.zero_copy && !backend.runtime_device
                        || backend.compiled && backend.codecs.is_empty()
                        || !backend.compiled
                            && backend.reason.as_deref().is_none_or(str::is_empty)
                })
        {
            return Err("getnative-engine returned invalid decode capabilities".to_owned());
        }
        let software = &capabilities.decode_backends[0];
        if capabilities.features.verify_engine_decode
            != (software.compiled && software.runtime_device)
        {
            return Err(
                "getnative-engine media decode feature is inconsistent".to_owned(),
            );
        }
    } else if capabilities.features.verify_engine_decode {
        return Err("getnative-engine omitted decode capabilities".to_owned());
    }
    if !capabilities.commands.capabilities || !capabilities.commands.geometry {
        return Err("getnative-engine command availability is inconsistent".to_owned());
    }
    let media_commands = [
        capabilities.commands.media_index_begin,
        capabilities.commands.media_frame_window,
        capabilities.commands.media_preview_begin,
        capabilities.commands.media_asset_batch_begin,
    ];
    let media_available = capabilities
        .media
        .as_ref()
        .is_some_and(|media| media.available);
    if media_commands.iter().any(|available| *available != media_available)
        || capabilities.features.verify_engine_decode != media_available
    {
        return Err("getnative-engine media command availability is inconsistent".to_owned());
    }
    if let Some(media) = &capabilities.media {
        if media.index_format != "lwi/vf.lwi"
            || (media.available
                && (media.ffmpeg_abi.as_deref().is_none_or(str::is_empty)
                    || media.index_version != Some(3)))
            || (!media.available
                && (media.ffmpeg_abi.is_some() || media.index_version.is_some()))
        {
            return Err("getnative-engine media capability schema is invalid".to_owned());
        }
    } else if media_available {
        return Err("getnative-engine omitted media capabilities".to_owned());
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
        if !matches!(backend.id.as_str(), "cpu" | "metal" | "cuda" | "vulkan") {
            return Err("getnative-engine returned an unknown backend id".to_owned());
        }
        if backends.insert(backend.id.as_str(), backend).is_some() {
            return Err("getnative-engine returned a duplicate backend id".to_owned());
        }
    }
    if backends.len() != 4 {
        return Err("getnative-engine did not return all required backends".to_owned());
    }
    let cpu = backends
        .get("cpu")
        .ok_or_else(|| "getnative-engine did not return the CPU backend".to_owned())?;
    if !cpu.compiled
        || !cpu.device_available
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
                    .any(|value| !matches!(value.as_str(), "scalar" | "sse2" | "avx2" | "avx512" | "neon"))
        })
        || cpu.available_isa.as_deref().is_none_or(|values| {
            values.first().map(String::as_str) != Some("scalar")
                || values
                    .iter()
                    .any(|value| !matches!(value.as_str(), "scalar" | "sse2" | "avx2" | "avx512" | "neon"))
        })
        || !matches!(
            cpu.selected_isa.as_deref(),
            Some("scalar" | "sse2" | "avx2" | "avx512" | "neon")
        )
        || cpu
            .math_modes
            .as_deref()
            .is_none_or(|values| values.len() != 1 || values[0] != "production")
        || cpu.selected_math_mode.as_deref() != Some("production")
        || cpu.selection_reason.as_deref().is_none_or(str::is_empty)
        || cpu.device_type.is_some()
        || cpu.auto_priority.is_some_and(|priority| priority != 100)
    {
        return Err("getnative-engine returned invalid CPU capabilities".to_owned());
    }

    for id in ["metal", "cuda", "vulkan"] {
        let backend = backends
            .get(id)
            .ok_or_else(|| format!("getnative-engine did not return the {id} backend"))?;
        let valid_shape = if backend.compiled {
            backend.axes == ["horizontal", "vertical", "both"]
                && backend.p_norms.as_ref().is_some_and(|range| {
                    range.minimum == 1
                        && match id {
                            "cuda" => (1..=CUDA_MAXIMUM_P_NORM).contains(&range.maximum),
                            "metal" | "vulkan" => range.maximum == 1,
                            _ => false,
                        }
                })
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
        let valid_device = !backend.device_available
            || backend.device.as_deref().is_some_and(|device| !device.is_empty());
        let valid_device_type = match id {
            "vulkan" => backend.device_type.as_deref().is_none_or(|device_type| {
                matches!(
                    device_type,
                    "discrete_gpu" | "integrated_gpu" | "virtual_gpu" | "cpu" | "other"
                )
            }),
            _ => backend.device_type.is_none(),
        };
        let valid_auto_priority = match id {
            "metal" => backend.auto_priority.is_none(),
            "cuda" => backend.auto_priority.is_none_or(|priority| {
                priority == 10 && backend.compiled && backend.device_available
            }),
            "vulkan" => backend.auto_priority.is_none_or(|priority| {
                priority == 20
                    && backend.compiled
                    && backend.device_available
                    && backend.device_type.as_deref() == Some("discrete_gpu")
            }),
            _ => false,
        };
        if backend.device_available && !backend.compiled
            || !valid_shape
            || !valid_math_mode
            || !valid_device
            || !valid_device_type
            || !valid_auto_priority
        {
            return Err(format!(
                "getnative-engine returned invalid {id} capabilities"
            ));
        }
    }
    if capabilities.backends.iter().any(|backend| {
        backend.analysis_command_available && (!backend.compiled || !backend.device_available)
    }) || capabilities.commands.analyze
        != capabilities
            .backends
            .iter()
            .any(|backend| backend.analysis_command_available)
    {
        return Err("getnative-engine analysis availability is inconsistent".to_owned());
    }
    let expected_profiles = [
        ("muf-d278cd3", "repeated_addition", "1", 5),
        ("getfnative-44c8d0f", "index_multiplication", "0.25", 10),
        ("modern", "decimal_fixed_point", "1", 5),
    ];
    if capabilities.profiles.len() != expected_profiles.len()
        || capabilities.profiles.iter().zip(expected_profiles).any(|(profile, expected)| {
            profile.id != expected.0
                || profile.grid_semantics != expected.1
                || profile.default_grid.start != "500"
                || profile.default_grid.stop != "1000"
                || profile.default_grid.step != expected.2
                || profile.default_grid.endpoint_rule != "inclusive"
                || profile.default_axis_mode != "h_plus_w"
                || profile.default_crop != expected.3
                || (profile.default_threshold - 0.015).abs() > f64::EPSILON
                || profile.threshold_comparison != "strict_greater_than"
                || profile.default_kernel.id != "bicubic"
                || profile.default_kernel.b != 0.0
                || profile.default_kernel.c != 0.5
                || profile.default_kernel.taps != 3
        })
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
pub async fn engine_capabilities(app: AppHandle) -> Result<Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let path = find_engine(&app)?;
        let payload = run_engine(&path, &["capabilities".to_owned()])?;
        validate_capabilities(&payload)?;
        Ok(json!({ "path": path, "payload": payload }))
    })
    .await
    .map_err(|error| format!("engine_task_error: {error}"))?
}

#[tauri::command]
pub async fn engine_geometry(app: AppHandle, request: GeometryRequest) -> Result<Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let args = geometry_args(request)?;
        let path = find_engine(&app)?;
        let payload = run_engine(&path, &args)?;
        Ok(json!({ "path": path, "payload": payload }))
    })
    .await
    .map_err(|error| format!("engine_task_error: {error}"))?
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
                {"id": "cpu", "compiled": true, "device_available": true, "analysis_command_available": false, "auto_priority": 100, "axes": ["horizontal", "vertical", "both"], "p_norms": {"minimum": 1, "maximum": 4294967295_u64}, "max_half_bandwidth": 29, "max_forward_width": 30, "compiled_isa": ["scalar", "sse2", "avx2", "avx512"], "available_isa": ["scalar", "sse2", "avx2"], "selected_isa": "avx2", "math_modes": ["production"], "selected_math_mode": "production", "selection_reason": "avx512 not benchmark-approved"},
                {"id": "metal", "compiled": true, "device_available": true, "analysis_command_available": false, "auto_priority": null, "device": "Apple GPU", "axes": ["horizontal", "vertical", "both"], "p_norms": {"minimum": 1, "maximum": 1}, "max_half_bandwidth": 15, "max_forward_width": 16},
                {"id": "cuda", "compiled": false, "device_available": false, "analysis_command_available": false, "auto_priority": null, "axes": [], "p_norms": null, "max_half_bandwidth": null, "max_forward_width": null, "reason": "not compiled"},
                {"id": "vulkan", "compiled": false, "device_available": false, "analysis_command_available": false, "auto_priority": null, "axes": [], "p_norms": null, "max_half_bandwidth": null, "max_forward_width": null, "reason": "not compiled"}
            ],
            "profiles": [
                {"id": "muf-d278cd3", "grid_semantics": "repeated_addition", "default_grid": {"start": "500", "stop": "1000", "step": "1", "endpoint_rule": "inclusive"}, "default_axis_mode": "h_plus_w", "default_crop": 5, "default_threshold": 0.015, "threshold_comparison": "strict_greater_than", "default_kernel": {"id": "bicubic", "b": 0.0, "c": 0.5, "taps": 3}},
                {"id": "getfnative-44c8d0f", "grid_semantics": "index_multiplication", "default_grid": {"start": "500", "stop": "1000", "step": "0.25", "endpoint_rule": "inclusive"}, "default_axis_mode": "h_plus_w", "default_crop": 10, "default_threshold": 0.015, "threshold_comparison": "strict_greater_than", "default_kernel": {"id": "bicubic", "b": 0.0, "c": 0.5, "taps": 3}},
                {"id": "modern", "grid_semantics": "decimal_fixed_point", "default_grid": {"start": "500", "stop": "1000", "step": "1", "endpoint_rule": "inclusive"}, "default_axis_mode": "h_plus_w", "default_crop": 5, "default_threshold": 0.015, "threshold_comparison": "strict_greater_than", "default_kernel": {"id": "bicubic", "b": 0.0, "c": 0.5, "taps": 3}}
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
    fn capability_schema_accepts_consistent_cpu_cuda_or_vulkan_analysis() {
        let mut cpu = valid_capabilities();
        cpu["commands"]["analyze"] = json!(true);
        cpu["backends"][0]["analysis_command_available"] = json!(true);
        assert!(validate_capabilities(&cpu).is_ok());

        let mut vulkan = valid_capabilities();
        vulkan["commands"]["analyze"] = json!(true);
        vulkan["backends"][3] = json!({
            "id": "vulkan", "compiled": true, "device_available": true,
            "analysis_command_available": true, "auto_priority": 20,
            "device": "Discrete GPU", "device_type": "discrete_gpu",
            "axes": ["horizontal", "vertical", "both"],
            "p_norms": {"minimum": 1, "maximum": 1},
            "max_half_bandwidth": 15, "max_forward_width": 16
        });
        assert!(validate_capabilities(&vulkan).is_ok());
    }

    #[test]
    fn capability_schema_rejects_inconsistent_analysis_or_cuda_claims() {
        let mut command_only = valid_capabilities();
        command_only["commands"]["analyze"] = json!(true);
        assert!(validate_capabilities(&command_only).is_err());

        let mut backend_only = valid_capabilities();
        backend_only["backends"][0]["analysis_command_available"] = json!(true);
        assert!(validate_capabilities(&backend_only).is_err());

        // Metal analyze is wired engine-side: analysis_command_available=true
        // is valid when consistent with commands.analyze, invalid without it.
        let mut metal_transport = valid_capabilities();
        metal_transport["commands"]["analyze"] = json!(true);
        metal_transport["backends"][1]["analysis_command_available"] = json!(true);
        assert!(validate_capabilities(&metal_transport).is_ok());

        let mut metal_inconsistent = valid_capabilities();
        metal_inconsistent["backends"][1]["analysis_command_available"] = json!(true);
        assert!(validate_capabilities(&metal_inconsistent).is_err());

        let mut cuda = valid_capabilities();
        cuda["backends"][2]["compiled"] = json!(true);
        assert!(validate_capabilities(&cuda).is_err());

        let mut profiles = valid_capabilities();
        profiles["profiles"] = json!([]);
        assert!(validate_capabilities(&profiles).is_err());
    }

    #[test]
    fn capability_schema_accepts_aarch64_neon_cpu_reporting() {
        // The engine reports its real ISA set on ARM instead of the x86
        // evaluator's misleading scalar-only fallback.
        let mut arm = valid_capabilities();
        arm["backends"][0] = json!({
            "id": "cpu", "compiled": true, "device_available": true,
            "analysis_command_available": false, "auto_priority": 100,
            "axes": ["horizontal", "vertical", "both"],
            "p_norms": {"minimum": 1, "maximum": 4294967295_u64},
            "max_half_bandwidth": 29, "max_forward_width": 30,
            "compiled_isa": ["scalar", "neon"], "available_isa": ["scalar", "neon"],
            "selected_isa": "neon", "math_modes": ["production"],
            "selected_math_mode": "production",
            "selection_reason": "AArch64 baseline NEON"
        });
        assert!(validate_capabilities(&arm).is_ok());
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
            "p_norms": {"minimum": 1, "maximum": 4},
            "max_half_bandwidth": 15, "max_forward_width": 16,
            "auto_priority": null,
            "reason": "CUDA driver is unavailable"
        });
        assert!(validate_capabilities(&no_device).is_ok());

        let mut legacy_cuda = no_device.clone();
        legacy_cuda["backends"][2]["p_norms"]["maximum"] = json!(1);
        assert!(validate_capabilities(&legacy_cuda).is_ok());

        let mut invalid_math_mode = no_device.clone();
        invalid_math_mode["backends"][2]["math_modes"] =
            json!(["strict", "relaxed-fma", "fast-math"]);
        invalid_math_mode["backends"][2]["selected_math_mode"] = json!("strict");
        assert!(validate_capabilities(&invalid_math_mode).is_err());

        let mut integrated_auto = valid_capabilities();
        integrated_auto["backends"][3] = json!({
            "id": "vulkan", "compiled": true, "device_available": true,
            "analysis_command_available": false, "auto_priority": 20,
            "device": "Integrated GPU", "device_type": "integrated_gpu",
            "axes": ["horizontal", "vertical", "both"],
            "p_norms": {"minimum": 1, "maximum": 1},
            "max_half_bandwidth": 15, "max_forward_width": 16
        });
        assert!(validate_capabilities(&integrated_auto).is_err());

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

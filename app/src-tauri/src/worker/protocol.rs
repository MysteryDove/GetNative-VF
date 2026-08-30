//! Worker protocol request shapes, validation, and command serialization.
//!
//! Wire field names follow `docs/worker-protocol-v1.md` (snake_case); the
//! Tauri command boundary receives camelCase and this module owns the
//! mapping plus every contract check shared by the analyze and verify
//! commands.

use crate::engine::CUDA_MAXIMUM_P_NORM;
use serde::Deserialize;
use serde_json::{json, Map, Value};
use std::path::Path;

use super::PROTOCOL_VERSION;

const MAX_FRAME_AXIS: u32 = 65_536;
const MAX_CANDIDATES: usize = 100_000;

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FrameAssetRef {
    pub path: String,
    pub format: String,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KernelCommand {
    pub id: String,
    pub b: Option<f64>,
    pub c: Option<f64>,
    pub taps: Option<u32>,
    pub blur: Option<f64>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MetricCommand {
    pub crop_left: Option<u32>,
    pub crop_right: Option<u32>,
    pub crop_top: Option<u32>,
    pub crop_bottom: Option<u32>,
    pub threshold: Option<f64>,
    pub p_norm: Option<u32>,
}

pub(crate) fn default_profile_id() -> String {
    "muf-d278cd3".to_owned()
}

pub(crate) fn default_endpoint_rule() -> String {
    "inclusive".to_owned()
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CandidateGridCommand {
    pub start: String,
    pub stop: String,
    pub step: String,
}

#[derive(Debug, Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct GeometryCommand {
    pub width: u32,
    pub height: u32,
    pub src_left: f64,
    pub src_top: f64,
    pub src_width: f64,
    pub src_height: f64,
    pub base_width: Option<u32>,
    pub base_height: Option<u32>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkerAnalyzeRequest {
    pub request_id: String,
    pub mode: String,
    pub frame_asset: FrameAssetRef,
    pub axis_mode: String,
    /// Height mode: the single scan kernel. Kernel mode: omit.
    pub kernel: Option<KernelCommand>,
    /// Kernel mode: the ordered kernel list (fixed geometry from
    /// `candidates[0]`). Height mode: omit.
    pub kernels: Option<Vec<KernelCommand>>,
    pub candidates: Vec<String>,
    pub metric: MetricCommand,
    pub backend: String,
    pub worker_count: Option<u32>,
    #[serde(default = "default_profile_id")]
    pub profile_id: String,
    #[serde(default = "default_endpoint_rule")]
    pub endpoint_rule: String,
    pub base_height: Option<String>,
    pub base_width: Option<String>,
    pub grid: Option<CandidateGridCommand>,
    pub geometry: Option<GeometryCommand>,
}

fn validate_kernel_command(kernel: &KernelCommand) -> Result<(), String> {
    if kernel
        .blur
        .is_some_and(|blur| !blur.is_finite() || blur <= 0.0)
    {
        return Err("bad_request: blur must be finite and greater than zero".to_owned());
    }
    match kernel.id.as_str() {
        "bilinear" | "spline16" | "spline36" | "spline64" => {}
        "bicubic" => {
            if kernel.b.is_none() || kernel.c.is_none() {
                return Err("bad_request: bicubic requires explicit b and c".to_owned());
            }
            for (label, value) in [("b", kernel.b), ("c", kernel.c)] {
                if value.is_some_and(|value| !value.is_finite()) {
                    return Err(format!("bad_request: bicubic {label} must be finite"));
                }
            }
        }
        "lanczos" => {
            if kernel.taps.is_none() {
                return Err("bad_request: lanczos requires explicit taps".to_owned());
            }
            if kernel.taps.is_some_and(|taps| !(1..=15).contains(&taps)) {
                return Err("bad_request: lanczos taps must be within 1..=15".to_owned());
            }
        }
        other => return Err(format!("bad_request: unknown kernel id {other}")),
    }
    Ok(())
}

/// Frame geometry range check shared by the analyze frame asset and the
/// media verify geometry; `label` keeps each call site's error wording.
fn validate_frame_geometry(width: u32, height: u32, label: &str) -> Result<(), String> {
    if width < 2 || height < 2 || width > MAX_FRAME_AXIS || height > MAX_FRAME_AXIS {
        return Err(format!(
            "bad_request: {label} must be within 2..={MAX_FRAME_AXIS}"
        ));
    }
    Ok(())
}

fn validate_geometry(geometry: &GeometryCommand, label: &str) -> Result<(), String> {
    validate_frame_geometry(geometry.width, geometry.height, label)?;
    for (name, value) in [
        ("srcLeft", geometry.src_left),
        ("srcTop", geometry.src_top),
        ("srcWidth", geometry.src_width),
        ("srcHeight", geometry.src_height),
    ] {
        if !value.is_finite() || (name == "srcWidth" || name == "srcHeight") && value <= 0.0 {
            return Err(format!(
                "bad_request: geometry {name} must be finite and positive"
            ));
        }
    }
    if geometry.src_left < 0.0
        || geometry.src_top < 0.0
        || geometry.src_left + geometry.src_width > geometry.width as f64 + 1e-9
        || geometry.src_top + geometry.src_height > geometry.height as f64 + 1e-9
    {
        return Err("bad_request: geometry source rectangle exceeds canvas".to_owned());
    }
    if geometry
        .base_width
        .is_some_and(|value| value == 0 || value > MAX_FRAME_AXIS)
        || geometry
            .base_height
            .is_some_and(|value| value == 0 || value > MAX_FRAME_AXIS)
    {
        return Err("bad_request: geometry base dimensions must be within 1..=65536".to_owned());
    }
    Ok(())
}

/// Backend + p_norm rule set shared by analyze and media verify. `unknown_message`
/// and `scope` preserve each
/// call site's error wording (scope is "" for analyze, " verify" for verify).
fn validate_backend(
    backend: &str,
    p_norm: Option<u32>,
    allow_metal: bool,
    unknown_message: impl FnOnce(&str) -> String,
    scope: &str,
) -> Result<(), String> {
    if !(matches!(backend, "cpu" | "cuda" | "vulkan" | "auto") || allow_metal && backend == "metal")
    {
        return Err(unknown_message(backend));
    }
    let p_norm = u64::from(p_norm.unwrap_or(1));
    if backend == "cuda" && p_norm > CUDA_MAXIMUM_P_NORM {
        return Err(format!(
            "unsupported: CUDA{scope} only supports p_norm in 1..4"
        ));
    }
    if backend == "vulkan" && !(1..=4).contains(&p_norm) {
        return Err(format!(
            "unsupported: Vulkan{scope} only supports p_norm in 1..4"
        ));
    }
    if backend == "metal" && !(1..=4).contains(&p_norm) {
        return Err(format!(
            "unsupported: Metal{scope} only supports p_norm in 1..4"
        ));
    }
    Ok(())
}

pub(crate) fn validate_analyze(request: &WorkerAnalyzeRequest) -> Result<(), String> {
    if request.request_id.trim().is_empty() {
        return Err("bad_request: requestId must not be empty".to_owned());
    }
    if !matches!(request.mode.as_str(), "height" | "kernel") {
        return Err(format!(
            "unsupported: mode must be height or kernel in worker protocol v1.1, got {}",
            request.mode
        ));
    }
    let asset = &request.frame_asset;
    if asset.path.trim().is_empty() {
        return Err("bad_request: frameAsset.path must not be empty".to_owned());
    }
    if asset.format != "f32le" {
        return Err(format!(
            "unsupported: frame asset format must be f32le in worker protocol v1, got {}",
            asset.format
        ));
    }
    validate_frame_geometry(asset.width, asset.height, "frame asset dimensions")?;
    if let Some(geometry) = request.geometry.as_ref() {
        validate_geometry(geometry, "geometry canvas")?;
    }
    if !matches!(request.axis_mode.as_str(), "h_only" | "w_only" | "h_plus_w") {
        return Err(format!(
            "bad_request: unknown axisMode {}",
            request.axis_mode
        ));
    }
    if !matches!(
        request.profile_id.as_str(),
        "muf-d278cd3" | "getfnative-44c8d0f" | "modern"
    ) {
        return Err(format!(
            "bad_request: unknown profileId {}",
            request.profile_id
        ));
    }
    if !matches!(
        request.endpoint_rule.as_str(),
        "inclusive" | "exclusive_stop"
    ) {
        return Err(format!(
            "bad_request: unknown endpointRule {}",
            request.endpoint_rule
        ));
    }
    for (label, value) in [
        ("baseHeight", request.base_height.as_deref()),
        ("baseWidth", request.base_width.as_deref()),
    ] {
        if let Some(value) = value {
            let parsed = value
                .parse::<u32>()
                .map_err(|_| format!("bad_request: {label} must be a positive integer decimal"))?;
            if parsed == 0 || parsed > MAX_FRAME_AXIS {
                return Err(format!(
                    "bad_request: {label} must be within 1..={MAX_FRAME_AXIS}"
                ));
            }
        }
    }
    if request.mode == "kernel" {
        if request.kernel.is_some() {
            return Err("bad_request: kernel mode takes kernels, not kernel".to_owned());
        }
        let kernels = request.kernels.as_ref().filter(|list| !list.is_empty());
        let Some(kernels) = kernels else {
            return Err("bad_request: kernel mode requires a non-empty kernels list".to_owned());
        };
        if kernels.len() > MAX_CANDIDATES {
            return Err(format!(
                "bad_request: kernels must contain 1..={MAX_CANDIDATES} entries"
            ));
        }
        for kernel in kernels {
            validate_kernel_command(kernel)?;
        }
        if request.candidates.len() != 1 {
            return Err(
                "bad_request: kernel mode takes exactly one candidate (the fixed axis value)"
                    .to_owned(),
            );
        }
    } else {
        if request.kernels.is_some() {
            return Err("bad_request: height mode takes kernel, not kernels".to_owned());
        }
        let Some(kernel) = request.kernel.as_ref() else {
            return Err("bad_request: height mode requires kernel".to_owned());
        };
        validate_kernel_command(kernel)?;
        if request.candidates.is_empty() || request.candidates.len() > MAX_CANDIDATES {
            return Err(format!(
                "bad_request: candidates must contain 1..={MAX_CANDIDATES} values"
            ));
        }
        if let Some(grid) = &request.grid {
            let mut step = None;
            for (label, value) in [
                ("grid.start", grid.start.as_str()),
                ("grid.stop", grid.stop.as_str()),
                ("grid.step", grid.step.as_str()),
            ] {
                let parsed = value
                    .parse::<f64>()
                    .map_err(|_| format!("bad_request: {label} must be a decimal"))?;
                if !parsed.is_finite() {
                    return Err(format!("bad_request: {label} must be finite"));
                }
                if label == "grid.step" {
                    step = Some(parsed);
                }
            }
            // The loop above already rejected an unparseable step.
            if step.unwrap_or(0.0) <= 0.0 {
                return Err("bad_request: grid.step must be positive".to_owned());
            }
        }
    }
    for candidate in &request.candidates {
        let Ok(value) = candidate.parse::<f64>() else {
            return Err(format!(
                "bad_request: candidate {candidate:?} is not a decimal"
            ));
        };
        if !value.is_finite() || value < 2.0 {
            return Err(format!(
                "bad_request: candidate {candidate:?} must be finite and >= 2"
            ));
        }
    }
    if request.metric.p_norm == Some(0) {
        return Err("bad_request: p_norm must be a positive integer".to_owned());
    }
    if request
        .metric
        .threshold
        .is_some_and(|threshold| !threshold.is_finite() || threshold < 0.0)
    {
        return Err("bad_request: threshold must be finite and non-negative".to_owned());
    }
    validate_backend(
        &request.backend,
        request.metric.p_norm,
        true,
        |backend| {
            format!(
                "unsupported: backend must be one of cpu/cuda/vulkan/metal/auto in worker protocol v1, got {backend}"
            )
        },
        "",
    )?;
    Ok(())
}

pub(crate) fn kernel_json(kernel: &KernelCommand) -> Value {
    let mut object = Map::new();
    object.insert("id".to_owned(), json!(kernel.id));
    if kernel.id == "bicubic" {
        if let Some(b) = kernel.b {
            object.insert("b".to_owned(), json!(b));
        }
        if let Some(c) = kernel.c {
            object.insert("c".to_owned(), json!(c));
        }
    }
    if kernel.id == "lanczos" {
        if let Some(taps) = kernel.taps {
            object.insert("taps".to_owned(), json!(taps));
        }
    }
    if let Some(blur) = kernel.blur {
        object.insert("blur".to_owned(), json!(blur));
    }
    Value::Object(object)
}

pub(crate) fn metric_json(metric: &MetricCommand) -> Value {
    let mut result = Map::new();
    for (key, value) in [
        ("crop_left", metric.crop_left),
        ("crop_right", metric.crop_right),
        ("crop_top", metric.crop_top),
        ("crop_bottom", metric.crop_bottom),
    ] {
        if let Some(value) = value {
            result.insert(key.to_owned(), json!(value));
        }
    }
    if let Some(value) = metric.threshold {
        result.insert("threshold".to_owned(), json!(value));
    }
    if let Some(value) = metric.p_norm {
        result.insert("p_norm".to_owned(), json!(value));
    }
    Value::Object(result)
}

pub(crate) fn analyze_command(request: &WorkerAnalyzeRequest) -> Result<Value, String> {
    let mut command = json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "analyze",
        "request_id": request.request_id,
        "mode": request.mode,
        "frame_asset": {
            "path": request.frame_asset.path,
            "format": request.frame_asset.format,
            "width": request.frame_asset.width,
            "height": request.frame_asset.height,
        },
        "axis_mode": request.axis_mode,
        "profile_id": request.profile_id,
        "endpoint_rule": request.endpoint_rule,
        "base_height": request.base_height,
        "base_width": request.base_width,
        "metric": metric_json(&request.metric),
        "backend": request.backend,
    });
    if let Some(geometry) = &request.geometry {
        command["geometry"] = json!({
            "width": geometry.width,
            "height": geometry.height,
            "src_left": geometry.src_left,
            "src_top": geometry.src_top,
            "src_width": geometry.src_width,
            "src_height": geometry.src_height,
            "base_width": geometry.base_width,
            "base_height": geometry.base_height,
        });
    }
    if request.mode == "kernel" {
        // Kernel mode: the single fixed axis value travels as `candidate`
        // and the ordered kernel list as `kernels` (engine protocol v1.1).
        command["candidate"] = json!(request.candidates[0]);
        command["kernels"] = Value::Array(
            request
                .kernels
                .as_ref()
                .map(|kernels| kernels.iter().map(kernel_json).collect())
                .unwrap_or_default(),
        );
    } else {
        let kernel = request
            .kernel
            .as_ref()
            .ok_or_else(|| "bad_request: height mode requires kernel".to_owned())?;
        command["kernel"] = kernel_json(kernel);
        command["candidates"] = json!(request.candidates);
        if let Some(grid) = &request.grid {
            command["grid"] = json!({
                "start": grid.start,
                "stop": grid.stop,
                "step": grid.step,
            });
        }
    }
    if let Some(worker_count) = request.worker_count {
        command["worker_count"] = json!(worker_count);
    }
    Ok(command)
}

// ---------------------------------------------------------------------------
// Verify streaming (worker protocol v1.1)
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifyMediaBeginRequest {
    pub request_id: String,
    pub path: String,
    pub fingerprint: Option<String>,
    pub stream_index: u32,
    pub width: u32,
    pub height: u32,
    pub selection: String,
    pub every_n: Option<u64>,
    pub start_frame: Option<u64>,
    pub end_frame: Option<u64>,
    pub axis_mode: String,
    pub kernel: KernelCommand,
    pub candidate: String,
    pub metric: MetricCommand,
    pub backend: String,
    #[serde(default = "default_media_verify_concurrency")]
    pub concurrency: u32,
    pub geometry: Option<GeometryCommand>,
}

fn default_media_verify_concurrency() -> u32 {
    2
}

pub(crate) fn validate_verify_media_begin(request: &VerifyMediaBeginRequest) -> Result<(), String> {
    if request.request_id.trim().is_empty() || request.path.trim().is_empty() {
        return Err("bad_request: requestId and path must not be empty".to_owned());
    }
    validate_frame_geometry(request.width, request.height, "verify geometry")?;
    if let Some(geometry) = request.geometry.as_ref() {
        validate_geometry(geometry, "verify geometry canvas")?;
    }
    if !matches!(request.axis_mode.as_str(), "h_only" | "w_only" | "h_plus_w") {
        return Err(format!(
            "bad_request: unknown axisMode {}",
            request.axis_mode
        ));
    }
    validate_kernel_command(&request.kernel)?;
    let candidate = request
        .candidate
        .parse::<f64>()
        .map_err(|_| "bad_request: candidate must be a decimal".to_owned())?;
    if !candidate.is_finite() || candidate < 2.0 {
        return Err("bad_request: candidate must be finite and >= 2".to_owned());
    }
    validate_backend(
        &request.backend,
        request.metric.p_norm,
        true,
        |backend| {
            format!("unsupported: media verify backend must be cpu/cuda/vulkan/metal/auto, got {backend}")
        },
        " verify",
    )?;
    if !(1..=8).contains(&request.concurrency) {
        return Err("bad_request: concurrency must be within 1..=8".to_owned());
    }
    match request.selection.as_str() {
        "all" | "decoded_i_picture" => {}
        "every_n" if request.every_n.is_some_and(|value| value >= 1) => {}
        "every_n" => return Err("bad_request: every-N selection requires everyN >= 1".to_owned()),
        other => return Err(format!("bad_request: unknown selection rule {other}")),
    }
    if request
        .start_frame
        .zip(request.end_frame)
        .is_some_and(|(start, end)| start > end)
    {
        return Err("bad_request: range start must be <= end".to_owned());
    }
    Ok(())
}

pub(crate) fn verify_media_begin_command(
    request: &VerifyMediaBeginRequest,
    cache_directory: &Path,
) -> Value {
    json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "verify_media_begin",
        "request_id": request.request_id,
        "media": {
            "path": request.path,
            "fingerprint": request.fingerprint,
            "stream_index": request.stream_index,
            "cache_directory": cache_directory,
        },
        "geometry": { "width": request.width, "height": request.height },
        "scan_scope": {
            "selection": request.selection,
            "every_n": request.every_n,
            "start_frame": request.start_frame,
            "end_frame": request.end_frame,
        },
        "axis_mode": request.axis_mode,
        "kernel": kernel_json(&request.kernel),
        "candidate": request.candidate,
        "metric": metric_json(&request.metric),
        "backend": request.backend,
        "concurrency": request.concurrency,
        "resolved_geometry": request.geometry.as_ref().map(|geometry| json!({
            "width": geometry.width,
            "height": geometry.height,
            "src_left": geometry.src_left,
            "src_top": geometry.src_top,
            "src_width": geometry.src_width,
            "src_height": geometry.src_height,
            "base_width": geometry.base_width,
            "base_height": geometry.base_height,
        })),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn analyze_request() -> WorkerAnalyzeRequest {
        serde_json::from_value(json!({
            "requestId": "req-1",
            "mode": "height",
            "frameAsset": {
                "path": "/tmp/frame.f32",
                "format": "f32le",
                "width": 320,
                "height": 240,
            },
            "axisMode": "h_only",
            "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
            "candidates": ["230", "231.5"],
            "profileId": "getfnative-44c8d0f",
            "endpointRule": "exclusive_stop",
            "baseHeight": "241",
            "baseWidth": "321",
            "grid": {"start": "230", "stop": "233", "step": "1.5"},
            "metric": {"cropLeft": 10, "threshold": 0.015, "pNorm": 1},
            "backend": "cpu",
        }))
        .unwrap()
    }

    #[test]
    fn analyze_command_serializes_the_wire_shape() {
        let command = analyze_command(&analyze_request()).unwrap();
        assert_eq!(command["protocol_version"], json!(1));
        assert_eq!(command["type"], json!("analyze"));
        assert_eq!(command["request_id"], json!("req-1"));
        assert_eq!(command["frame_asset"]["format"], json!("f32le"));
        assert_eq!(
            command["kernel"],
            json!({"id": "bicubic", "b": 0.0, "c": 0.5})
        );
        assert_eq!(command["metric"]["p_norm"], json!(1));
        assert_eq!(command["profile_id"], json!("getfnative-44c8d0f"));
        assert_eq!(command["endpoint_rule"], json!("exclusive_stop"));
        assert_eq!(command["base_height"], json!("241"));
        assert_eq!(command["base_width"], json!("321"));
        assert_eq!(
            command["grid"],
            json!({"start": "230", "stop": "233", "step": "1.5"})
        );
        assert!(command.get("worker_count").is_none());
    }

    #[test]
    fn analyze_command_omits_irrelevant_kernel_parameters() {
        let mut request = analyze_request();
        request.kernel = Some(KernelCommand {
            id: "lanczos".to_owned(),
            b: Some(9.0),
            c: Some(9.0),
            taps: Some(3),
            blur: None,
        });
        let command = analyze_command(&request).unwrap();
        assert_eq!(command["kernel"], json!({"id": "lanczos", "taps": 3}));
    }

    #[test]
    fn kernel_mode_serializes_candidate_and_kernel_list() {
        let request: WorkerAnalyzeRequest = serde_json::from_value(json!({
            "requestId": "req-k1",
            "mode": "kernel",
            "frameAsset": {
                "path": "/tmp/frame.f32",
                "format": "f32le",
                "width": 320,
                "height": 240,
            },
            "axisMode": "h_only",
            "kernels": [
                {"id": "bicubic", "b": 0.0, "c": 0.5},
                {"id": "lanczos", "taps": 3},
            ],
            "candidates": ["200"],
            "metric": {"pNorm": 1},
            "backend": "cpu",
        }))
        .unwrap();
        validate_analyze(&request).unwrap();
        let command = analyze_command(&request).unwrap();
        assert_eq!(command["mode"], json!("kernel"));
        assert_eq!(command["candidate"], json!("200"));
        assert_eq!(
            command["kernels"],
            json!([{"id": "bicubic", "b": 0.0, "c": 0.5}, {"id": "lanczos", "taps": 3}])
        );
        assert!(command.get("kernel").is_none());
        assert!(command.get("candidates").is_none());
    }

    #[test]
    fn kernel_mode_validation_rejects_bad_shapes() {
        let valid = json!({
            "requestId": "req-k2",
            "mode": "kernel",
            "frameAsset": {
                "path": "/tmp/frame.f32",
                "format": "f32le",
                "width": 320,
                "height": 240,
            },
            "axisMode": "h_only",
            "kernels": [{"id": "bicubic", "b": 0.0, "c": 0.5}],
            "candidates": ["200"],
            "metric": {"pNorm": 1},
            "backend": "cpu",
        });

        let mut value = valid.clone();
        value["candidates"] = json!(["200", "201"]);
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());

        let mut value = valid.clone();
        value["kernels"] = json!([]);
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());

        let mut value = valid.clone();
        value["kernel"] = json!({"id": "bicubic"});
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());

        let mut value = valid;
        value["kernels"] = json!([{"id": "lanczos", "taps": 16}]);
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());
    }

    #[test]
    fn analyze_validation_rejects_out_of_contract_shapes() {
        let mut request = analyze_request();
        request.mode = "width".to_owned();
        assert!(validate_analyze(&request)
            .unwrap_err()
            .contains("unsupported"));

        let mut request = analyze_request();
        request.frame_asset.format = "f64le".to_owned();
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.frame_asset.width = 1;
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.backend = "cuda".to_owned();
        assert!(validate_analyze(&request).is_ok());

        let mut request = analyze_request();
        request.backend = "auto".to_owned();
        assert!(validate_analyze(&request).is_ok());

        let mut request = analyze_request();
        request.backend = "vulkan".to_owned();
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(4);
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(5);
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.backend = "metal".to_owned();
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(4);
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(5);
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.candidates = vec!["1.5".to_owned()];
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.candidates = vec!["not-a-number".to_owned()];
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.metric.p_norm = Some(4);
        assert!(validate_analyze(&request).is_ok());
        request.backend = "cuda".to_owned();
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(5);
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        if let Some(kernel) = request.kernel.as_mut() {
            kernel.taps = Some(16);
            kernel.id = "lanczos".to_owned();
        }
        assert!(validate_analyze(&request).is_err());
    }

    #[test]
    fn analyze_command_validates_and_serializes_blur() {
        let mut request = analyze_request();
        request.kernel.as_mut().unwrap().blur = Some(1.25);
        let command = analyze_command(&request).unwrap();
        assert_eq!(command["kernel"]["blur"], json!(1.25));

        request.kernel.as_mut().unwrap().blur = Some(0.0);
        assert!(validate_analyze(&request).is_err());
        request.kernel.as_mut().unwrap().blur = Some(f64::NAN);
        assert!(validate_analyze(&request).is_err());
    }

    #[test]
    fn media_verify_concurrency_defaults_and_validates() {
        let value = json!({
            "requestId": "verify-media-1",
            "path": "/tmp/video.mkv",
            "fingerprint": null,
            "streamIndex": 0,
            "width": 320,
            "height": 240,
            "selection": "all",
            "everyN": null,
            "startFrame": null,
            "endFrame": null,
            "axisMode": "h_only",
            "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
            "candidate": "200",
            "metric": {"pNorm": 1},
            "backend": "cpu"
        });
        let request: VerifyMediaBeginRequest = serde_json::from_value(value.clone()).unwrap();
        assert_eq!(request.concurrency, 2);
        assert!(validate_verify_media_begin(&request).is_ok());

        let mut blurred = value.clone();
        blurred["kernel"]["blur"] = json!(1.25);
        let request: VerifyMediaBeginRequest = serde_json::from_value(blurred).unwrap();
        assert!(validate_verify_media_begin(&request).is_ok());
        assert_eq!(
            verify_media_begin_command(&request, Path::new("/tmp/cache"))["kernel"]["blur"],
            json!(1.25)
        );

        let mut metal = value.clone();
        metal["backend"] = json!("metal");
        let request: VerifyMediaBeginRequest = serde_json::from_value(metal).unwrap();
        assert!(validate_verify_media_begin(&request).is_ok());

        for concurrency in 1..=8 {
            let mut accepted = value.clone();
            accepted["concurrency"] = json!(concurrency);
            let request: VerifyMediaBeginRequest = serde_json::from_value(accepted).unwrap();
            assert!(validate_verify_media_begin(&request).is_ok());
        }
        for concurrency in [0, 9] {
            let mut rejected = value.clone();
            rejected["concurrency"] = json!(concurrency);
            let request: VerifyMediaBeginRequest = serde_json::from_value(rejected).unwrap();
            assert!(validate_verify_media_begin(&request).is_err());
        }
        for invalid in [json!(-1), json!(1.5), json!("two")] {
            let mut rejected = value.clone();
            rejected["concurrency"] = invalid;
            assert!(serde_json::from_value::<VerifyMediaBeginRequest>(rejected).is_err());
        }
    }
}

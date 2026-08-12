import type {
  HeightAnalyzeRequest,
  KernelAnalyzeRequest,
  MetricSpec,
  VerifyRequest,
} from "./protocol";

export type ShapeGuardResult =
  | { ok: true }
  | { ok: false; code: string; message: string };

function fail(code: string, message: string): ShapeGuardResult {
  return { ok: false, code, message };
}

export function validateMetricSpec(metric: MetricSpec): ShapeGuardResult {
  const crops = [metric.cropLeft, metric.cropRight, metric.cropTop, metric.cropBottom];
  if (crops.some((value) => !Number.isInteger(value) || value < 0)) {
    return fail("metric_crop_invalid", "Metric crop edges must be non-negative integers");
  }
  if (!Number.isFinite(metric.pixelExclusionThreshold) || metric.pixelExclusionThreshold < 0) {
    return fail(
      "metric_exclusion_invalid",
      "Pixel Exclusion Threshold must be a finite non-negative number",
    );
  }
  if (!Number.isInteger(metric.pNorm) || metric.pNorm < 1) {
    return fail("metric_p_norm_invalid", "p-norm must be a positive integer");
  }
  return { ok: true };
}

/**
 * Height Run: exactly one Sample, one fixed kernel, many heights.
 * Multi-Sample or multi-kernel user commands must create a RunGroup of member Runs.
 */
export function validateHeightShape(request: HeightAnalyzeRequest): ShapeGuardResult {
  if (request.mode !== "height") {
    return fail("height_mode_mismatch", "Height shape requires mode=height");
  }
  if (!request.sampleId) {
    return fail("height_sample_required", "Height Run requires exactly one Sample");
  }
  if (!request.kernel?.id) {
    return fail("height_kernel_required", "Height Run requires one fixed kernel");
  }
  if (!request.heightGrid?.candidates?.length) {
    return fail("height_grid_empty", "Height Run requires a non-empty height candidate grid");
  }
  if (request.heightGrid.candidates.length < 2) {
    return fail("height_grid_too_small", "Height search requires at least two candidate heights");
  }
  const metric = validateMetricSpec(request.metric);
  if (!metric.ok) return metric;
  if (!request.profileId) {
    return fail("height_profile_required", "Compatibility profile is required");
  }
  return { ok: true };
}

/**
 * Kernel Run: exactly one Sample, one fixed geometry, many kernels.
 */
export function validateKernelShape(request: KernelAnalyzeRequest): ShapeGuardResult {
  if (request.mode !== "kernel") {
    return fail("kernel_mode_mismatch", "Kernel shape requires mode=kernel");
  }
  if (!request.sampleId) {
    return fail("kernel_sample_required", "Kernel Run requires exactly one Sample");
  }
  if (!request.geometry) {
    return fail("kernel_geometry_required", "Kernel Run requires one fixed geometry");
  }
  if (!request.kernels?.length) {
    return fail("kernel_list_empty", "Kernel Run requires one or more kernel candidates");
  }
  if (request.kernels.length < 2) {
    return fail("kernel_list_too_small", "Kernel search requires at least two kernel candidates");
  }
  const unique = new Set(request.kernels.map((kernel) => kernel.id));
  if (unique.size !== request.kernels.length) {
    // Same id with different parameters is allowed for Bicubic b/c exploration only when
    // parameters differ; collapse pure id duplicates.
    const signatures = new Set(
      request.kernels.map((kernel) => `${kernel.id}:${JSON.stringify(kernel.parameters)}`),
    );
    if (signatures.size !== request.kernels.length) {
      return fail("kernel_list_duplicate", "Kernel candidate list contains duplicate entries");
    }
  }
  const metric = validateMetricSpec(request.metric);
  if (!metric.ok) return metric;
  return { ok: true };
}

/**
 * Verification Run: one Source, one locked Recipe snapshot, one ScanScope.
 * Multi-source verification is a RunGroup of member VerificationRuns.
 */
export function validateVerifyShape(request: VerifyRequest): ShapeGuardResult {
  if (request.mode !== "verify") {
    return fail("verify_mode_mismatch", "Verify shape requires mode=verify");
  }
  if (!request.sourceId || !request.sourcePath) {
    return fail("verify_source_required", "Verification requires one Source");
  }
  if (!request.recipeId) {
    return fail("verify_recipe_required", "Verification requires a locked Recipe snapshot");
  }
  if (!request.kernel?.id) {
    return fail("verify_kernel_required", "Verification Recipe must include a kernel");
  }
  if (!request.geometry) {
    return fail("verify_geometry_required", "Verification Recipe must include geometry");
  }
  if (!Number.isInteger(request.concurrency)
      || request.concurrency < 1 || request.concurrency > 8) {
    return fail(
      "verify_concurrency_invalid",
      "Verification concurrency must be an integer within 1..8",
    );
  }
  const scope = request.scanScope;
  if (scope == null || !Number.isInteger(scope.streamIndex) || scope.streamIndex < 0) {
    return fail("verify_stream_required", "ScanScope requires a non-negative stream index");
  }
  if (scope.selection === "every_n") {
    if (!scope.everyN || !Number.isInteger(scope.everyN) || scope.everyN < 1) {
      return fail("verify_every_n_invalid", "every-N selection requires everyN >= 1");
    }
  }
  if (
    scope.startFrame != null &&
    scope.endFrame != null &&
    scope.startFrame > scope.endFrame
  ) {
    return fail("verify_range_invalid", "Custom frame range start must be <= end");
  }
  const metric = validateMetricSpec(request.metric);
  if (!metric.ok) return metric;
  return { ok: true };
}

/** Reject shapes that mix many heights with many kernels in one Run. */
export function rejectMixedHeightAndKernel(input: {
  heightCandidateCount: number;
  kernelCandidateCount: number;
}): ShapeGuardResult {
  if (input.heightCandidateCount > 1 && input.kernelCandidateCount > 1) {
    return fail(
      "mixed_height_kernel",
      "One engine Run cannot combine multiple heights with multiple kernels",
    );
  }
  return { ok: true };
}

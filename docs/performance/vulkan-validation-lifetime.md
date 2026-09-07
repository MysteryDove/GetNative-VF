# Vulkan Video validation crash workaround

Date: 2026-09-06. Hardware: Linux, RTX 5080, NVIDIA 595.84.

## Result and Scope

The first-frame SIGSEGV under handle wrapping is avoided by a bounded local VVL
overlay. Core validation, synchronization validation and handle wrapping remain
enabled. This is a validation-runtime workaround; the stock SDK remains affected.
It does not establish that every Verify selection/input passes validation.

The overlay and reproducible isolated build helper are in
`tools/vulkan-validation/`. VVL is pinned to SDK 1.4.357.0 commit
`f4874eee15c78d7bdb2b7e60659d539f14741500`. No system driver or SDK installation
was overwritten; the tested layer used an isolated installation prefix.

## Causal Evidence

1. Initialization/reset commands submitted successfully. The first
   `vkCmdDecodeVideoKHR` recording was followed by SIGSEGV in NVIDIA's driver
   under `vvl::DispatchDevice::QueueSubmit2`.
2. The VVL handle-wrapping path creates a deep copy of `VkVideoDecodeInfoKHR`,
   unwraps its handles, calls the driver and destroys the copy immediately.
3. Suppressing that destructor under GDB allowed two eight-frame jobs to finish
   with all three validation settings enabled. This diagnostic intentionally
   retained memory and was not used as the delivered solution.
4. A compiled patch retained the copies through the command-buffer recording.
   Two 5001-frame jobs then finished without validation errors; all complete
   frame records exactly matched the earlier Vulkan results.
5. Removing only the retention from the same locally built VVL restored the
   eight-frame crash. The installed patched library was kept separate from this
   negative-control build and the source was restored afterward.

These observations strongly support delayed driver access to the recorded
parameters. They do not constitute vendor confirmation of the precise internal
driver defect. The similar upstream report is
<https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/11849>.

QueueSubmit2 emulation and restricting decode image views to video-only usage
did not resolve the crash. Those probes were not added to the application.

## Bounded Lifetime

The patched layer retains an owning deep copy per recorded decode command,
indexed by command buffer and its wrapped command-pool handle. Copies are
released after successful begin/reset, buffer free, pool reset/destroy, or device
destruction. Concurrent bookkeeping is mutex-protected. This avoids keeping a
history proportional to all frames processed by reused command buffers.

Pending GPU work must still complete before legal reset/free, exactly as required
by Vulkan. The patch does not bypass any of those validation checks.

## Verification

- H.264 8-bit / 5001 frames / both axes / bilinear / p=1: two jobs completed under
  the patched layer, with exact seq, frame identity, timestamp and error parity.
- HEVC 10-bit / 128 frames / both axes / bilinear / p=4: two jobs completed and
  complete frame records matched the previous Vulkan reference.
- H.264 8-bit / 34072-frame complete source: two jobs completed with zero decode
  retries, no fallback, complete unique coverage and zero validation messages.
  The measured job was 46655.566575 ms (730.287991 fps), including validation
  overhead; it is not a production performance comparison.
- Positive control `validation_canary.cpp`: intentionally invalid fence flags
  and a write-after-write buffer hazard were both caught (`core_errors=1`,
  `sync_errors=1`). Those intentional errors have a separate log.
- Ten cancellation/reuse cycles on the real H.264 source completed with GPU
  decode, exact 257-frame contiguous and 37-frame every-seven results, and no
  errors in the dedicated validation log. RSS samples are recorded; they include
  allocator, FFmpeg and driver caches and are not a formal leak-proof result.
- `vulkan_decode_runs_test.py` checks an independently encoded open-GOP input:
  240-frame full decode, every-seven, I pictures, partial range and decoder reuse.
  Numerical/identity checks pass, but the additional validation-log gate exposes
  the residual issue below.

An initial installed-manifest test was invalid because a bare shared-library
name failed to resolve in the isolated prefix. The build helper now writes a
relative library path. The canary and reuse tests were rerun after this fix.
Earlier installed-prefix results without a loaded layer are not acceptance
evidence. Build-directory tests did load the layer.

## Remaining Boundary

The newly exposed open-GOP sparse cases can still submit concealed/missing DPB
references, producing `07238`, `07151` and `07264` even when selected numerical
results match. A narrow media fix flushes Vulkan between independently sought
runs and prevents the observed fallback after a completed prefix on the real
source. It does not resolve all generated open-GOP validation errors.

A leading-frame-based preroll experiment did not clear that remaining gate and
was removed. The regression tool accepts an optional validation-log path as its
second argument after the engine, and fails on new validation errors. Do not
waive those errors or treat the crash workaround as full adaptive-decode release
acceptance. Windows, other GPUs/drivers, and the complete adaptive scheduling
plan remain unverified/unimplemented as recorded separately.

## Evidence

Raw debugger and validation logs are not distributed with this repository.
Canary runs intentionally contain errors; archived open-GOP failures and
successful reuse runs are separate evidence. Interpret each result using its
stated configuration rather than treating the entire experiment as passed.

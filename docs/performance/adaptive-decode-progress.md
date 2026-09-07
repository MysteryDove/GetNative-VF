# Adaptive decode implementation checkpoint

Current default (2026-09-07): adaptive hardware Verify decoding is enabled via
`GETNATIVE_ENABLE_ADAPTIVE_DECODE=ON` for CUDA, Vulkan Video, and VideoToolbox.
The former test-only option has been replaced; builders can explicitly use
`GETNATIVE_ENABLE_ADAPTIVE_DECODE=OFF` for single-session decoding, and the
internal fixed-tier override still takes precedence. This default change does
not close the remaining implementation or platform-validation gaps below.

Default-build validation on Apple M4 Max: all 20 CTest cases passed; the supplied
`00001.m2ts` completed 34,072 frames in each of two VideoToolbox/Metal runs with
zero failures or retries, zero-copy provenance, and identical frame records.
Both runs retained one session. This verifies the default build on macOS, not
multi-session expansion or a performance gain. Configuration checks also
covered explicit disablement and the fixed-tier override.

The following sections record the implementation and evidence from 2026-09-05
and 2026-09-06, when release defaults were disabled. They are historical
checkpoints, not a release acceptance report.

2026-09-06 follow-up: the validation crash now has a tested, bounded local VVL
workaround with handle wrapping still enabled. Stock VVL remains affected.
Closed-restart packet checks now avoid the invalid open-GOP DPB starts; the
synthetic sparse/I-frame/partial-range test passes with zero validation messages.
See [the lifetime investigation](vulkan-validation-lifetime.md). The original
validation observations below are historical, not the current test result.

## Linux continuation, 2026-09-06

Latest: [Vulkan optimization experiments](vulkan-optimization-experiments.md)
records the same-thread frame-lock repair and actual decode-queue exposure.
With these changes, the same-source 34072-frame automatic/single three-pair run
passes strict frame parity: 1161.60 versus 807.22 fps, 30.51% less elapsed time.
This supersedes the earlier conclusion that Vulkan multi-session decode has no
benefit. Shader unroll/tiling experiments were reverted and plan caching remains
an internal backend option defaulting off. No release adaptive default is enabled.

Vulkan full-job parity subsequently exposed an FFmpeg bitstream-padding defect,
including corruption in the old fixed-single baseline. The repair and raw-pixel
evidence are in [vulkan-bitstream-padding.md](vulkan-bitstream-padding.md).
The patched pinned 8.1.2 Linux SDK has been built through the project script and
passes the focused raw-pixel check. Earlier unpatched Vulkan comparisons below
remain historical measurements, not proof of correct decode output.

The shared indexed-range scheduler and controller now drive all three hardware
routes behind internal CMake test configuration. Delivery reservation/commit
prevents duplicate analysis during tail transfer and retry. Hardware memory
queries feed the budget; the unpublished CUDA environment switch is removed.
VideoToolbox has cancellable callback waits and a health timeout. Release
defaults remain disabled, and Windows testing is explicitly deferred by the user.

The first formal Vulkan 5001-frame three-pair test regressed by 3.6843%:
6147.561147 ms fixed one versus 6374.054562 ms automatic. The probe finished the
job without completing its three stable windows. This is a failed gate, recorded
under the historical run label `paired-vulkan-5001`.

Probe admission now accounts for prospective doubled throughput: remaining
work, expressed in current-tier seconds, must cover initialization plus six
seconds before expansion. That preserves three observation windows even at
ideal twofold throughput; the 8% retention and 3% regression thresholds are
unchanged. It intentionally suppresses expansion in some short CUDA jobs too.

The corrected Vulkan 5001-frame interleaved three-pair run passed exact frame
parity, with 6144.004088 ms fixed-one median and 6149.885536 ms automatic median
(+0.0957%). Automatic stayed at one session in all three runs. Full-card sampled
memory peaks, including warmup, were 794 MiB fixed and 791 MiB automatic.
Historical run label: `paired-vulkan-tail-5001`.

The earlier CUDA 5001-frame automatic result (23.2% less elapsed time) used the
previous admission policy and is not evidence for the corrected binary.
Controller boundary tests and the scheduler ThreadSanitizer executable passed
locally after this continuation. Formal long-job results are recorded separately
as they finish.

The corrected CUDA 34072-frame interleaved three-pair run passed exact frame
parity: fixed-one median 40262.048441 ms (846.255998 fps), automatic median
23270.731918 ms (1464.156784 fps), elapsed-time ratio 0.577981817. Automatic
elapsed-time spread was 3.20145%; all measured runs peaked at four sessions,
with one reverting the four-session probe and two retaining it. Full-card
sampled peaks including warmup were 1063 MiB fixed and 1495 MiB automatic.
Historical run label: `paired-cuda-tail-34072`. This comparison
does not establish the separate 95%-of-fixed-dual gate.

The current local build also passed all 20 CTest cases with the documented
FFmpeg library environment.

Remaining implementation gaps include persistent adjacent 256-4096-source-frame
chunks (current leases still use large tails), full session/thread failure
injection, initialization-to-first-output telemetry, and profile-specific DPB
estimates. These prevent claiming the complete plan is delivered.

## Implemented

- `engine/include/getnative/decode_control.hpp`: job-local 1/2/4 controller,
  one-second cumulative demand sampler, conservative memory-budget calculation,
  warmup acknowledgement, three-window median trials, rollback acknowledgement,
  resource caps and failed-edge freezing. The scheduler must provide stable
  windows, producer thread-time exposure and session readiness acknowledgements.
- `MediaVerifyPipeline`: mutex-protected cumulative starvation, producer capacity
  wait and queued-frame time, including waits spanning observation boundaries.
  Decoder options carry a media-owned feedback callback for each hardware Verify
  route. Thread creation failure joins already-created analysis workers.
- Optional result telemetry: `analysis_starvation_thread_ms`,
  `producer_capacity_thread_ms`, `analysis_queue_frame_ms`. These are cumulative
  thread/frame-time counters, not wall time or normalized percentages.
- Vulkan device/FFmpeg agreement for enabled timeline, synchronization2 and
  sampler YCbCr conversion features. Queue locks use actual family/index pairs.
- Vulkan video frame pool uses the codec's FFmpeg profile and queries supported
  video formats/usages before initialization; it removes unused storage usage
  and requires mutable format for the luma view. The luma view restricts usage
  to sampled access.
- Vulkan image layout transitions cover the entire image, matching AVVkFrame's
  per-image layout state. Semaphore waits protect layout transitions as well as
  shader reads. Early surface release after conversion remains intact.

## Earlier checkpoint validation

Local macOS build: `cmake --build build/engine -j 8`.

Local CTest: 19/19 passed with the build's FFmpeg runtime directory in
`DYLD_LIBRARY_PATH`.
Without that environment, the worker protocol test's portable-executable copy
cannot load the adjacent FFmpeg dylibs because it copies only the executable.
The controller was also rebuilt and rerun after the last budget/rollback edits.

Linux RTX 5080: source and build were isolated from the regular checkout.
Controller, existing CUDA fixed dual-session integration, media worker and Vulkan
analysis tests passed (4/4). This is not adaptive scheduler coverage.

Vulkan numerical checks against the previously retained implementation:

- H.264 8-bit, 5001 frames, bilinear, both axes, p=1: all complete frame records
  exactly equal, including seq, identity, timestamps and error. One warm measured
  job was 6142.032723 ms / 814.225555 fps. This is a smoke measurement, not the
  required interleaved three-pair performance campaign.
- HEVC 10-bit, 128 frames, bilinear, both axes, p=4: all complete records exactly
  equal. An initial comparison used different p-norms and was discarded.

Full validation is NOT passed. With default handle wrapping, validation layers
1.4.357 and 1.4.341 both encounter process termination. The captured 1.4.357
stack has SIGSEGV in NVIDIA 595.84's `libnvidia-eglcore.so` under
`vvl::DispatchDevice::QueueSubmit2`. This alone does not establish driver fault.
The system FFmpeg CLI, independently of this project's shared device, also
returns -11 (SIGSEGV) when decoding only eight H.264 frames with full validation;
`standalone-ffmpeg.json` contains the exact command and return code. Loading the
build SDK's FFmpeg libraries via an isolated `LD_LIBRARY_PATH` also reproduces
worker termination. No driver/validation/FFmpeg combination tested so far clears
the full-validation gate.

A diagnostic run with `unique_handles=false`, core validation and synchronization
validation enabled exposed real WRITE_AFTER_READ and plane-layout mismatches.
After the barrier fixes, the H.264 5001-frame and HEVC 128-frame diagnostic runs
had zero validation messages. Disabling wrapping is diagnostic only and does not
satisfy the full-validation acceptance requirement.

Raw experiment records are not distributed with this repository. The validation
layer was installed separately; no system package or GPU driver was changed.

## Earlier checkpoint remaining work (superseded above)

- Resolve the full Vulkan validation crash and retain reproducible full core and
  synchronization validation evidence, with no ignored VUIDs.
- Replace static CUDA bisection with persistent indexed-range scheduling,
  safe tail handoff, adjacent continuation, delivery ownership and retry recovery.
- Backend live resource estimates and controller actuation, including session
  creation failures, retirement and cancellation. The feedback callback exists
  but is not yet consumed by the decoder scheduler.
- VideoToolbox health checks and cancellable waits, without per-range flushing.
- Remove the unpublished CUDA environment switch when its replacement is wired;
  migrate fixed-tier integration tests to internal configuration.
- Scheduler identity/fault tests, all selection modes, p/axis/kernel and
  8/10-bit matrices, candidate scan/preview regressions, Linux/Windows/macOS
  runtime gates, and full paired 5001/34072-frame performance campaigns.
- Enable the implicit default only after the requested platform gates pass.

The 95%-of-fixed-dual target needs a workload boundary: using prior measured
851/1462 fps, a 5001-frame job that spends its first two seconds at one session
has an idealized lower bound of approximately 4.26 seconds even with instant
expansion and no probe overhead. That is approximately 1175 fps, below 95% of
1462 fps. No observation policy or performance threshold has been changed to
hide that conflict.

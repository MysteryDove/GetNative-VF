# Vulkan Verify experiments — Linux RTX 5080

2026-09-06 continuation. Release adaptive defaults remain gated. All new GPU
experiments use the isolated test host and the pinned FFmpeg 8.1.2 SDK with the
bitstream-padding and coincident-view-usage patches. Diagnostic FFmpeg snapshot
experiments below are explicitly separate from that SDK.

## Findings

The former conclusion that Vulkan compute was saturated, or 63% slower than
CUDA kernels, was not supported by the counters. `compute_ms` sums analysis-call
wall time, including waiting; Vulkan `upload_ms` measures host packing. High
normalized analysis starvation and negligible producer capacity waits indicated
a supply/synchronization limit worth investigating.

The RTX 5080 driver advertises two queues in its video-decode family. The shared
device previously created and advertised only one. Creating both supported
queues and exposing the actual count to FFmpeg produces a substantial measured
gain. Each actual family/index has its own mutex, including both decode queues.
This uses reported queue availability, not a model table or inferred physical
decoder count. Devices with only one supported queue retain one; same-family
compute/video devices retain the existing conservative shared-queue route.

The queued-frame handoff also transferred a locked FFmpeg frame mutex from the
decoder thread to an analysis thread. That violates mutex ownership. Queued
leases now retain only the AVFrame. On the consumer thread,
`acquire_for_submit` creates a scoped submission lease, locks the frame, and
reads fresh mutable layout/access/queue/semaphore state. Conversion submission
updates that state and unlocks on the same thread. Early GPU semaphore signaling
after conversion remains unchanged. Unconsumed cancelled frames own no mutex.

## Controlled experiments

5001 frames, H.264 1080p, bilinear, both axes, p=1, analysis concurrency 8.
Each experiment alternates baseline/candidate order for three pairs, with one
warmup and one measured job per process. Every warmup and measured frame record
must match exactly. Instrumentation is disabled for performance comparisons.

| Experiment | Baseline median ms | Candidate median ms | Disposition |
|---|---:|---:|---|
| Per-slot immutable plan residency, one session | 6130.374043 | 6126.770622 | 0.059% is below noise; internal option defaults off |
| Inverse loop unroll hints, one session | 6125.695024 | 6121.605931 | 0.067% is below noise; reverted |
| Shared-memory tiled horizontal inverse, one session | 6124.275583 | 6123.008456 | 0.021% is below noise; reverted |
| Two decode queues, fixed two sessions, other experiments present | 6008.665220 | 4190.025485 | 30.27% less time; isolate queue change next |
| Only queue change, fixed two sessions | 6024.324460 | 4151.665452 | 31.08% less time; 830.13 to 1204.58 fps |

The last comparison uses `engine-dual-range-linux` and `engine-queueonly-dual`.
It precedes the same-thread frame-lock repair and is not the final stability
acceptance. Whole-card peak samples including warmup were 918/955 MiB; candidate
elapsed-time spread was 2.03%. This is fixed-two-session performance, not the
automatic controller's end-to-end result.

An optional GPU timestamp profiler now measures luma, transpose, inverse,
forward, metric and result-copy intervals. Timestamps are taken at the relevant
compute/transfer pipeline stage and are inactive in ordinary runs. They can
overlap across slots and include GPU scheduling contention. The first exploratory
profile used TOP/BOTTOM boundaries and showed inverse dominated the recorded
intervals; that profile does not establish isolated shader latency or GPU
saturation, and must not be presented as the final timestamp implementation.

## Correctness and failure investigation

Before the frame-lock repair, ordinary matrices intermittently produced differing
metrics or device loss. Full VVL matrices could pass, so validation alone did not
establish correctness. Isolated FFmpeg experiments retaining slice offsets or
deep-copying H.264/HEVC decode structures each passed once then failed on repeat.
Neither experimental FFmpeg change is retained in the project SDK scripts.

After the same-thread frame-lock repair, ordinary 8/10-bit matrices passed three
consecutive runs (54 cases each): p=1..4, all three axes, bilinear/lanczos3,
full/partial/every-N/I-picture selections, with exact single/dual parity. Full
core/sync validation then passed the same 54 cases with zero logged errors.
The fixed single/dual integration test also passed full and partial GOP ranges,
sparse/short/serial cases, cancellation, and subsequent decoder/analysis reuse.
The local macOS build and all 20 CTest cases passed earlier in this continuation;
that is not a substitute for Linux Vulkan execution.

## Evidence and remaining acceptance

Remote root: `/home/owen/tmp/gnvf-tune-20260905/adaptive-evidence/`.
The `paired-vulkan-plan-cache-5001`, `paired-vulkan-unroll-5001`,
`paired-vulkan-tiled-5001`, `paired-vulkan-queues-5001` and
`paired-vulkan-queueonly-5001` directories contain binary/library hashes,
per-job records and summaries. `matrix-vulkan-lock-{1,2,3}.json` and
`matrix-vulkan-lock-validation.json` record the completed correctness matrices.

Final same-source automatic/single long-job timing, full-job validation,
short-job regression and final runtime evidence follow.

The final same-source 34072-frame three-pair run passed exact parity for every
warmup and measured frame record. `engine-lock-single` versus `engine-lock-auto`
with the same `ffmpeg-8.1.2-fixed-sdk` runtime measured:

| Mode | Median ms | Median fps | Elapsed-time spread | Whole-card peak MiB |
|---|---:|---:|---:|---:|
| Fixed one session | 42208.965321 | 807.221872 | 0.00738% | 855 |
| Automatic | 29331.861138 | 1161.603754 | 0.82754% | 1146 |

Automatic elapsed time is 0.694920165 of fixed single (-30.508%), equivalent to
43.90% more throughput. Every measured automatic job retained two sessions,
probed four, and reverted to two; `decode_sessions=4` records the peak, not the
retained tier. The total includes probing, initialization and tail/restart costs.
The memory column is sampled whole-card usage including warmup, not per-job
allocated bytes. See `paired-vulkan-final-34072/summary.json` for binary and
resolved runtime-library hashes. This comparison supersedes the earlier
fixed-dual queue-only screening for the final delivery's end-to-end claim.

The final 5001-frame three-pair short-job gate also passed: fixed-single median
6130.767539 ms (815.721681 fps), automatic median 6131.712163 ms (815.596014 fps),
only 0.01541% more elapsed time. Automatic retained one session because the
remaining-work admission guard cannot fund a complete expansion trial. All
records matched exactly. Evidence: `paired-vulkan-final-5001/summary.json`.

Full core/sync validation of the final automatic binary completed two 34072-frame
jobs, each exactly matching the fixed-single results, with zero log messages.
The same isolated patched VVL retains handle wrapping; the canary immediately
before this run again emitted one intentional core and one synchronization
error. The normal job's log is separate. Evidence:
`vulkan-final-full-validation.json` and `optimization-validation/validation.log`.
This does not claim the stock VVL parameter-lifetime crash is fixed upstream.

The final 5001-frame validation run also completed twice with exact parity.
Open-GOP every-seven/I-picture/partial-range/reuse regression and the Vulkan
analysis suite (including noncoherent-buffer validation and the internal plan
cache test) passed with the same layer enabled; the normal validation log
remained empty. Final local build/CTest passed all 20 cases.

The final fixed-dual GPU-stage profile (`profile-vulkan-final-dual.json`) passed
exact parity with the unprofiled baseline. Its measured 5001-frame intervals,
using compute/transfer timestamp boundaries, were:

| Stage | Summed GPU interval ms |
|---|---:|
| Luma | 88.693056 |
| Transpose | 88.708608 |
| Inverse | 7690.842880 |
| Forward | 86.392512 |
| Metric | 335.235552 |
| Result copy | 16.569440 |

Inverse accounts for about 92.6% of these recorded intervals, so it remains the
main shader target for later profiling. These are overlapping per-slot GPU
intervals, not exclusive device-busy time or a theoretical throughput ceiling.
The formal throughput table above was measured with profiling disabled.

Windows remains deferred by the user. The larger adaptive plan still requires
persistent adjacent range scheduling and additional failure injection before
complete delivery or platform-default enablement can be claimed.

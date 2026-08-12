# Pipeline Optimization Evidence (2026-08-10)

All measurements below use the same `engine/bench/pipeline_probe.py`, a real
FFmpeg FFV1 fixture, resident worker sessions, and five samples. Raw JSON was
written outside the repository under `/tmp` so generated media and device
telemetry do not become source artifacts.

## Verify ring

The 1920x1080, 1,000-frame comparison produced identical checksums:

| Transport | Median wall | Median FPS | Estimated frontend IPC |
| --- | ---: | ---: | ---: |
| `frame_asset` fallback | 7,224.7 ms | 138.4 | 2,004 |
| shared ring | 3,689.0 ms | 271.1 | 3 |

Ring throughput improved 95.8%. The 10,000-frame ring run remained stable:
worker RSS was 136,634,368 to 136,671,232 bytes across five samples, and the
producer's peak RSS was 287,776,768 bytes against a 265,420,800-byte, 32-slot
ring (1.084x). All ring/file and 10,000-frame checksums matched.

The protocol test also exercises one-slot wrap, stale generation, out-of-order
sequence, slot range, cancellation wake-up, and fallback progress before the
file producer reaches its two-slot limit.

## Media batch

For eight selected 1080p frames from one encoded source, serial export took a
928.4 ms median and one-process batch export took 165.8 ms, an 82.1% wall-time
improvement. Both paths wrote 66,355,200 bytes with the same checksum. A
single-frame member remains on the legacy exporter, so the batch feature adds
no single-frame startup cost.

## CPU scheduling

The rotated formal matrix used 301 candidates and CPU worker counts 1/8/16/auto
(plus an explicit 32-worker comparison). Checksums were stable. Auto was within
5% of the best explicit 8/16 result for bicubic (0.03%) and lanczos8 (0.89%),
but bilinear's fixed narrow-kernel cap was 24.0% slower than the explicit
16-worker result on this host. The auto policy therefore remains a correctness
implementation with this performance gap recorded; it is not claimed as a
passed universal performance gate. The explicit 32 comparison is also too
host-frequency-sensitive to serve as a release baseline without the prior
binary's outer-chunk implementation.

## CUDA

On an RTX 5080, capability discovery plus the first CUDA job was 210.9 ms with
the retained worker engine versus 471.9 ms for the temporary-capability plus
fresh-job comparison, a 55.3% improvement. Checksums matched.

The device input LRU was tested with explicit `GETNATIVE_CUDA_INPUT_CACHE_BYTES`
on/off in rotated pairs. Cold H+W source upload fell from 16,588,800 to
8,294,400 bytes. Warm bilinear and bicubic were 3.17% and 5.67% slower with the
cache, so the device input cache is implemented but defaults to zero bytes;
opt-in remains available for workloads that pass their own guard. Host
packed-plan caching is enabled by default for cross-process reuse and can be
redirected or disabled from the application Settings page.

Probe commands:

```sh
python3 engine/bench/pipeline_probe.py build/engine-default/getnative-engine \
  --samples 5 --verify-frames 1000,10000
python3 engine/bench/pipeline_probe.py build/engine-cuda/getnative-engine \
  --backend cuda --samples 5 --skip-kernel --skip-verify --skip-media-batch
```

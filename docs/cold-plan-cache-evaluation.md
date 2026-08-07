# Cold (Persistent) Plan Cache Evaluation

- Status: **Implemented** (E4, 2026-08-08) — L1 single-flight LRU
  default-on; L2 pack store opt-in (`GETNATIVE_PLAN_CACHE=on` /
  `GETNATIVE_PLAN_CACHE_DIR`). Outcome, final numbers, and the
  keep/reject split: `docs/performance/e4-cold-plan-store-20260808.md`.
  The format implemented is GNPK **v3** (adds codec field, LZ4 default,
  XXH64 content checksums); the v2 sketch in §3.2 was superseded during
  bring-up.
- Date: 2026-08-08.
- Question: can plans survive process restarts so repeated getfnative-style
  workloads across different images preheat from disk instead of rebuilding?

## 1. Why reuse across images is sound

`PlanKey` (`engine/src/planner/axis_plan_key.hpp`) is a bit-exact function of
**geometry and kernel only**: `{source_size, destination_size, active_length,
shift, kernel type, b, c, taps, border}`. Image content never enters the key.
A plan for `1080 → 810, bicubic(0, 0.5)` is identical for every 1080p frame
ever fed to the engine. The typical getfnative workload — one show, many
episodes, same resolution, same candidate grid — therefore reuses **100%**
of its plans across images, samples, and app restarts. Different resolutions
or grids produce different keys and miss cleanly.

`AxisPlan` is immutable and pointer-free (seven flat arrays +
scalars, ~94.4 KiB average, `axis_plan_storage_bytes`), so serialization is a
mechanical layout exercise. dsmvc has no disk format to borrow (confirmed
during the port survey); we design our own, using the CUDA `PackedBatch`
single-blob idea only as a layout reference.

## 2. What it buys — honest numbers

Measured facts (`docs/performance/pre-gpu-planner-results.md`):

- Parallel batch build: 1,000 unique plans in **15.81 ms** (6.65x over the
  105.62 ms serial baseline). A disk read of the same 92.2 MiB is ~50-100 ms
  on NVMe — **slower than rebuilding for bicubic-class plans**.
- Warm in-session hit: 16.97 → 0.059 ms (291.78x) — already solved by the
  worker session cache (E0).
- **Fixed-admission cliff**: the current cache retains at most
  1024 entries / 256 MiB with fixed admission (overflow plans are returned
  but *not retained*, and it is not single-flight). For 1,000-plan
  Lanczos-6/7/8 scans, measured hit rates drop to **92.7 / 79.7 / 69.8%** —
  i.e. up to 302 plans are rebuilt *within one repeated scan*. Large-taps
  plans are also the biggest (~2-4x average size) and slowest to build.

Conclusion on value:

| Scenario | Cold cache win |
| --- | --- |
| Integer coarse scan, bicubic, 301 candidates | ~None (build ≈ 5 ms) |
| Large-taps 1,000-point scans (Lanczos 6-8) | **Real**: eliminates the 7-30% forced rebuilds; effective hit rate → ~100% for repeated grids |
| App restart, same series (810p episode checks) | First-scan latency removed via preheat; UX, not throughput |
| Cross-machine cache sharing | Possible only with identical build fingerprints (see §4) |

So the cold cache is a **robustness and latency feature, not a throughput
feature** — and it only makes sense bundled with the in-memory policy
upgrade the strategy doc already lists (single-flight LRU, D3).

## 3. Proposed design (two-level cache)

```
L1  AxisPlanCache (memory): fixed-admission → single-flight LRU
    (adopt dsmvc SingleFlightLru semantics; byte+entry dual cap)
L2  PlanStore (disk): content-addressed, read-through, write-behind
```

Lookup: L1 hit → return; L1 miss → L2 file `<cache>/plans/v1/<key_hash>.gnpl`
→ validate → publish into L1 → return; L2 miss → build (batch workers) →
publish L1 → write-behind to L2 (atomic tmp+rename, no-replace publish).

### 3.2 Chunked pack layout (sparse reads without 30,000 files)

A 30,000-candidate grid as individual files is a filesystem anti-pattern
(inode pressure, directory scans, sync/backup pain); a single monolithic
zstd stream forces whole-pack reads for sparse access. The resolution is
the standard **chunked pack with a sparse index** (same family of layout
as zstd-seekable and git packfiles): one file per grid, plans grouped in
grid order into independent zstd frames of K plans each; an in-header
index maps `key_hash → chunk + offset`, so reading one plan decompresses
only its chunk (~0.6 MiB compressed, sub-millisecond), while full preheat
is a sequential stream of chunk frames.

Chunk size measured on the cooked corpora (ratio vs raw payload):

| K (plans/chunk) | bicubic | spline64 | Chunk compressed size |
| --- | --- | --- | --- |
| 16 | 4.56x | 4.02x | ~0.3 MiB |
| **64** | **4.63x** | **4.07x** | **~0.6 MiB** |
| 256 | 4.64x | 4.04x | ~2.4 MiB |
| whole-pack (§3.1) | 4.74x | 4.13x | all |

K=64 loses only ~2% ratio against the monolithic pack while keeping
sparse reads at ~0.6 MiB / ~0.2 ms — chosen. (K=16 is also fine; the flat
curve says within-plan redundancy dominates and cross-plan redundancy is
a small bonus, consistent with the modest dictionary gain in §3.1.)

For a 30,000-candidate grid: 469 chunks, in-header index 30,000 × 20 B =
~600 KiB (loaded once per pack open, binary search), file ~650 MiB,
single-plan sparse read ~0.4 ms, full preheat ~0.25 s.

Alternatives considered and rejected: SQLite (extra dependency, per-blob
compression loses the within-chunk redundancy unless a dictionary is
bolted on); zstd-seekable contrib (reusable frame table, but we need the
key-hash index anyway and libzstd alone suffices).

File format v2 (little-endian, self-validating):

```
magic "GNPK" | format_version u32 | grid_hash u64 | build_fingerprint u64
plan_count u32 | chunk_size u32 | chunk_count u32
plan index[plan_count]: {key_hash u64, chunk_ordinal u32,
                         cooked_offset u32, cooked_len u32}  (sorted)
chunk directory[chunk_count]: {file_offset u64, compressed_len u32}
chunk data: zstd1 frames, one per chunk, plans in grid order
checksum u64
```

- `key_hash` / `grid_hash`: FNV-1a over the canonical PlanKey / the ordered
  key list (already implemented primitives).
- `build_fingerprint`: compiler id+version, FP mode (`planner_fp`
  strict/fast), build type, planner source identity. Mismatch → treat as
  miss and rebuild; **never** load plans whose provenance differs, because
  last-bit Float64 assembly order is part of the upstream-conformance
  contract.
- Validation on load: magic/version/hash, size bounds
  (destination ≤ 65536, half-bandwidth ≤ 29), all values finite,
  `AxisPlan::valid()`, checksum match. Any failure → delete file, rebuild.
  Corrupt cache can never fail a job.
- Eviction: bounded directory (default 1 GiB), mtime-LRU sweep at session
  start; atomic publish makes concurrent workers safe.

### 3.1 Codec selection, measured (2026-08-08, supersedes the earlier assumption)

The first draft of this document assumed plan data was "mostly
incompressible float data". That assumption was **wrong**; measurements on
a 400-plan corpus per kernel family (1080 → 700..1099, mirror border,
`engine/bench/plan_compression_probe.cpp` on branch
`engine/worker-protocol`) show:

Structural pre-transform ("cooked") applied before entropy coding —
lossless, exploits invariants verified on every plan:

- `forward_offsets` is always the uniform stride `row * forward_width`
  (drop entirely, reconstruct from two scalars);
- `forward_indices` is always per-row consecutive runs (keep one `left`
  anchor per row instead of `destination × forward_width` entries);
- `transpose_offsets`/`transpose_indices` delta-varint encoded.

| Variant (bicubic corpus, raw 41.18 MB) | Bytes | Ratio | Compress | Decompress |
| --- | --- | --- | --- | --- |
| raw + zstd1 | 10.46 MB | 3.94x | 1037 MB/s | 2470 MB/s |
| **cooked + zstd1** | **8.69 MB** | **4.74x** | **1448 MB/s** | **2990 MB/s** |
| cooked + zstd3 | 7.03 MB | 5.86x | 554 MB/s | 2345 MB/s |
| cooked + zstd9 | 6.60 MB | 6.24x | 229 MB/s | 2722 MB/s |
| cooked + lz4 | 12.11 MB | 3.40x | 2888 MB/s | 13027 MB/s |
| cooked + lz4hc9 | 10.98 MB | 3.75x | 187 MB/s | 8532 MB/s |

Ratios are consistent across families (cooked+zstd1: bicubic 4.74x,
lanczos3 4.26x, spline64 4.13x; cooked+zstd3: 5.86x/5.47x/5.25x). zstd9
adds little over zstd3 — level 1/3 is the sweet spot. lz4 decompresses at
a remarkable 13 GB/s but costs ~40% more storage than zstd1.

Whole-corpus (one pack per grid) vs per-plan file, measured with a trained
zstd dictionary (110 KiB, `--train` on the cooked corpus):

| Store shape | bicubic | spline64 |
| --- | --- | --- |
| Grid pack, cooked+zstd1 | 4.74x | 4.13x |
| Per-plan file + plain zstd1 | 3.21x | 3.20x |
| Per-plan file + trained dictionary | 3.92x | 3.69x |

The grid pack beats even the dictionary-assisted per-plan store and needs
no dictionary lifecycle management; it is also exactly the "preheat: one
read" shape. Decision: **cooked pre-transform + zstd1, one pack per
(candidate grid × build fingerprint)**; per-plan dict-assisted files are a
recorded fallback for sparse access patterns.

Projected footprint for a 30,000-candidate stress grid (bicubic,
100.7 KiB logical/plan):

| Store | Size | Access |
| --- | --- | --- |
| raw | 2.95 GiB | — |
| **chunked pack (K=64), cooked+zstd1** | **~640 MiB** | **sparse plan ~0.4 ms; full preheat ~0.25 s** |
| monolithic pack, cooked+zstd1 | ~630 MiB | preheat only (~0.2-0.3 s); sparse = full read |
| per-plan files + dictionary | ~770 MiB | sparse fast; 30,000 inodes |

zstd1 is the default: within ~20% of the zstd9 ratio at 4-5x the
compression speed and full decompress speed. lz4 remains the "fastest
possible preheat" escape hatch (13 GB/s) if load time ever dominates.

## 4. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Cross-build FP non-determinism (fast-math planner, compiler differences) | build fingerprint in key; miss-and-rebuild |
| Format drift as AxisPlan evolves | format_version + fail-closed reader |
| Corrupt/poisoned cache files | checksum + full structural validation + rebuild fallback |
| Multi-process writers | atomic no-replace publish (pattern already in `benchmark_support.hpp`) |
| Disk growth | bounded LRU sweep; opt-out env var |
| Write-behind stalls the job | writer on a background thread, best-effort |

## 5. Preheat paths (protocol v1.1, optional)

- On `hello`: background sweep of the L2 directory index (sizes/mtimes
  only, no payload reads).
- On `analyze`: read-through is automatic; a future `preheat` command
  (source dims + grid) can bulk-load matching plans while the user is
  still picking frames. This is the "load once, read all" shape the
  question asks about — it falls out of L2 for free.

## 6. Recommendation

Feasible and worth doing, in this order, as one planner-lane change:

1. L1: replace fixed admission with single-flight LRU (correctness of
   retention under pressure; dsmvc-proven semantics).
2. L2 disk store with the format/validation above; targets the measured
   Lanczos 7-30% rebuild cliff.
3. Evidence: repeated 1,000-plan Lanczos-6/8 scans, effective hit rate
   79.7/69.8% → ~100%; first-scan latency after process restart, preheated
   vs cold. Keep/reject per the usual discipline.

Explicitly not now: cross-machine sharing (fingerprint-gated), CUDA packed
batch persistence (device layouts are internal and version-sensitive),
trained zstd dictionaries (grid packs already beat them, §3.1).

Priority: below the Tauri transport merge and E2 verification; above any
further kernel micro-optimization, because it removes a measured
correctness-of-retention cliff rather than chasing marginal wall time.

# Vendored LZ4

- Version: 1.10.0 (upstream release tarball), `lib/lz4.c` + `lib/lz4.h`
  verbatim, BSD-2-Clause (`LICENSE`).
  `xxhash.c`/`xxhash.h` (same release) provide the chunk content
  checksum (XXH64).
- Used by: the cold plan store (E4, `src/planner/plan_store.cpp`) for GNPK
  v3 chunk compression. Chosen over zstd for fetch-latency-critical reads
  (~13 GB/s decompress measured on plan corpora; see
  docs/cold-plan-cache-evaluation.md §3.1 and the E4 evidence doc).

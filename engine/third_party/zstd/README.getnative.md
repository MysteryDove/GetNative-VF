# Vendored zstd (single-file)

- Version: 1.5.7 (upstream release tarball).
- Generated: `build/single_file_libs/combine.py -r lib -o zstd.c zstd-in.c`.
- Files: `zstd.c` (amalgamated library), `zstd.h`, `zstd_errors.h`,
  `LICENSE` (BSD-3-Clause / BSD-2-Clause per upstream).
- Used by: the cold plan store (E4, `src/planner/plan_store.cpp`) for
  GNPK chunk compression (level 1). Not part of the public engine API.

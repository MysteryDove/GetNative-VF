# Local benchmark fixture

`6.2-1.png` is the real-image performance fixture used by the kernel benchmark.

`metal_kernel_matrix.json` includes unity controls plus Spline64 blur 1.25 and
1.5 workloads. `getnative_fixed_recipe_benchmark` accepts the equivalent
`--blur SCALE` argument; omitted blur remains exactly 1.0.

Regenerate the dsmvc blur oracle from the pinned local checkout with:

```sh
cmake -S engine -B build/fixture-export \
  -DDSMVC_ROOT="$HOME/Documents/vapoursynth-descalemvc"
cmake --build build/fixture-export --target dsmvc_blur_fixture_export
build/fixture-export/dsmvc_blur_fixture_export \
  > engine/bench/fixtures/dsmvc_blur_plans.json
```

The dsmvc planner in commit `652cc95` is inverse-only. The fixture therefore
labels inverse arrays as dsmvc oracle data and forward arrays as GetNative-VF's
retained zimg-style projection; it does not misattribute forward data to dsmvc.
It is intentionally excluded from version control because redistribution rights
have not been established.

Each performance worktree must place the same locally authorized file at this
path and verify it before recording results:

```sh
cd engine/bench/fixtures
shasum -a 256 -c 6.2-1.png.sha256
```

Expected properties: 1920 x 1080, 8-bit RGB PNG.

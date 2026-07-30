# Local benchmark fixture

`6.2-1.png` is the real-image performance fixture used by the kernel benchmark.
It is intentionally excluded from version control because redistribution rights
have not been established.

Each performance worktree must place the same locally authorized file at this
path and verify it before recording results:

```sh
cd engine/bench/fixtures
shasum -a 256 -c 6.2-1.png.sha256
```

Expected properties: 1920 x 1080, 8-bit RGB PNG.

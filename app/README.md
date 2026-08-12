# GetNative VF GUI

Tauri 2 + React/TypeScript desktop workbench for the standalone C++ engine.

The frontend has no general shell permission. Rust owns typed Project
persistence, application preferences, and engine commands. React receives only
those typed commands.

## Current GUI surface

- **Project Hub** is the first screen: New Project, Open Project, Quick
  Analysis (untitled recoverable Project), recent list, and recovery.
- **Project shell** destinations: Overview, Media, Samples, Analyze, Verify,
  Results, plus Settings and Diagnostics.
- Pages without a real engine capability show an explicit blocked/empty state
  and never expose runnable fake analysis, media, or result controls.
- Existing `engine_capabilities` and `engine_geometry` live under Diagnostics.
- Locales: Simplified Chinese (`zh-CN`, first-launch default) and English
  (`en`). Language is an application preference, not Project state.
- Project storage uses a versioned schema-1 manifest behind a neutral storage
  boundary (file vs future bundle is not baked into page components).
- Media accepts mixed still/video imports, native drag and drop, real probing,
  still preview, and silent exact-frame browsing when the media sidecar is
  available.
- Samples preserve Source, stream, frame number, PTS, timebase, timestamp,
  include state, order, and tags in the Project manifest.
- Analysis controls remain capability-gated while the engine reports
  `commands.analyze=false`; no synthetic curves or Runs are created.

## Development

### Media runtime on macOS

Video probing and frame browsing use the engine's in-process FFmpeg libraries.
Build and stage the pinned LGPL-only configuration with:

```sh
npm run stage:ffmpeg:macos
```

The command verifies the source SHA-256, rejects non-system dynamic
dependencies, and stages the required `libavformat`, `libavcodec`, `libavutil`,
and `libswscale` libraries together with licenses, build information, and the
corresponding source archive. It does not stage the `ffmpeg` or `ffprobe`
programs.

After staging, exercise the application probe/index/preview path with a
generated two-stream VFR fixture. Set `GETNATIVE_FIXTURE_FFMPEG` to a full
development FFmpeg build that includes `lavfi` and the native MPEG-4 encoder:

```sh
GETNATIVE_FIXTURE_FFMPEG=/absolute/path/to/ffmpeg npm run test:media:smoke:macos
```

The generated fixture is temporary and is not included in the application
bundle.

```sh
cd app
npm install
npm run tauri dev
```

Set `GETNATIVE_ENGINE_PATH` to use another engine binary.

Useful checks:

```sh
npm run build
npm run test:locale
cargo fmt --manifest-path src-tauri/Cargo.toml -- --check
cargo test --manifest-path src-tauri/Cargo.toml
```

`npm run test:locale` checks `zh-CN` / `en` resource-key parity via
`src/i18n/check-locale.mjs`. Pure frontend normalization helpers live in
`src/project/normalize.ts` with companion assertions in
`src/project/normalize.test.ts`.


Create a macOS application bundle with:

```sh
npm run tauri build -- --bundles app
```

The bundle includes the engine at `Contents/Resources/bin/getnative-engine`.
Code signing and notarization are release steps and are not performed by the
local development build.

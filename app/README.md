# GetNative VF GUI

Tauri 2 + React/TypeScript desktop workbench for the standalone C++ engine.
The frontend has no general shell permission. Rust locates the packaged or
development `getnative-engine` binary and exposes only typed capability and
geometry commands.

## Development

The Tauri command configures, builds, and stages the C++ engine automatically:

```sh
cd app
npm install
npm run tauri dev
```

Set `GETNATIVE_ENGINE_PATH` to use another engine binary.

Create a macOS application bundle with:

```sh
npm run tauri build -- --bundles app
```

The bundle includes the engine at `Contents/Resources/bin/getnative-engine`.
Code signing and notarization are release steps and are not performed by the
local development build.

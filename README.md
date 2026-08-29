# GetNative VF

**Find the true native resolution of any video — no VapourSynth, no Python, no scripts.**

GetNative VF is a standalone desktop app that detects the resolution a video
was originally produced at, then verifies that result across every frame of
the video. It is a complete, self-contained reimagining of the classic
`getnative` workflow: everything runs inside one app, with nothing to install
or configure.

## Download

Grab the latest package from
[Releases](https://github.com/MysteryDove/GetNative-VF/releases):

- **Windows** — portable ZIP; unzip and run `getnative-gui.exe`. Includes CUDA
  and Vulkan acceleration out of the box.
- **Linux** — `.deb` and AppImage from GitHub Releases.
- **macOS (Apple Silicon)** — unsigned `.app.zip` from GitHub Releases. Not
  notarized; Gatekeeper may require a right-click Open the first time.

## Features

- **Resolution scan** — test hundreds of candidate native resolutions per
  second and see the results as an interactive error curve. On an RTX 5080 the
  engine reaches about **3,600 candidates/s**.
- **Whole-video verification** — once you pick a result, verify it against
  every frame of the video at up to **1,600 fps**, with per-frame review so
  you can inspect exactly where the match holds or breaks.
- **Fractional refinement** — search beyond integer heights to pin down
  non-integer source resolutions.
- **Kernel comparison** — not sure which resizing kernel was used? Run
  bilinear, bicubic, Lanczos, and more side by side and compare.
- **Precise frame browser** — inspect any frame, crop region, or plane in
  silence and at full accuracy.
- **GPU acceleration everywhere** — CUDA on NVIDIA, Metal on macOS, Vulkan on
  Linux/Windows, with a fast SIMD CPU fallback. The app picks the best
  available backend automatically and always tells you which one is active.
- **Projects and recipes** — save your sources, lock in an analysis recipe,
  and reproduce any result later with full provenance.
- **Bilingual UI** — English and 简体中文.

## Why it exists

Upscaling is everywhere: streaming catalogs, fan releases, and "HD remasters"
routinely contain video that was produced at a lower resolution and stretched.
Tools like `getnative` can reveal the truth, but until now they required a
VapourSynth setup, Python, plugins, and hand-written `.vpy` scripts. GetNative
VF packages the same math — validated bit-for-bit against the reference
implementations — into a fast native engine and a focused desktop UI that
anyone can use.

## Under the hood

- **Hand-tuned SIMD kernels** — the core descale math runs on
  runtime-dispatched SSE2/AVX2 (and experimental AVX-512) code ported from the
  Descale-MVC project, and is validated bit-for-bit against the reference
  VapourSynth implementations.
- **A custom GPU scheduler** — instead of inheriting the frame-at-a-time
  limits of the VapourSynth plugin model, the native engine batches and tiles
  candidates with its own scheduler to keep the GPU saturated. That's how a
  scan reaches **~3,600 candidates/s** and whole-video verification reaches
  **~1,600 fps** on an RTX 5080.
- **Self-contained media layer** — video decoding is built in (pinned FFmpeg
  libraries, with NVDEC and Vulkan Video hardware decode where available), so
  no external tools ever run at runtime.

## System requirements

- Windows 10+, a recent Linux distribution (x64), or macOS 15+ on Apple Silicon
- Optional: any recent NVIDIA (CUDA), or Vulkan-capable, GPU for acceleration —
  the app works fine without one. macOS uses Metal.

## Supported Build Matrix

| Target | CPU | GPU | CI |
| --- | --- | --- | --- |
| macOS ARM64 (`macos-15`) | NEON | Metal | `engine-macos.yml` |
| Linux x86_64 (`ubuntu-24.04`) | SIMD/scalar | Vulkan when a device is available | `engine-linux.yml` |
| Linux ARM64 (`ubuntu-24.04-arm`) | NEON/scalar | Vulkan when a device is available | `engine-linux.yml` |
| Windows x86_64 | SIMD/scalar | CUDA/Vulkan | Existing package/remote workflow |

Linux ARM64 Vulkan builds use the distro packages `glslang-tools`,
`spirv-tools`, and `libvulkan-dev`. LunarG does not publish a Linux ARM64 SDK;
`glslangValidator` is accepted as the shader compiler fallback and SPIR-V is
architecture-independent. Runtime Vulkan conformance is skipped with the
existing `SKIP:` contract when no device is available. MoltenVK, Lavapipe
device testing, and Raspberry Pi v3dv evidence remain out of scope. The macOS
arm64 `.app.zip` is an unsigned CI bundle.

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.

---

*Building from source or contributing? See `docs/architecture.md` and the
developer notes in `app/README.md`.*

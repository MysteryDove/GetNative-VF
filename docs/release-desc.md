Check now uses adaptive hardware decoding by default across CUDA, Vulkan Video, and VideoToolbox. This update also adds per-candidate Blur controls and improves measured-result handoff, video navigation, and verification history.

## New Features

- Adaptive hardware decoding — Check starts with one decode session and can probe two or four according to workload, available memory, and safe video partitions. Short or analysis-bound jobs may stay at one; no manual session-count setting is required.
- Blur candidate controls — build Algorithm Test candidates with per-filter Blur values, including Bicubic B/C grids.
- Check metric inheritance — use metrics from Resolution Test or a local Check override without changing the saved Recipe. Fusion uses the metrics recorded by each Run.
- Higher Check concurrency — the frame-concurrency range is now 1–16, with a default of 8. GPU execution is capped at 8 in both the interface and engine.

## Bug Fixes

- Preserve the measured filter, including non-default Blur, when applying a candidate. Older results recover omitted parameters from their original input snapshots.
- Preserve historical verification Recipes when the active Recipe is edited or deleted, so completed Runs and Fusion keep their original inputs.
- Restore live Check curves on macOS WebKit and keep running-result coverage up to date.
- Show relative video time and seek correctly in files with non-zero timestamp origins.
- Fix successful candidate additions reporting a duplicate, and reject oversized kernel grids before exceeding the engine's 4096-candidate limit.
- Preserve reference phase and border behavior when planner cache replay encounters half-pixel ties.
- Fix Vulkan frame-lock ownership, shared-device features, image/view usage, and synchronization between decoding and analysis.
- Improve Fusion legends, visibility controls, and frame navigation; avoid plot extent failures on large result sets.

## Performance

- Separate Vulkan compute and decode queues where supported, release decode surfaces after conversion, and specialize small inverse bandwidths.
- Share decode-demand sampling, memory budgeting, and session adjustment across the three hardware Check paths.
- In a controlled Linux RTX 5080 test on a 34,072-frame H.264 video, automatic Vulkan decoding measured 1161.6 fps versus 807.2 fps with one fixed session, with exact frame-record parity. This is a configuration-specific comparison, not a general speedup over v0.2.2. See [the experiment record](https://github.com/MysteryDove/GetNative-VF/blob/v0.2.3/docs/performance/vulkan-optimization-experiments.md).
- Avoid constructing resident GPU analysis engines just to report capabilities.

## Packaging

- Apply FFmpeg Vulkan bitstream-padding and image-view-usage fixes in the Linux and Windows SDK builds, with patch changes included in cache keys.
- Verify VAAPI capability entries on Linux and D3D11VA entries on Windows alongside NVDEC and Vulkan Video.
- Continue shipping Windows x64 portable ZIP, Linux x64 `.deb` / AppImage, and macOS arm64 `.app.zip` packages.
- Remove unused starter assets and refresh developer documentation.

## Notes

- Existing project files remain on schema version 2. Worker timestamp additions preserve absolute-time behavior by default; the GUI requests relative time explicitly.
- Vulkan Check uses Vulkan Video when available and falls back to software decoding otherwise. Software decoding and preview remain single-session.
- Builders can disable adaptive decoding with `GETNATIVE_ENABLE_ADAPTIVE_DECODE=OFF`. Internal Vulkan plan caching remains off by default.
- Adaptive multi-session runtime coverage is still configuration-specific; Windows runtime acceptance remains open. Clean Vulkan validation results used an isolated patched Validation Layer and do not establish an upstream fix.
- The macOS app is unsigned and not notarized; see the Gatekeeper instructions below.
- Linux AppImage needs host WebKitGTK 4.1. On Ubuntu, the `.deb` installs this dependency through apt.

## macOS: unsigned app (Gatekeeper / xattr)

The arm64 `.app.zip` is unsigned and not notarized. After unzip, macOS tags the bundle with `com.apple.quarantine`, so double-click may show “GetNative VF is damaged” or “can’t be opened because it is from an unidentified developer.”

Clear extended attributes on the unpacked app (adjust the path):

```bash
xattr -cr "GetNative VF.app"
```

`-c` clears all xattrs; `-r` walks the whole `.app` bundle. Gatekeeper only needs `com.apple.quarantine` gone; `-cr` is the usual one-liner for a freshly unzipped unsigned app. Then open it normally, or:

```bash
open "GetNative VF.app"
```

If Finder still blocks it: Control-click the app → Open → Open. You only need this once per download.

Do this on the `.app`, not only the zip. Gatekeeper often re-applies quarantine when you unzip into Downloads.

## Linux: install WebKitGTK 4.1

GetNative / Tauri 2 links `libwebkit2gtk-4.1.so.0` (not 4.0 or 6.0). The `.deb` pulls this in automatically. The AppImage does **not** bundle GTK/WebKit; install the host library first.

Ubuntu / Debian:

```bash
sudo apt update
sudo apt install libwebkit2gtk-4.1-0
```

Ubuntu 24.04+ AppImages may also need FUSE:

```bash
sudo apt install libfuse2t64 || sudo apt install libfuse2
```

Fedora:

```bash
sudo dnf install webkit2gtk4.1
```

Arch:

```bash
sudo pacman -S webkit2gtk-4.1
```

openSUSE:

```bash
sudo zypper install libwebkit2gtk-4_1-0
```

GNOME desktops often already have 4.1. KDE Plasma usually does not. After install, run the AppImage again. If it still says WebKitGTK is missing, `ldconfig -p | grep libwebkit2gtk-4.1` should list `libwebkit2gtk-4.1.so.0`.

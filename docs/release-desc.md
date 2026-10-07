_There are two colours in my head_

This release adds linear-light analysis and puts every backend on the same sample scale, so CPU, CUDA, Vulkan, and Metal now report the same error for the same video.

Resolution Test gets explicit Integer / Non-integer scan modes, Algorithm Test is renamed Kernel Search and gains presets, Blur sweeps, and a Bicubic heatmap, and Runs can be filtered and deleted from one shared selector.

> **Results are not comparable with 0.2.4 for studio-range video.** On CPU, CUDA, and Vulkan, errors measured without a transfer curve are about 1.17× higher than before. Re-run the tests you want to compare.

## New Features

- Transfer curves — run Resolution Test, Kernel Search, and Check in linear light with `gamma22`, `bt1886`, `srgb`, or `bt709`. Results are grouped by curve, and applying a result stores the curve in the Recipe so Check runs under it.
- Integer / Non-integer scan modes — these replace the search preset in Resolution Test. Integer rounds typed decimals and sends no base; Non-integer keeps decimals and requires an even/odd base on the scanned axis. The resolved base values are shown under the selectors.
- H+W base width "auto" — the width base follows the height and takes the parity of the source width.
- Non-proportional geometry — apply separate height-only and width-only results together as one Recipe.
- Kernel Search presets — the default list grows from 6 to 15 entries: Mitchell, B-spline, sharp `(0, 1)`, `(0, 0.75)`, and the three Robidoux cubics for Bicubic, plus Lanczos taps 2 and 4.
- Blur sweep — add kernels across a Blur start/stop/step range. Lanczos taps are also selectable for the fixed kernel.
- Bicubic heatmap — a regular b × c sweep renders as a heatmap with the lowest cell outlined. Scattered presets stay on the line plot.
- Rank kernels across Samples, and exclude a Sample or toggle its visibility from the test page.
- Shared Run selector — Resolution Test, Kernel Search, and Check label each Run with what it measured. Right-click deletes one Run and the trash button deletes the listed Runs. Results gains per-test delete buttons and group summaries.
- Multi-select Runs and Source filters on Resolution Test, Kernel Search, Check, and Results, plus a new Sample filter. The Resolution Test result table now follows the plot's filters and legend toggles.
- Frame navigation — Shift+←/→ steps through keyframes, the frame number field follows timeline drags and clamps typed values, and there is a go-to-frame button.
- Parameter drafts are saved with the project.

## Bug Fixes

- Analyse studio-range video on nominal 0..1 samples on every path. Software decode, CUDA, and Vulkan previously used the decoder's code scale while Metal stretched 16..235, so the same video measured about 1.169× higher on Metal.
- Keep excursions outside 16..235 on all backends; Metal no longer clamps them.
- Request a VideoToolbox surface that matches the stream's bit depth and range. 10-bit sources were narrowed to 8 bits before Metal saw them, which raised the Check error floor on a lossless 10-bit clip from 1.1e-4 to 1.1e-3.
- Keep the width base parity stable in H+W non-integer scans. With the base on auto, parity moved with the scan range, so one candidate could be measured on two different width canvases.
- Carry the width parity a scan used when applying an H+W result, so the Recipe matches the measured geometry.
- Restore CUDA telemetry counters, which read 0 unless stage profiling was enabled.
- Fix `CUDA_ERROR_CONTEXT_IS_DESTROYED` when a recreated engine received the same context handle as a destroyed one.
- List only real parameters in kernel labels; compare kernels no longer copy capability descriptors such as `kind=none`.
- Show at most four decimals in parameter labels. The engine still receives the exact values.
- Reject Blur values above 16 and handle non-finite pixel values in metric calculations.
- Keep Shift+arrow keyframe jumps working after dragging the timeline or clicking a filmstrip thumbnail, and keep the selected frame centred in the filmstrip.
- Activate file drop only when file paths are dragged into the window.

## Packaging

- Download `getnative-vf-0.2.5-windows-x64-portable.zip` from this release's Assets, together with `SHA256SUMS-windows-x64.txt`.
- Continue shipping Windows x64 portable ZIP, Linux x64 `.deb` / AppImage, and macOS arm64 `.app.zip` packages.
- Build the packages weekly so CUDA, Vulkan, and bundling breakage shows up before a release tag. Scheduled runs do not create a release.
- Run the Linux CPU-only engine tests and a Rust formatting check on pull requests.
- Key the FFmpeg caches on the patches the build scripts apply, pin the Rust toolchain action to a commit, give every job a timeout, and require a full version in the release tag.

## Notes

- Existing project files remain on schema version 2. Recipes now persist the transfer curve.
- Curve-less video Runs measured before this release are hidden, because their errors are on the old sample scale. H+W Runs whose derived width parity differs from the source are hidden from overlays; applying them still carries the parity they were measured with.
- The worker protocol adds an optional `transfer` on analyze and media verify, `frame_asset.range`, and a `sample_scale: "nominal"` echo in results. Support is advertised as `features.analysis_transfer` and `features.verify_transfer`. `verify_begin` with client-pushed frames defaults `transfer_range` to full. See `docs/worker-protocol-v1.md`.
- Check refuses a transfer curve the worker cannot apply.
- Algorithm Test is now called Kernel Search in the English interface.
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

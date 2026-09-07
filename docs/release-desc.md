# GetNative VF 0.2.3 — changes since v0.2.2

This update improves Check correctness and Vulkan video execution, adds per-candidate blur controls, and fixes result handoff, timeline navigation, and historical recipe preservation. Metal/VideoToolbox support, Vulkan compute, the keep-alive shell, and the Media/sample layout were already present in v0.2.2.

## Analysis and result correctness

- Add Blur to the Algorithm Test candidate builder, including Bicubic B/C grids.
- Preserve non-default blur in engine result echoes and use candidate ids for result selection. Applying a measured candidate now preserves the actual filter. Older results recover omitted parameters through their original candidate ids and input snapshots.
- Reject kernel lists above the engine's 4096-entry limit before constructing an oversized Cartesian grid; batch deduplication avoids repeatedly copying the growing list.
- Fix successful candidate additions incorrectly reporting a duplicate.
- Skip planner period-cache replay on half-pixel ties to preserve reference phase and border behavior.

## Check workflow and UI

- Check can inherit MetricSpec from Resolution Test or use a local override without editing the Recipe. Execution and Fusion use the effective metrics recorded by each Run.
- Store independent Recipe snapshots for verification. Editing or deleting the active Recipe preserves historical inputs and Fusion eligibility, including legacy metadata captured before modification.
- Raise the Check frame-concurrency range to 1..16 with a default of 8; GPU execution is capped at 8 in the UI and engine.
- Restore live Check curves on macOS WebKit by calling browser timers with the correct receiver. Running result cards update their live coverage.
- Display relative video time and translate time-based seeks correctly for files with non-zero PTS origins. Raw PTS remains available in the result data.
- Improve Fusion legends, visibility controls, and frame navigation; compatibility checks avoid building a per-frame union unnecessarily.
- Avoid spread-argument limits in plot extent calculations for large result sets.

## Vulkan decoding and packaging

- Vulkan Check selects Vulkan Video when usable and otherwise falls back to software decode. Hardware capabilities listed in Diagnostics are distinguished from each job's actual decoder and zero-copy provenance.
- Separate supported compute/decode queues, release decode surfaces after conversion, and fix frame-lock ownership across decoder and analysis threads.
- Correct shared Vulkan device features, image/view usage, layout barriers, and semaphore dependencies; specialize small inverse bandwidths.
- Apply and record FFmpeg Vulkan bitstream-padding and coincident-view-usage fixes in SDK builds. Update Linux/Windows media SDK and CI capability checks, including VAAPI/D3D11VA entries.
- Probe capabilities without unnecessarily constructing resident analysis engines.

## Experimental adaptive decoding

A shared indexed-range scheduler, demand sampler, memory budget, and 1→2→4 session controller are available behind internal test configuration. **Adaptive decoding remains disabled in release defaults.** Vulkan plan caching also remains off by default; shader experiments without measurable gains were reverted.

Historical Linux RTX 5080 validation on a 34,072-frame H.264 workload measured 807.2 fps fixed-single versus 1161.6 fps automatic, with exact frame-record parity. This compares two configurations of the repaired experimental build; it is not a general v0.2.2-to-0.2.3 performance guarantee. See [the retained experiment record](performance/vulkan-optimization-experiments.md).

Full Vulkan core/sync validation used an isolated patched Validation Layer with a positive canary. This does not establish that stock/upstream VVL is fixed. Persistent adjacent chunk scheduling, additional failure injection, profile-specific DPB estimates, Windows runtime acceptance, and default enablement remain open.

## Validation and compatibility

- Local frontend: 180 tests passed; production build and locale-key checks passed.
- Local engine: CTest 20/20 passed with the documented FFmpeg runtime environment. Rust: 53 passed, 3 fixture-dependent tests ignored.
- Actual macOS GUI: 34,072/34,072 frames completed without failures using a blur=1.1 Recipe; relative 10-second navigation selected frame 240 correctly, and live curves updated during execution.
- Nine supplied PNGs: before/after CPU and Metal height-scan results were unchanged for each backend; all updated kernel echoes preserved blur. This does not claim CPU/Metal bitwise equality to each other.
- Project schema remains version 2. Worker protocol additions preserve absolute timestamp semantics by default; the GUI explicitly requests relative time.
- The latest GUI/protocol fixes have not been runtime-tested on Linux or Windows. Existing bundle-size warnings remain.

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

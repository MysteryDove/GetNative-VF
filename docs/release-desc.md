macOS is now a first-class release: Metal analysis, VideoToolbox decode, and an Apple Silicon app bundle. Windows and Linux packages also pick up a Vulkan compute backend. The desktop app folds sample picking into Media, keeps project pages alive across navigation, and lets Check run more frames in flight.

## New Features
- macOS Metal analysis — the engine can run scans and verification on Apple Silicon GPUs, with blur-aware planning on that path.
- VideoToolbox decode — macOS packages decode video in-process through VideoToolbox instead of relying on an external FFmpeg binary. Metal verify uses a persistent async VTDecompressionSession (zero-copy via IOSurface) instead of per-frame FFmpeg waits or RAP-split multi-session decode.
- Adaptive Metal whole-video verify — verification on Metal adjusts itself instead of using a one-size-fits-all schedule.
- macOS arm64 app — GitHub Releases now include an unsigned `GetNative VF_<version>_arm64.app.zip`.
- Vulkan compute backend — Windows and Linux packages can analyze on Vulkan when a device is available, alongside CUDA and CPU.
- Blur-aware planning — blur is part of the plan key and fixtures, so blurred recipes reproduce instead of silently mismatching.
- Media owns samples — the separate Samples page is gone. Add stills and video frames, include/exclude, and remove sources (with their frames) on Media.
- Keep-alive project shell — Overview, Media, Resolution Test, Algorithm Test, Check, Results, and Settings stay mounted. Switching pages is a short fade; plot zoom, recipe drafts, and the last media preview survive the round trip.
- Collapsible navigation — the sidebar collapses to icons and the page content resizes with it (180ms). Labels fade with the column instead of popping off.
- Resolution Test results — the table defaults to sort by error, keeps the top 20, and scrolls in place next to the plot.
- Algorithm Test kernel picker — kernels add as a family grid of chips, with Blur on the add form; a scan result can be set as the recipe kernel without a separate apply-from-list step.
- Check MetricSpec — inherits Resolution Test by default and can be unlinked for the scan without rewriting the Recipe.
- Check frame concurrency — default 8; CPU maximum 16, GPU (CUDA/Vulkan/Metal/auto-GPU) maximum 8.
- Custom menus and motion — native `<select>` / spinner widgets are replaced with in-app menus and the same short motion on buttons, dropdowns, and the nav collapse (avoids the Linux WebKit pointer-grab issue on more controls).
- Analysis chrome — tighter parameter blocks, recipe picker / apply-to-current-recipe dialog, result history, and a brand mark on the project chrome.

## Bug Fixes
- Rank a perfect single-point descale (error 0) over a nearby shallower multi-kernel valley.
- Preserve zimg half-pixel forward phases so geometry matches the reference.
- Keep Metal verify progress updates from breaking the CPU fallback path.
- Scope plot decimation and marker budget to the zoom window so dense curves stay readable.
- Make blur validation and planner `filter.cpp` safe under fast-math.
- Accept Vulkan `p_norm` 1..4 in the worker protocol.
- Restore engine builds on older Apple standard libraries, Xcode 16 stop tokens, and non-jthread toolchains.
- Allow Vulkan builds against distro headers that omit AV1 video symbols.
- Link CoreFoundation for native VideoToolbox decode.
- Replace WebKitGTK native number spinners and range sliders so Linux text fields do not grab the pointer.
- Let Quick Analysis replace the untitled recovery slot instead of failing with a project-mismatch error.
- Vulkan Check decodes with VAAPI (Linux Intel/AMD), D3D11VA (Windows), or NVDEC and copies luma to compute, instead of FFmpeg Vulkan Video.
- Clamp GPU Check concurrency to 8 (analysis slot limit) instead of erroring when the UI asked for more.
- Color Check fusion curves from the plot palette, with a legend checkbox, and avoid crashing full-video fusion plots (extent loops + numeric frame picker).
- Keep media preview frames when leaving the Media page; returning no longer re-decodes from scratch.
- Swallow a Tauri file-drop unlisten rejection when leaving Media (harmless console `listeners[eventId].handlerId` error).
- Stop the result-table scrollbar from hitching against scroll anchoring while virtualizing.
- Show the duplicate-sample notice only after Add, and clear it when the kernel family or parameters change.
- Drop the extra divider on the Apply to Current Recipe dialog.

## Performance
- Batch inverse rows through NEON on AArch64.
- Port Metal register lag windows and a packed plan arena from the DSMVC work.
- Gate GPU stage timers behind `GETNATIVE_GPU_STAGE_PROFILE` (off by default) and bind the CUDA analysis context once per thread.
- Pages you have already opened keep their plot layers; collapsing the nav does not relayout hidden Check/Analyze curves, and plot canvases wait until the width tween settles before reallocating.

## Packaging
- Pin the macOS FFmpeg SDK flow (VideoToolbox-enabled, self-contained dylibs).
- Enable NVDEC and Vulkan Video in the Linux packaged FFmpeg SDK, matching Windows hwaccel, and allow intra-FFmpeg `$ORIGIN` NEEDED entries in the SDK closure.
- Fix Windows packaged FFmpeg shipping without NVDEC/Vulkan Video (MSYS2 path checks never saw the SDK headers, so CUDA verify fell back to software decode). Fail CI if the Windows SDK omits hevc_nvdec/hevc_vulkan.
- Ship Linux `.deb` / AppImage and Windows portable ZIP with CUDA + Vulkan. The AppImage is a thin package: GTK, WebKitGTK, GLib, and Wayland come from the host so GNOME/KDE input methods and AT-SPI match the session. FFmpeg and the pinned Vulkan loader stay in the image. AppImage uses system patchelf and pinned linuxdeploy tools.
- Windows packages use the prebuilt Vulkan SDK instead of a source-built loader.

## Notes
- The macOS build is not notarized. Gatekeeper may require a right-click → Open the first time, or clear quarantine with `xattr` (see below).
- Linux AppImage needs host WebKitGTK 4.1. Prefer the `.deb` on Ubuntu (apt installs WebKitGTK for you).
- The interface language is English for this release (the language control is kept, disabled, for compatibility).
- Compatibility profile is muvsfunc getnative (`muf-d278cd3`) only. Other profile ids in older manifests are coerced to that contract.
- Existing 0.2.1 project files still open. Sample lists now live on Media; Check concurrency in new runs is 1–16 on CPU and 1–8 on GPU.

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

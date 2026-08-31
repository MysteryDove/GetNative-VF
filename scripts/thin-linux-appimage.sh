#!/usr/bin/env bash
# Rewrite a Tauri AppImage so GTK/WebKit/GLib/Wayland come from the host.
# Keep the engine, FFmpeg, and the pinned Vulkan loader.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 path/to/GetNative.AppImage" >&2
  exit 1
fi

appimage=$(readlink -f "$1")
if [[ ! -f "$appimage" ]]; then
  echo "thin-linux-appimage: not a file: $appimage" >&2
  exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf -- "$workdir"' EXIT
cp -- "$appimage" "$workdir/in.AppImage"
chmod +x "$workdir/in.AppImage"

offset=$("$workdir/in.AppImage" --appimage-offset)
if ! [[ "$offset" =~ ^[0-9]+$ ]] || (( offset < 4096 )); then
  echo "thin-linux-appimage: invalid AppImage offset: $offset" >&2
  exit 1
fi

(
  cd "$workdir"
  ./in.AppImage --appimage-extract >/dev/null
)
root="$workdir/squashfs-root"
if [[ ! -d "$root" ]]; then
  echo "thin-linux-appimage: --appimage-extract did not produce squashfs-root" >&2
  exit 1
fi

# Anything GIO/GTK/WebKit will dlopen must come from the session. linuxdeploy
# puts 22.04 copies on LD_LIBRARY_PATH/RPATH ahead of Ubuntu 26.04 (libmount
# MOUNT_2_40, atk-bridge, glib, wayland, …). Keep FFmpeg anywhere, and the
# pinned Vulkan loader only next to the engine in bin/ so WebKit uses host
# libvulkan.
keep_so() {
  local path=$1 base
  base=$(basename "$path")
  case "$base" in
    libavformat.so*|libavcodec.so*|libavutil.so*|libswscale.so*)
      return 0
      ;;
    libvulkan.so*)
      [[ "$path" == */bin/* ]] && return 0
      return 1
      ;;
  esac
  return 1
}
while IFS= read -r -d '' path; do
  if keep_so "$path"; then
    continue
  fi
  rm -f -- "$path"
done < <(find "$root" \( -type f -o -type l \) \( -name '*.so' -o -name '*.so.*' \) -print0)

# Also drop directory trees the GTK/GStreamer plugins copy wholesale.
mapfile -d '' gtk_dirs < <(find "$root" -type d \( \
  -name gtk-3.0 -o -name gtk-4.0 -o \
  -name webkit2gtk-4.1 -o -name webkit2gtk-4.0 -o \
  -name gdk-pixbuf-2.0 -o -name gstreamer-1.0 \
\) -print0)
for dir in "${gtk_dirs[@]}"; do
  [[ -n "$dir" ]] || continue
  rm -rf -- "$dir"
done
rm -rf -- "$root"/usr/lib/*/gio/modules "$root"/usr/lib/gio/modules

# Helper binaries WebKit loads from hardcoded relative paths.
find "$root" -type f \( \
  -name WebKitWebProcess -o -name WebKitNetworkProcess -o \
  -name WebKitGTKInjectedBundle.so -o -name libwebkit2gtkinjectedbundle.so \
\) -delete

# AppRun sources linuxdeploy-plugin-gtk.sh by absolute path. Replace it
# in place: do not delete (startup would fail) and do not keep GDK_BACKEND=x11.
mkdir -p "$root/apprun-hooks"
cat > "$root/apprun-hooks/linuxdeploy-plugin-gtk.sh" <<'HOOK'
# Host WebKitGTK 4.1 is required. This AppImage does not bundle GTK/WebKit
# so GNOME/KDE input methods and AT-SPI match the session.
webkit_found=0
for candidate in \
  /usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0 \
  /usr/lib64/libwebkit2gtk-4.1.so.0 \
  /usr/lib/libwebkit2gtk-4.1.so.0 \
  /lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0
do
  if [ -e "$candidate" ]; then
    webkit_found=1
    break
  fi
done
if [ "$webkit_found" -eq 0 ] && command -v ldconfig >/dev/null 2>&1; then
  ldconfig -p 2>/dev/null | grep -q 'libwebkit2gtk-4.1.so.0' && webkit_found=1
fi
if [ "$webkit_found" -eq 0 ]; then
  echo "GetNative VF needs WebKitGTK 4.1 from the host system." >&2
  echo "  Ubuntu/Debian:  sudo apt install libwebkit2gtk-4.1-0" >&2
  echo "  Fedora:         sudo dnf install webkit2gtk4.1" >&2
  echo "  Arch:           sudo pacman -S webkit2gtk-4.1" >&2
  echo "  openSUSE:       sudo zypper install libwebkit2gtk-4_1-0" >&2
  exit 1
fi
HOOK
if [[ -f "$root/apprun-hooks/linuxdeploy-plugin-gstreamer.sh" ]]; then
  printf '# gstreamer plugins are not bundled in the thin AppImage\n' \
    > "$root/apprun-hooks/linuxdeploy-plugin-gstreamer.sh"
fi

# DT_RPATH on the GUI binary is inherited by host libgio and would still
# prefer $APPDIR/usr/lib. The engine keeps $ORIGIN for FFmpeg/Vulkan.
gui=$(find "$root" -type f -name getnative-gui -print -quit)
if [[ -n "$gui" ]] && command -v patchelf >/dev/null 2>&1; then
  patchelf --remove-rpath "$gui" || patchelf --set-rpath '' "$gui" || true
fi

# linuxdeploy's AppRun prepends $APPDIR/usr/lib after hooks run. Strip those
# entries so leftover libraries cannot shadow GIO/GTK/WebKit.
python3 - "$root/AppRun" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
cleanup = r'''
# Thin AppImage: never let $APPDIR/usr/lib shadow host GIO/GTK/WebKit.
if [ -n "${LD_LIBRARY_PATH:-}" ] && [ -n "${APPDIR:-}" ]; then
  filtered=
  old_ifs=$IFS
  IFS=:
  for dir in $LD_LIBRARY_PATH; do
    case "$dir" in
      "$APPDIR"/usr/lib|"$APPDIR"/usr/lib/*|"$APPDIR"/usr/lib64|"$APPDIR"/usr/lib64/*|"$APPDIR"/lib|"$APPDIR"/lib/*)
        continue
        ;;
    esac
    if [ -n "$filtered" ]; then
      filtered="$filtered:$dir"
    else
      filtered="$dir"
    fi
  done
  IFS=$old_ifs
  if [ -n "$filtered" ]; then
    export LD_LIBRARY_PATH="$filtered"
  else
    unset LD_LIBRARY_PATH
  fi
fi
'''
idx = text.rfind("\nexec ")
if idx == -1:
    idx = text.rfind("\nexec\t")
if idx == -1:
    text = text.rstrip() + "\n" + cleanup
else:
    text = text[: idx + 1] + cleanup + text[idx + 1 :]
path.write_text(text)
PY

dd if="$workdir/in.AppImage" of="$workdir/runtime" bs="$offset" count=1 status=none
# The Type-2 runtime shipped with linuxdeploy-plugin-appimage only mounts
# zlib/gzip and zstd images. xz packs but then --appimage-extract fails.
mksquashfs "$root" "$workdir/fs.squash" -comp gzip -all-root -noappend -no-progress >/dev/null
cat "$workdir/runtime" "$workdir/fs.squash" > "$appimage"
chmod +x "$appimage"

echo "thin-linux-appimage: rewrote $appimage (host WebKitGTK 4.1)"

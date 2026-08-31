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

# Session-integration libraries must match the desktop (GNOME/KDE, IME, AT-SPI).
# Bundling the build-machine copies is what froze Ubuntu 26.04.
remove_patterns=(
  'libgtk-3.so*'
  'libgtk-4.so*'
  'libgdk-3.so*'
  'libgdk-4.so*'
  'libwebkit2gtk*'
  'libjavascriptcoregtk*'
  'libsoup-2*'
  'libsoup-3*'
  'libglib-2.0.so*'
  'libgio-2.0.so*'
  'libgobject-2.0.so*'
  'libgmodule-2.0.so*'
  'libgthread-2.0.so*'
  'libwayland-*.so*'
  'libatk-1.0.so*'
  'libatk-bridge-2.0.so*'
  'libatspi.so*'
  'libpango*.so*'
  'libpangocairo*.so*'
  'libpangoft2*.so*'
  'libharfbuzz.so*'
  'libgdk_pixbuf-2.0.so*'
  'libepoxy.so*'
  'libcairo.so*'
  'libcairo-gobject.so*'
  'libgst*.so*'
  'libnotify.so*'
  'libayatana*.so*'
  'libdbusmenu*.so*'
  'libcloudproviders.so*'
  'libcolord.so*'
  'librest-*.so*'
  'libjson-glib-1.0.so*'
  'libsecret-1.so*'
)

find_args=()
for pattern in "${remove_patterns[@]}"; do
  find_args+=(-o -name "$pattern")
done
while IFS= read -r -d '' path; do
  rm -f -- "$path"
done < <(find "$root" -type f \( -false "${find_args[@]}" \) -print0)

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

# The stock gtk hook forces GDK_BACKEND=x11 and prepends the host GTK_PATH.
# The gstreamer hook pins a private plugin registry. Neither belongs in a thin image.
rm -f -- \
  "$root/apprun-hooks/linuxdeploy-plugin-gtk.sh" \
  "$root/apprun-hooks/linuxdeploy-plugin-gstreamer.sh"

mkdir -p "$root/apprun-hooks"
cat > "$root/apprun-hooks/getnative-webkit-host.sh" <<'HOOK'
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

dd if="$workdir/in.AppImage" of="$workdir/runtime" bs="$offset" count=1 status=none
# The Type-2 runtime shipped with linuxdeploy-plugin-appimage only mounts
# zlib/gzip and zstd images. xz packs but then --appimage-extract fails.
mksquashfs "$root" "$workdir/fs.squash" -comp gzip -all-root -noappend -no-progress >/dev/null
cat "$workdir/runtime" "$workdir/fs.squash" > "$appimage"
chmod +x "$appimage"

echo "thin-linux-appimage: rewrote $appimage (host WebKitGTK 4.1)"

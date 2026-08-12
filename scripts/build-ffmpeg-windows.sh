#!/usr/bin/env bash
set -euo pipefail

ffmpeg_version="8.1.2"
ffmpeg_sha256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
archive="ffmpeg-${ffmpeg_version}.tar.xz"
url="https://ffmpeg.org/releases/${archive}"
zlib_version="1.3.1"
zlib_archive="zlib-${zlib_version}.tar.gz"
zlib_sha256="17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c"
zlib_url="https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v${zlib_version}"

if [[ $# -ne 1 ]]; then
  echo "usage: $0 OUTPUT_SDK_DIRECTORY" >&2
  exit 2
fi
if ! command -v cl.exe >/dev/null 2>&1; then
  echo "build-ffmpeg-windows.sh requires an initialized MSVC x64 environment" >&2
  exit 1
fi
if ! command -v nmake.exe >/dev/null 2>&1; then
  echo "build-ffmpeg-windows.sh requires nmake.exe" >&2
  exit 1
fi

# FFmpeg passes native MSVC switches through MSYS2. Do not rewrite /MT, /I,
# or /LIBPATH arguments as if they were Unix paths.
export MSYS2_ARG_CONV_EXCL='*'

mkdir -p "$(cygpath -u "$1")"
sdk_dir=$(CDPATH= cd -- "$(cygpath -u "$1")" && pwd)
sdk_windows=$(cygpath -m "$sdk_dir")

work_dir=$(mktemp -d "${TMP:-/tmp}/getnative-ffmpeg-windows.XXXXXX")
cleanup() {
  case "$work_dir" in
    */getnative-ffmpeg-windows.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

curl --fail --location --retry 5 --retry-all-errors \
  --output "${work_dir}/${archive}" "$url"
actual_sha256=$(sha256sum "${work_dir}/${archive}" | awk '{print $1}')
if [[ "$actual_sha256" != "$ffmpeg_sha256" ]]; then
  echo "FFmpeg source checksum mismatch: ${actual_sha256}" >&2
  exit 1
fi
curl --fail --location --retry 5 --retry-all-errors \
  --output "${work_dir}/${zlib_archive}" "$zlib_url"
actual_zlib_sha256=$(sha256sum "${work_dir}/${zlib_archive}" | awk '{print $1}')
if [[ "$actual_zlib_sha256" != "$zlib_sha256" ]]; then
  echo "zlib source checksum mismatch: ${actual_zlib_sha256}" >&2
  exit 1
fi

tar -xf "${work_dir}/${archive}" -C "$work_dir"
tar -xf "${work_dir}/${zlib_archive}" -C "$work_dir"
zlib_dir="${work_dir}/zlib-${zlib_version}"
# FFmpeg's Windows config defines HAVE_UNISTD_H to 0. zconf.h tests only
# whether it is defined, so make that test honor the value before MSVC uses it.
sed -i 's/^#ifdef HAVE_UNISTD_H/#if defined(HAVE_UNISTD_H) \&\& HAVE_UNISTD_H/' \
  "${zlib_dir}/zconf.h"
(
  cd "$zlib_dir"
  nmake.exe -f win32/Makefile.msc zlib.lib CFLAGS="-nologo -MT -W3 -O2 -Oy-"
)
zlib_windows=$(cygpath -m "$zlib_dir")
export LIB="${zlib_windows};${LIB:-}"

cd "${work_dir}/ffmpeg-${ffmpeg_version}"
if ! ./configure \
  --prefix="$sdk_windows" \
  --toolchain=msvc \
  --arch=x86_64 \
  --target-os=win64 \
  --disable-autodetect \
  --disable-debug \
  --disable-doc \
  --disable-network \
  --disable-gpl \
  --disable-nonfree \
  --disable-version3 \
  --disable-programs \
  --disable-static \
  --enable-shared \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swresample \
  --disable-everything \
  --enable-w32threads \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-zlib \
  --extra-cflags="/MT /I${zlib_windows}" \
  --extra-ldflags="/LIBPATH:${zlib_windows}" \
  --enable-protocol=file,pipe \
  --enable-demuxer=avi,flv,h264,matroska,mov,mpegps,mpegts,mpegvideo,ogg,rawvideo \
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3 \
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vp8,vp9 \
  --enable-encoder=png; then
  tail -200 ffbuild/config.log >&2
  exit 1
fi

make -j "${NUMBER_OF_PROCESSORS:-4}"
make install

legal_dir="${sdk_dir}/share/ffmpeg"
mkdir -p "${legal_dir}/source"
cp "${work_dir}/${archive}" "${legal_dir}/source/${archive}"
cp COPYING.LGPLv2.1 LICENSE.md "${legal_dir}/"
cp ffbuild/config.mak "${legal_dir}/BUILD_INFO.txt"
mkdir -p "${legal_dir}/zlib/source"
cp "${work_dir}/${zlib_archive}" "${legal_dir}/zlib/source/${zlib_archive}"
cp "${zlib_dir}/LICENSE" "${legal_dir}/zlib/LICENSE"
printf 'zlib_version=%s\nzlib_sha256=%s\npatch=zconf HAVE_UNISTD_H value check\n' \
  "$zlib_version" "$zlib_sha256" > "${legal_dir}/zlib/BUILD_INFO.txt"

for library in avformat-62.dll avcodec-62.dll avutil-60.dll swscale-9.dll; do
  test -f "${sdk_dir}/bin/${library}" || {
    echo "FFmpeg SDK is missing ${library}" >&2
    exit 1
  }
done
test ! -e "${sdk_dir}/bin/ffmpeg.exe"
test ! -e "${sdk_dir}/bin/ffprobe.exe"

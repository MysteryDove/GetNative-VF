#!/bin/sh
# Builds the pinned FFmpeg 8.1.2 SDK (include/ + lib/ + legal materials) for
# macOS engine builds, mirroring scripts/build-ffmpeg-linux.sh. The engine
# links these dylibs directly and resolves them via @loader_path at runtime,
# so their install names are rewritten the same way stage_ffmpeg_macos.sh
# does for the packaging runtime.
set -eu

ffmpeg_version="8.1.2"
ffmpeg_sha256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
archive="ffmpeg-${ffmpeg_version}.tar.xz"
url="https://ffmpeg.org/releases/${archive}"

if [ "$#" -ne 1 ]; then
  echo "usage: $0 OUTPUT_SDK_DIRECTORY" >&2
  exit 2
fi

sdk_dir=$(mkdir -p "$1" && CDPATH= cd -- "$1" && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/getnative-ffmpeg-macos.XXXXXX")
cleanup() {
  case "$work_dir" in
    "${TMPDIR:-/tmp}"/getnative-ffmpeg-macos.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

curl --fail --location --retry 5 --retry-all-errors \
  --output "${work_dir}/${archive}" "$url"
actual_sha256=$(shasum -a 256 "${work_dir}/${archive}" | awk '{print $1}')
if [ "$actual_sha256" != "$ffmpeg_sha256" ]; then
  echo "FFmpeg source checksum mismatch: ${actual_sha256}" >&2
  exit 1
fi

tar -xf "${work_dir}/${archive}" -C "$work_dir"
cd "${work_dir}/ffmpeg-${ffmpeg_version}"
./configure \
  --prefix="$sdk_dir" \
  --cc=clang \
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
  --enable-videotoolbox \
  --disable-avdevice \
  --disable-swresample \
  --disable-everything \
  --enable-pthreads \
  --enable-avcodec \
  --enable-avfilter \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-zlib \
  --enable-protocol=file,pipe \
  --enable-demuxer=avi,flv,h264,matroska,mov,mpegps,mpegts,mpegvideo,ogg,rawvideo \
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3 \
  --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,prores_videotoolbox,vp9_videotoolbox,av1_videotoolbox,mpeg1_videotoolbox,mpeg2_videotoolbox,mpeg4_videotoolbox \
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vp8,vp9 \
  --enable-filter=scale,select \
  --enable-encoder=png \
  --enable-muxer=image2,image2pipe

make -j "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
make install

# The engine executable resolves these via @loader_path (its own directory);
# bundle-stage/bin receives copies next to getnative-engine at build time.
# libavfilter is not linked by the engine but is built by configure; rewrite
# it too so every SDK dylib stays self-consistent.
libraries="libavformat.62.dylib libavcodec.62.dylib libavutil.60.dylib libswscale.9.dylib"
all_libraries="$libraries libavfilter.11.dylib"
for library in $all_libraries; do
  install_name_tool -id "@loader_path/${library}" "${sdk_dir}/lib/${library}"
done
for library in $all_libraries; do
  for dependency in $all_libraries; do
    install_name_tool -change \
      "${sdk_dir}/lib/${dependency}" \
      "@loader_path/${dependency}" \
      "${sdk_dir}/lib/${library}" 2>/dev/null || true
  done
done

legal_dir="${sdk_dir}/share/ffmpeg"
mkdir -p "${legal_dir}/source"
cp "${work_dir}/${archive}" "${legal_dir}/source/${archive}"
cp COPYING.LGPLv2.1 LICENSE.md "${legal_dir}/"
cp ffbuild/config.mak "${legal_dir}/BUILD_INFO.txt"

for library in $libraries; do
  test -f "${sdk_dir}/lib/${library}" || {
    echo "FFmpeg SDK is missing ${library}" >&2
    exit 1
  }
done
test ! -e "${sdk_dir}/bin/ffmpeg"
test ! -e "${sdk_dir}/bin/ffprobe"

# Fail the SDK build closed if VideoToolbox was silently omitted by configure.
grep -Eq '^CONFIG_VIDEOTOOLBOX=yes$' "${legal_dir}/BUILD_INFO.txt"
for hwaccel in av1 h264 hevc mpeg1 mpeg2 mpeg4 prores vp9; do
  upper=$(printf '%s' "${hwaccel}" | tr '[:lower:]' '[:upper:]')
  grep -Eq "^CONFIG_${upper}_VIDEOTOOLBOX_HWACCEL=yes$" "${legal_dir}/BUILD_INFO.txt"
done

# Only indented lines are real dependency entries; otool also echoes the
# input paths as unindented headers, which would match the patterns below.
if otool -L $(for library in $all_libraries; do printf '%s ' "${sdk_dir}/lib/${library}"; done) \
  | grep -E "^[[:space:]]" \
  | grep -Eq "${work_dir}|${sdk_dir}|/opt/homebrew|/usr/local|@rpath"; then
  echo "FFmpeg SDK libraries contain a non-self-contained dynamic dependency" >&2
  exit 1
fi

echo "ffmpeg_sdk=${sdk_dir}"

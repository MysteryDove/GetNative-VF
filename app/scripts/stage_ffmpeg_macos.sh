#!/bin/sh
set -eu

ffmpeg_version="8.1.2"
ffmpeg_sha256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
ffmpeg_archive="ffmpeg-${ffmpeg_version}.tar.xz"
ffmpeg_url="https://ffmpeg.org/releases/${ffmpeg_archive}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
stage_dir="${app_dir}/src-tauri/bundle-stage"
runtime_dir="${app_dir}/src-tauri/ffmpeg-runtime"
work_dir=$(mktemp -d "/tmp/getnative-ffmpeg.XXXXXX")

cleanup() {
  case "$work_dir" in
    /tmp/getnative-ffmpeg.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

archive_path="${work_dir}/${ffmpeg_archive}"
source_dir="${work_dir}/ffmpeg-${ffmpeg_version}"
install_dir="${work_dir}/install"

curl --fail --location --retry 5 --retry-all-errors \
  --output "$archive_path" "$ffmpeg_url"

actual_sha256=$(shasum -a 256 "$archive_path" | awk '{print $1}')
if [ "$actual_sha256" != "$ffmpeg_sha256" ]; then
  echo "FFmpeg source checksum mismatch" >&2
  echo "expected: $ffmpeg_sha256" >&2
  echo "actual:   $actual_sha256" >&2
  exit 1
fi

tar -xf "$archive_path" -C "$work_dir"
cd "$source_dir"

./configure \
  --prefix="$install_dir" \
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
  --disable-avdevice \
  --disable-swresample \
  --disable-everything \
  --enable-avcodec \
  --enable-avfilter \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-zlib \
  --enable-protocol=file,pipe \
  --enable-demuxer=avi,flv,h264,matroska,mov,mpegps,mpegts,mpegvideo,ogg,rawvideo \
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3 \
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vp8,vp9 \
  --enable-filter=scale,select \
  --enable-encoder=png \
  --enable-muxer=image2,image2pipe

jobs=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
make -j "$jobs"
make install

mkdir -p "$runtime_dir" "${stage_dir}/bin" "${stage_dir}/share/ffmpeg/source"
rm -f "${runtime_dir}/ffmpeg" "${runtime_dir}/ffprobe" \
  "${stage_dir}/bin/ffmpeg" "${stage_dir}/bin/ffprobe"
libraries="libavformat.62.dylib libavcodec.62.dylib libavutil.60.dylib libswscale.9.dylib"
for library in $libraries; do
  cp "${install_dir}/lib/${library}" "${runtime_dir}/${library}"
  install_name_tool -id "@loader_path/${library}" "${runtime_dir}/${library}"
done
for library in $libraries; do
  for dependency in $libraries; do
    install_name_tool -change \
      "${install_dir}/lib/${dependency}" \
      "@loader_path/${dependency}" \
      "${runtime_dir}/${library}" 2>/dev/null || true
  done
done
cp "$archive_path" "${stage_dir}/share/ffmpeg/source/${ffmpeg_archive}"
cp "${source_dir}/COPYING.LGPLv2.1" "${stage_dir}/share/ffmpeg/COPYING.LGPLv2.1"
cp "${source_dir}/LICENSE.md" "${stage_dir}/share/ffmpeg/LICENSE.md"
cp "${source_dir}/ffbuild/config.mak" "${stage_dir}/share/ffmpeg/BUILD_INFO.txt"

if otool -L $(for library in $libraries; do printf '%s ' "${runtime_dir}/${library}"; done) \
  | grep -Eq "${work_dir}|/opt/homebrew|/usr/local|@rpath"; then
  echo "staged FFmpeg libraries contain a non-system dynamic dependency" >&2
  exit 1
fi

for library in $libraries; do
  echo "prepared ${runtime_dir}/${library}"
done

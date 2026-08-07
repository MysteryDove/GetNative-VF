#!/bin/sh
set -eu

ffmpeg_version="8.1.2"
ffmpeg_sha256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
ffmpeg_archive="ffmpeg-${ffmpeg_version}.tar.xz"
ffmpeg_url="https://ffmpeg.org/releases/${ffmpeg_archive}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
stage_dir="${app_dir}/src-tauri/bundle-stage"
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
  --disable-ffplay \
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
  --enable-demuxer=avi,flv,matroska,mov,mpegps,mpegts,mpegvideo,ogg,rawvideo \
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3 \
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vp8,vp9 \
  --enable-filter=scale,select \
  --enable-encoder=png \
  --enable-muxer=image2,image2pipe

jobs=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
make -j "$jobs"
make install

mkdir -p "${stage_dir}/bin" "${stage_dir}/share/ffmpeg/source"
cp "${install_dir}/bin/ffmpeg" "${stage_dir}/bin/ffmpeg"
cp "${install_dir}/bin/ffprobe" "${stage_dir}/bin/ffprobe"
cp "$archive_path" "${stage_dir}/share/ffmpeg/source/${ffmpeg_archive}"
cp "${source_dir}/COPYING.LGPLv2.1" "${stage_dir}/share/ffmpeg/COPYING.LGPLv2.1"
cp "${source_dir}/LICENSE.md" "${stage_dir}/share/ffmpeg/LICENSE.md"
"${stage_dir}/bin/ffmpeg" -buildconf > "${stage_dir}/share/ffmpeg/BUILD_INFO.txt" 2>&1

if otool -L "${stage_dir}/bin/ffmpeg" "${stage_dir}/bin/ffprobe" | grep -Eq '/opt/homebrew|/usr/local|@rpath'; then
  echo "staged FFmpeg binaries contain a non-system dynamic dependency" >&2
  exit 1
fi

"${stage_dir}/bin/ffmpeg" -hide_banner -version | sed -n '1,4p'
"${stage_dir}/bin/ffprobe" -hide_banner -version | sed -n '1,2p'

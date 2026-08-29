#!/bin/sh
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
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/getnative-ffmpeg-linux.XXXXXX")
cleanup() {
  case "$work_dir" in
    "${TMPDIR:-/tmp}"/getnative-ffmpeg-linux.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

curl --fail --location --retry 5 --retry-all-errors \
  --output "${work_dir}/${archive}" "$url"
actual_sha256=$(sha256sum "${work_dir}/${archive}" | awk '{print $1}')
if [ "$actual_sha256" != "$ffmpeg_sha256" ]; then
  echo "FFmpeg source checksum mismatch: ${actual_sha256}" >&2
  exit 1
fi

tar -xf "${work_dir}/${archive}" -C "$work_dir"
cd "${work_dir}/ffmpeg-${ffmpeg_version}"
./configure \
  --prefix="$sdk_dir" \
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
  --enable-pthreads \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-zlib \
  --enable-protocol=file,pipe \
  --enable-demuxer=avi,flv,h264,matroska,mov,mpegps,mpegts,mpegvideo,ogg,rawvideo \
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3 \
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vp8,vp9 \
  --enable-encoder=png

make -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make install

command -v patchelf >/dev/null 2>&1 || {
  echo "patchelf is required to make the FFmpeg runtime self-contained" >&2
  exit 1
}
for library in libavformat.so.62 libavcodec.so.62 libavutil.so.60 libswscale.so.9; do
  patchelf --set-rpath '$ORIGIN' "${sdk_dir}/lib/${library}"
done

legal_dir="${sdk_dir}/share/ffmpeg"
mkdir -p "${legal_dir}/source"
cp "${work_dir}/${archive}" "${legal_dir}/source/${archive}"
cp COPYING.LGPLv2.1 LICENSE.md "${legal_dir}/"
cp ffbuild/config.mak "${legal_dir}/BUILD_INFO.txt"
printf '\nGETNATIVE_POST_INSTALL_RPATH=$ORIGIN\n' >> "${legal_dir}/BUILD_INFO.txt"

for library in libavformat.so.62 libavcodec.so.62 libavutil.so.60 libswscale.so.9; do
  test -f "${sdk_dir}/lib/${library}" || {
    echo "FFmpeg SDK is missing ${library}" >&2
    exit 1
  }
done
test ! -e "${sdk_dir}/bin/ffmpeg"
test ! -e "${sdk_dir}/bin/ffprobe"

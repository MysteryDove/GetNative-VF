#!/usr/bin/env bash
set -euo pipefail

ffmpeg_version="8.1.2"
ffmpeg_sha256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
archive="ffmpeg-${ffmpeg_version}.tar.xz"
url="https://ffmpeg.org/releases/${archive}"
offline="${FFMPEG_OFFLINE:-0}"
nvch_version="12.2.72.0"

if [[ "$#" -ne 1 ]]; then
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
if [[ "$actual_sha256" != "$ffmpeg_sha256" ]]; then
  echo "FFmpeg source checksum mismatch: ${actual_sha256}" >&2
  exit 1
fi

tar -xf "${work_dir}/${archive}" -C "$work_dir"

# Hardware decode headers: nv-codec-headers (ffnvcodec) and the Vulkan SDK are
# external inputs. Each hwaccel family is enabled only when its headers are
# present, so CPU-only CI jobs without a Vulkan SDK keep working.
nvch_default="$(pwd)/.deps/nv-codec-headers"
if [[ "$offline" != "1" && -z "${NV_CODEC_HEADERS:-}" \
      && ! -f "${nvch_default}/include/ffnvcodec/dynlink_cuda.h" ]]; then
  git clone --depth 1 --branch "n${nvch_version}" --quiet \
    https://github.com/FFmpeg/nv-codec-headers.git "${nvch_default}-git" \
  && mkdir -p "${nvch_default}/include" \
  && cp -r "${nvch_default}-git/include/ffnvcodec" "${nvch_default}/include/" \
  && rm -rf "${nvch_default}-git" \
  || echo "note: nv-codec-headers fetch failed; NVDEC hwaccels disabled" >&2
fi
nvch_dir="${NV_CODEC_HEADERS:-$nvch_default}"

pc_dir="${work_dir}/pkgconfig"
mkdir -p "$pc_dir"
export PKG_CONFIG_PATH="${pc_dir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

prepend_pkg_config() {
  local dir=$1
  if [[ -d "$dir" ]]; then
    export PKG_CONFIG_PATH="${dir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  fi
}
if [[ -n "${VULKAN_SDK:-}" ]]; then
  prepend_pkg_config "${VULKAN_SDK}/lib/pkgconfig"
  prepend_pkg_config "${VULKAN_SDK}/lib64/pkgconfig"
fi

hwaccel_switches=()
extra_cflags=()
hwaccel_list=""
enabled_nvdec=0
enabled_vulkan=0
enabled_vaapi=0

if [[ -f "${nvch_dir}/include/ffnvcodec/dynlink_cuda.h" ]]; then
  # ffnvcodec lives in configure's HWACCEL_AUTODETECT list, so the global
  # --disable-autodetect would skip its probe entirely; enable it explicitly.
  hwaccel_switches+=(--enable-ffnvcodec --enable-cuda --enable-nvdec)
  extra_cflags+=("-I${nvch_dir}/include")
  hwaccel_list="av1_nvdec,h264_nvdec,hevc_nvdec,mjpeg_nvdec,mpeg1_nvdec,mpeg2_nvdec,mpeg4_nvdec,vc1_nvdec,vp8_nvdec,vp9_nvdec,wmv3_nvdec"
  enabled_nvdec=1
  cat > "${pc_dir}/ffnvcodec.pc" <<EOF
prefix=${nvch_dir}
includedir=\${prefix}/include

Name: ffnvcodec
Description: FFmpeg version of Nvidia codec headers
Version: ${nvch_version}
Cflags: -I\${includedir}
EOF
else
  echo "note: ffnvcodec headers missing; NVDEC hwaccels disabled" >&2
fi

vulkan_include=""
if [[ -n "${VULKAN_SDK:-}" && -f "${VULKAN_SDK}/include/vulkan/vulkan.h" ]]; then
  vulkan_include="${VULKAN_SDK}/include"
elif [[ -f /usr/include/vulkan/vulkan.h ]]; then
  vulkan_include=/usr/include
fi
if [[ -n "$vulkan_include" ]]; then
  # Decode hwaccels are header-only (libvulkan is loaded at runtime). Only
  # the header-only codecs; ffv1/prores/dpx additionally need a SPIR-V
  # compiler and the rest of the codec list has no Vulkan hwaccel.
  hwaccel_switches+=(--enable-vulkan)
  extra_cflags+=("-I${vulkan_include}")
  hwaccel_list+="${hwaccel_list:+,}av1_vulkan,h264_vulkan,hevc_vulkan,vp9_vulkan"
  enabled_vulkan=1
else
  echo "note: Vulkan SDK headers missing; Vulkan hwaccels disabled" >&2
fi
if pkg-config --exists libva 2>/dev/null; then
  # Host libva/libva-drm, same model as libvulkan: decode at runtime on
  # Intel/AMD. Copy-out to system memory is the Check ingest path.
  hwaccel_switches+=(--enable-vaapi)
  hwaccel_list+="${hwaccel_list:+,}h264_vaapi,hevc_vaapi,mpeg2_vaapi,vp9_vaapi"
  enabled_vaapi=1
else
  echo "note: libva missing; VAAPI hwaccels disabled" >&2
fi
if [[ -n "$hwaccel_list" ]]; then
  hwaccel_switches+=(--enable-hwaccel="$hwaccel_list")
fi

configure_args=(
  --prefix="$sdk_dir"
  --disable-autodetect
  --disable-debug
  --disable-doc
  --disable-network
  --disable-gpl
  --disable-nonfree
  --disable-version3
  --disable-programs
  --disable-static
  --enable-shared
  --disable-avdevice
  --disable-avfilter
  --disable-swresample
  --disable-everything
  --enable-pthreads
  --enable-avcodec
  --enable-avformat
  --enable-avutil
  --enable-swscale
  --enable-zlib
  --enable-protocol=file,pipe
  --enable-demuxer=avi,flv,h264,matroska,mov,mpegps,mpegts,mpegvideo,ogg,rawvideo
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vp8,vp9
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,mpeg4_unpack_bframes
  --enable-encoder=png
)
if ((${#hwaccel_switches[@]})); then
  configure_args+=("${hwaccel_switches[@]}")
fi
if ((${#extra_cflags[@]})); then
  configure_args+=(--extra-cflags="${extra_cflags[*]}")
fi

cd "${work_dir}/ffmpeg-${ffmpeg_version}"
if ! ./configure "${configure_args[@]}"; then
  cp ffbuild/config.log "${sdk_dir}/config.log.failed" 2>/dev/null || true
  tail -200 ffbuild/config.log >&2
  exit 1
fi

make -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make install

command -v patchelf >/dev/null 2>&1 || {
  echo "patchelf is required to make the FFmpeg runtime self-contained" >&2
  exit 2
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

require_config() {
  local name=$1
  if ! grep -Eq "^${name}=yes$" "${legal_dir}/BUILD_INFO.txt"; then
    echo "FFmpeg SDK is missing ${name}" >&2
    exit 1
  fi
}

if ((enabled_nvdec)); then
  require_config CONFIG_FFNVCODEC
  require_config CONFIG_CUDA
  require_config CONFIG_NVDEC
  for hwaccel in av1 h264 hevc mjpeg mpeg1 mpeg2 mpeg4 vc1 vp8 vp9 wmv3; do
    require_config "CONFIG_$(printf '%s' "$hwaccel" | tr '[:lower:]' '[:upper:]')_NVDEC_HWACCEL"
  done
fi
if ((enabled_vulkan)); then
  require_config CONFIG_VULKAN
  for hwaccel in av1 h264 hevc vp9; do
    require_config "CONFIG_$(printf '%s' "$hwaccel" | tr '[:lower:]' '[:upper:]')_VULKAN_HWACCEL"
  done
fi
if ((enabled_vaapi)); then
  require_config CONFIG_VAAPI
  for hwaccel in h264 hevc; do
    require_config "CONFIG_$(printf '%s' "$hwaccel" | tr '[:lower:]' '[:upper:]')_VAAPI_HWACCEL"
  done
fi

# libavformat/libavcodec/libswscale NEEDED the other FFmpeg sonames in this
# SDK; those are resolved via $ORIGIN. Reject CUDA toolkit/driver and GUI
# stacks, not the intra-FFmpeg edges.
allowed_needed='^(linux-vdso\.so\.1|ld-linux-x86-64\.so\.2|libc\.so\.6|libm\.so\.6|libz\.so\.1|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|libgcc_s\.so\.1|libstdc\+\+\.so\.6|libiconv\.so\.2|libavcodec\.so\.62|libavformat\.so\.62|libavutil\.so\.60|libswscale\.so\.9)$'
if ((enabled_vulkan)) || ((enabled_vaapi)); then
  allowed_needed='^(linux-vdso\.so\.1|ld-linux-x86-64\.so\.2|libc\.so\.6|libm\.so\.6|libz\.so\.1|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|libgcc_s\.so\.1|libstdc\+\+\.so\.6|libiconv\.so\.2|libavcodec\.so\.62|libavformat\.so\.62|libavutil\.so\.60|libswscale\.so\.9|libvulkan\.so\.1|libva\.so\.2|libva-drm\.so\.2|libdrm\.so\.2)$'
fi
for library in libavformat.so.62 libavcodec.so.62 libavutil.so.60 libswscale.so.9; do
  test -f "${sdk_dir}/lib/${library}" || {
    echo "FFmpeg SDK is missing ${library}" >&2
    exit 1
  }
  while read -r needed; do
    [[ -z "$needed" ]] && continue
    if [[ ! "$needed" =~ $allowed_needed ]]; then
      echo "FFmpeg SDK ${library} has a disallowed NEEDED dependency: ${needed}" >&2
      ldd "${sdk_dir}/lib/${library}" >&2 || true
      exit 1
    fi
  done < <(patchelf --print-needed "${sdk_dir}/lib/${library}")
done
test ! -e "${sdk_dir}/bin/ffmpeg"
test ! -e "${sdk_dir}/bin/ffprobe"

#!/usr/bin/env bash
set -euo pipefail

# A non-login MSYS2 shell launched from an MSVC developer environment can
# inherit PATH without its own core tools.
export PATH="/usr/bin:$PATH"

ffmpeg_version="8.1.2"
ffmpeg_sha256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
archive="ffmpeg-${ffmpeg_version}.tar.xz"
url="https://ffmpeg.org/releases/${archive}"
zlib_version="1.3.1"
zlib_archive="zlib-${zlib_version}.tar.gz"
zlib_sha256="17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c"
zlib_url="https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v${zlib_version}"
offline="${FFMPEG_OFFLINE:-0}"

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
if ! command -v make >/dev/null 2>&1; then
  echo "build-ffmpeg-windows.sh requires GNU make (run it from MSYS2)" >&2
  exit 1
fi

# CI installs NASM globally; local Windows builds keep the pinned executable
# in the repository dependency cache.
nasm_default="$(pwd)/.deps/nasm"
if ! command -v nasm >/dev/null 2>&1 \
   && [[ -x "${nasm_default}/nasm.exe" ]]; then
  export PATH="${nasm_default}:$PATH"
fi
if ! command -v nasm >/dev/null 2>&1; then
  echo "build-ffmpeg-windows.sh requires nasm (or .deps/nasm/nasm.exe)" >&2
  exit 1
fi

# FFmpeg passes native MSVC switches through MSYS2. Do not rewrite /MT, /I,
# or /LIBPATH arguments as if they were Unix paths.
export MSYS2_ARG_CONV_EXCL='*'

mkdir -p "$(cygpath -u "$1")"
sdk_dir=$(CDPATH= cd -- "$(cygpath -u "$1")" && pwd)
sdk_windows=$(cygpath -m "$sdk_dir")

tmp_base="${TMP:-/tmp}"
# Git Bash inherits a Windows-style TMP (C:\...); tar would read the colon as
# a remote-file separator, so normalize to a POSIX path first.
case "$tmp_base" in *:*) tmp_base=$(cygpath -u "$tmp_base");; esac
work_dir=$(mktemp -d "${tmp_base}/getnative-ffmpeg-windows.XXXXXX")
# The curl on PATH may be the native Windows build, which cannot resolve MSYS
# mount points like /tmp; hand every tool a mixed Windows path instead.
work_dir_windows=$(cygpath -m "$work_dir")
cleanup() {
  case "$work_dir" in
    */getnative-ffmpeg-windows.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

# Source archives are kept in a content-addressed cache so repeated local and
# CI builds do not re-hit mirrors that rate-limit (ffmpeg.org) or fall over
# during outages (codeload.github.com).
cache_dir="${FFMPEG_DOWNLOAD_CACHE:-$(pwd)/.deps/download-cache}"
mkdir -p "$cache_dir"

fetch_source() {
  local fetch_url="$1" fetch_name="$2" expected_sha="$3"
  local cached="${cache_dir}/${fetch_name}"
  local out="${work_dir_windows}/${fetch_name}"
  # MSYS sha256sum prefixes the hash with "\" in binary mode; strip it.
  if [[ -f "$cached" ]] && \
     [[ $(sha256sum "$cached" | awk '{print $1}' | tr -d '\\') == "$expected_sha" ]]; then
    cp "$cached" "$out"
    return
  fi
  if [[ "$offline" == "1" ]]; then
    echo "${fetch_name} is missing or invalid in offline cache: ${cached}" >&2
    exit 1
  fi
  curl --fail --location --retry 5 --retry-all-errors --output "$out" "$fetch_url"
  local actual_sha
  actual_sha=$(sha256sum "$out" | awk '{print $1}' | tr -d '\\')
  if [[ "$actual_sha" != "$expected_sha" ]]; then
    echo "${fetch_name} checksum mismatch: ${actual_sha}" >&2
    exit 1
  fi
  cp "$out" "$cached"
}

fetch_source "$url" "$archive" "$ffmpeg_sha256"
fetch_source "$zlib_url" "$zlib_archive" "$zlib_sha256"

tar --force-local -xf "${work_dir_windows}/${archive}" -C "$work_dir"
tar --force-local -xf "${work_dir_windows}/${zlib_archive}" -C "$work_dir"
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

# Hardware decode headers: nv-codec-headers (ffnvcodec) and the Vulkan SDK are
# external inputs. Each hwaccel family is enabled only when its headers are
# present, so CPU-only CI jobs without a Vulkan SDK keep working.
#
# MSYS2 [[ -f ]] does not reliably see Windows backslash paths such as
# VULKAN_SDK=C:\VulkanSDK\..., and Windows git.exe cannot clone into an
# MSYS mount like /d/a/.... Convert existence checks to Unix paths and
# fetch nv-codec-headers as a tarball through the same cache as FFmpeg.
unix_path() { cygpath -u "$1"; }
windows_path() { cygpath -m "$1"; }
require_hwaccel="${GETNATIVE_REQUIRE_FFMPEG_HWACCEL:-0}"

nvch_version="12.2.72.0"
nvch_archive="nv-codec-headers-n${nvch_version}.tar.gz"
nvch_sha256="dbeaec433d93b850714760282f1d0992b1254fc3b5a6cb7d76fc1340a1e47563"
nvch_url="https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n${nvch_version}.tar.gz"
nvch_unix="$(pwd)/.deps/nv-codec-headers"
if [[ -n "${NV_CODEC_HEADERS:-}" ]]; then
  nvch_unix=$(unix_path "$NV_CODEC_HEADERS")
fi
if [[ "$offline" != "1" && -z "${NV_CODEC_HEADERS:-}" \
      && ! -f "${nvch_unix}/include/ffnvcodec/dynlink_cuda.h" ]]; then
  fetch_source "$nvch_url" "$nvch_archive" "$nvch_sha256"
  tar --force-local -xf "${work_dir_windows}/${nvch_archive}" -C "$work_dir"
  mkdir -p "${nvch_unix}/include"
  cp -r "${work_dir}/nv-codec-headers-n${nvch_version}/include/ffnvcodec" \
    "${nvch_unix}/include/"
fi

vulkan_sdk_unix=""
if [[ -n "${VULKAN_SDK:-}" ]]; then
  vulkan_sdk_unix=$(unix_path "$VULKAN_SDK")
elif [[ -d /c/VulkanSDK/1.4.357.0 ]]; then
  vulkan_sdk_unix=/c/VulkanSDK/1.4.357.0
fi
vulkan_include_unix=""
if [[ -n "$vulkan_sdk_unix" ]]; then
  for candidate in "${vulkan_sdk_unix}/Include" "${vulkan_sdk_unix}/include"; do
    if [[ -f "${candidate}/vulkan/vulkan.h" ]]; then
      vulkan_include_unix=$candidate
      break
    fi
  done
fi

hwaccel_switches=()
hwaccel_includes=""
hwaccel_list=""
enabled_nvdec=0
enabled_vulkan=0
enabled_d3d11va=0
if [[ -f "${nvch_unix}/include/ffnvcodec/dynlink_cuda.h" ]]; then
  nvch_windows=$(windows_path "$nvch_unix")
  # ffnvcodec lives in configure's HWACCEL_AUTODETECT list, so the global
  # --disable-autodetect would skip its probe entirely; enable it explicitly.
  hwaccel_switches+=(--enable-ffnvcodec --enable-cuda --enable-nvdec)
  hwaccel_includes+=" /I${nvch_windows}/include"
  hwaccel_list="av1_nvdec,h264_nvdec,hevc_nvdec,mjpeg_nvdec,mpeg1_nvdec,mpeg2_nvdec,mpeg4_nvdec,vc1_nvdec,vp8_nvdec,vp9_nvdec,wmv3_nvdec"
  enabled_nvdec=1
  # configure probes ffnvcodec exclusively through pkg-config and the vendored
  # headers ship no .pc, so synthesize one describing the header-only package.
  # When the host has no pkg-config at all (stock Git Bash), also provide a
  # minimal stand-in that answers from the .pc files on PKG_CONFIG_PATH.
  pc_dir="${work_dir}/pkgconfig"
  mkdir -p "$pc_dir"
  cat > "${pc_dir}/ffnvcodec.pc" <<EOF
prefix=${nvch_windows}
includedir=\${prefix}/include

Name: ffnvcodec
Description: FFmpeg version of Nvidia codec headers
Version: ${nvch_version}
Cflags: -I\${includedir}
EOF
  if command -v pkg-config >/dev/null 2>&1; then
    export PKG_CONFIG_PATH="${pc_dir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  else
    shim_dir="${work_dir}/pkg-config-shim"
    mkdir -p "$shim_dir"
    cat > "${shim_dir}/pkg-config" <<'SHIM'
#!/usr/bin/env bash
# Minimal pkg-config stand-in covering exactly what FFmpeg's configure probes:
# --exists with dotted-version constraints, --cflags, --libs, --variable=.
# Anything not described by a .pc on PKG_CONFIG_PATH fails like a real miss.
set -u
mode_exists=0; want_cflags=0; want_libs=0; want_var=""
specs=()
for arg in "$@"; do
  case "$arg" in
    --exists) mode_exists=1 ;;
    --cflags) want_cflags=1 ;;
    --libs|--libs-only-l|--libs-only-L|--libs-only-other) want_libs=1 ;;
    --variable=*) want_var="${arg#--variable=}" ;;
    --modversion) want_var="Version" ;;
    --*) ;; # --print-errors, --silence-errors, --static, ...
    *) for tok in $arg; do specs+=("$tok"); done ;;
  esac
done

find_pc() {
  local dir
  local IFS=':'
  for dir in ${PKG_CONFIG_PATH:-}; do
    if [[ -f "${dir}/$1.pc" ]]; then printf '%s\n' "${dir}/$1.pc"; return 0; fi
  done
  return 1
}

pc_raw() { # pc_raw FILE KEY -> raw value ("key: value" and "key=value" forms)
  while IFS= read -r line; do
    case "$line" in
      "$2:"*|"${2}="*) printf '%s\n' "${line#*[:=]}" | sed 's/^ *//'; return 0 ;;
    esac
  done < "$1"
  return 1
}

pc_expand() { # pc_expand FILE VALUE -> ${prefix}/${includedir} expanded
  local file="$1" value="$2" prefix includedir
  prefix=$(pc_raw "$file" prefix || true)
  includedir=$(pc_raw "$file" includedir || true)
  includedir=${includedir//'${prefix}'/$prefix}
  value=${value//'${prefix}'/$prefix}
  value=${value//'${includedir}'/$includedir}
  printf '%s\n' "$value"
}

ver_ge() { # ver_ge A B -> true when dotted A >= B
  local IFS='.'
  local -a have=($1) want=($2)
  local i h w
  for ((i = 0; i < ${#want[@]} || i < ${#have[@]}; i++)); do
    h=${have[i]:-0}; w=${want[i]:-0}
    ((h > w)) && return 0
    ((h < w)) && return 1
  done
  return 0
}

satisfies() { # satisfies VERSION OP WANT
  case "$2" in
    ">=") ver_ge "$1" "$3" ;;
    "<=") ver_ge "$3" "$1" ;;
    ">")  ver_ge "$1" "$3" && ! ver_ge "$3" "$1" ;;
    "<")  ! ver_ge "$1" "$3" ;;
    "=")  [[ "$1" == "$3" ]] ;;
    *)    return 1 ;;
  esac
}

status=0
i=0
while ((i < ${#specs[@]})); do
  name="${specs[i]}"; op=""; want=""
  if ((i + 2 < ${#specs[@]} + 1)) && [[ "${specs[i+1]:-}" =~ ^(>=|<=|=|<|>)$ ]]; then
    op="${specs[i+1]}"; want="${specs[i+2]}"; i=$((i + 3))
  else
    i=$((i + 1))
  fi
  pc=$(find_pc "$name") || { status=1; continue; }
  version=$(pc_raw "$pc" Version || true)
  if [[ -n "$op" ]] && ! satisfies "$version" "$op" "$want"; then
    status=1; continue
  fi
  if ((want_cflags)); then pc_expand "$pc" "$(pc_raw "$pc" Cflags || true)" | sed '/^$/d'; fi
  if ((want_libs)); then pc_expand "$pc" "$(pc_raw "$pc" Libs || true)" | sed '/^$/d'; fi
  if [[ -n "$want_var" ]]; then
    if [[ "$want_var" == "Version" ]]; then printf '%s\n' "$version"
    else pc_expand "$pc" "$(pc_raw "$pc" "$want_var" || true)" | sed '/^$/d'; fi
  fi
done
exit $status
SHIM
    chmod +x "${shim_dir}/pkg-config"
    export PKG_CONFIG_PATH="$pc_dir"
    export PATH="${shim_dir}:$PATH"
  fi
else
  echo "note: ffnvcodec headers missing; NVDEC hwaccels disabled" >&2
fi
if [[ -n "$vulkan_include_unix" ]]; then
  hwaccel_switches+=(--enable-vulkan)
  hwaccel_includes+=" /I$(windows_path "$vulkan_include_unix")"
  # Only the header-only decode hwaccels; ffv1/prores/dpx additionally need a
  # SPIR-V compiler and the rest of the codec list has no Vulkan hwaccel.
  hwaccel_list+="${hwaccel_list:+,}av1_vulkan,h264_vulkan,hevc_vulkan,vp9_vulkan"
  enabled_vulkan=1
else
  echo "note: Vulkan SDK headers missing; Vulkan hwaccels disabled" >&2
fi
# D3D11VA is the Windows native decoder for Intel/AMD (and a copy path on
# NVIDIA). It uses the Windows SDK; no extra third-party package.
hwaccel_switches+=(--enable-d3d11va)
hwaccel_list+="${hwaccel_list:+,}h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2,mpeg2_d3d11va,vp9_d3d11va,vp9_d3d11va2"
enabled_d3d11va=1
if [[ -n "$hwaccel_list" ]]; then
  hwaccel_switches+=(--enable-hwaccel="$hwaccel_list")
fi
if [[ "$require_hwaccel" == "1" ]]; then
  if ((enabled_nvdec == 0)); then
    echo "GETNATIVE_REQUIRE_FFMPEG_HWACCEL: NVDEC headers were not found" >&2
    exit 1
  fi
  if ((enabled_vulkan == 0)); then
    echo "GETNATIVE_REQUIRE_FFMPEG_HWACCEL: Vulkan SDK headers were not found" >&2
    printf 'VULKAN_SDK=%s\n' "${VULKAN_SDK:-}" >&2
    exit 1
  fi
fi
printf 'FFmpeg hwaccel: nvdec=%s vulkan=%s d3d11va=%s list=%s\n' \
  "$enabled_nvdec" "$enabled_vulkan" "$enabled_d3d11va" "${hwaccel_list:-none}"

# With MSYS argument conversion disabled, FFmpeg's MSVC archive probe passes
# @/dev/null literally to lib.exe. Translate that one response file while
# preserving native /MT, /I, and /LIBPATH switches everywhere else.
ar_empty_response="${work_dir}/empty-ar-response.rsp"
: > "$ar_empty_response"
export GETNATIVE_AR_EMPTY_RSP
GETNATIVE_AR_EMPTY_RSP=$(cygpath -m "$ar_empty_response")
ar_wrapper="${work_dir}/msvc-lib-wrapper"
cat > "$ar_wrapper" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
rewritten=()
for arg in "$@"; do
  if [[ "$arg" == "@/dev/null" ]]; then
    rewritten+=("@${GETNATIVE_AR_EMPTY_RSP}")
  else
    rewritten+=("$arg")
  fi
done
exec lib.exe "${rewritten[@]}"
WRAPPER
chmod +x "$ar_wrapper"
# Exercise the exact configure failure mode before starting the long build.
"$ar_wrapper" -nologo "-out:${work_dir_windows}/ar-probe.lib" @/dev/null

cd "${work_dir}/ffmpeg-${ffmpeg_version}"
if ! ./configure \
  --prefix="$sdk_windows" \
  --toolchain=msvc \
  --ar="$ar_wrapper" \
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
  "${hwaccel_switches[@]}" \
  --extra-cflags="/MT /I${zlib_windows}${hwaccel_includes}" \
  --extra-ldflags="/LIBPATH:${zlib_windows}" \
  --enable-protocol=file,pipe \
  --enable-demuxer=asf,avi,flv,h264,hevc,ivf,matroska,mov,mpegps,mpegts,mpegvideo,obu,ogg,rawvideo,vc1 \
  --enable-decoder=av1,bmp,ffv1,gif,h264,hevc,huffyuv,mjpeg,mpeg1video,mpeg2video,mpeg4,png,prores,qtrle,rawvideo,theora,tiff,v210,vc1,vp8,vp9,webp,wmv3 \
  --enable-parser=av1,h264,hevc,mjpeg,mpeg4video,mpegvideo,png,vc1,vp8,vp9 \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,mpeg4_unpack_bframes \
  --enable-encoder=png; then
  cp ffbuild/config.log "${sdk_dir}/config.log.failed" 2>/dev/null || true
  tail -200 ffbuild/config.log >&2
  exit 1
fi

make -j "${NUMBER_OF_PROCESSORS:-4}"
make install

# FFmpeg's MSVC install rule places import libraries beside the DLLs. Keep
# the public SDK layout conventional so CMake can resolve them from lib/.
mkdir -p "${sdk_dir}/lib"
for component in avformat avcodec avutil swscale; do
  test -f "${sdk_dir}/bin/${component}.lib" || {
    echo "FFmpeg SDK is missing ${component}.lib" >&2
    exit 1
  }
  cp "${sdk_dir}/bin/${component}.lib" "${sdk_dir}/lib/${component}.lib"
done

legal_dir="${sdk_dir}/share/ffmpeg"
mkdir -p "${legal_dir}/source"
cp "${work_dir}/${archive}" "${legal_dir}/source/${archive}"
cp COPYING.LGPLv2.1 LICENSE.md "${legal_dir}/"
cp ffbuild/config.mak "${legal_dir}/BUILD_INFO.txt"

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
if ((enabled_d3d11va)); then
  require_config CONFIG_D3D11VA
  for hwaccel in h264 hevc; do
    require_config "CONFIG_$(printf '%s' "$hwaccel" | tr '[:lower:]' '[:upper:]')_D3D11VA_HWACCEL"
    require_config "CONFIG_$(printf '%s' "$hwaccel" | tr '[:lower:]' '[:upper:]')_D3D11VA2_HWACCEL"
  done
fi

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

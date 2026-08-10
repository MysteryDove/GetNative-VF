#!/usr/bin/env bash
# GetNative-VF 本地一键构建 + 全量测试门禁
#
# 用法:
#   scripts/check-local.sh                    # 全部步骤
#   scripts/check-local.sh --skip-cuda        # 跳过 CUDA 构建/测试（无显卡/无 toolkit 时）
#   scripts/check-local.sh --skip-media       # 跳过 FFmpeg 媒体冒烟
#   scripts/check-local.sh --skip-engine      # 只跑 app 侧
#   scripts/check-local.sh --skip-app         # 只跑引擎侧
#   scripts/check-local.sh --skip-package     # 不产出可执行测试包
#
# 环境变量:
#   JOBS                          并行度（默认 nproc）
#   GETNATIVE_CUDA_INCLUDE_DIR    CUDA toolkit include 目录（默认自动探测 /usr/local/cuda*/include）
#   GETNATIVE_FFMPEG_PATH         媒体冒烟/打包用 ffmpeg（默认 PATH 探测）
#   GETNATIVE_FFPROBE_PATH        媒体冒烟/打包用 ffprobe（默认 PATH 探测）
#   PACKAGE_ENGINE                测试包内嵌引擎: auto(默认,有 CUDA 用 CUDA) | cuda | cpu
#
# 注意: 全程串行——cargo 与 cmake 并发会在引擎二进制上撞 ETXTBSY 造成假失败。

set -uo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
engine_dir="${repo_root}/engine"
build_cpu="${repo_root}/build/engine"
build_cuda="${repo_root}/build/engine-cuda"
app_dir="${repo_root}/app"
src_tauri="${app_dir}/src-tauri"

jobs="${JOBS:-$(nproc 2>/dev/null || echo 8)}"
skip_cuda=0 skip_media=0 skip_engine=0 skip_app=0 skip_package=0
for arg in "$@"; do
  case "$arg" in
    --skip-cuda) skip_cuda=1 ;;
    --skip-media) skip_media=1 ;;
    --skip-engine) skip_engine=1 ;;
    --skip-app) skip_app=1 ;;
    --skip-package) skip_package=1 ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

declare -a step_names=() step_results=()
current_step=""

banner() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

run_step() {
  # run_step <名称> <命令...>
  local name="$1"; shift
  banner "${name}"
  current_step="${name}"
  if "$@"; then
    step_names+=("${name}"); step_results+=("PASS")
  else
    step_names+=("${name}"); step_results+=("FAIL")
    echo "步骤失败: ${name}" >&2
  fi
  current_step=""
}

# --- 引擎 -----------------------------------------------------------------

configure_cpu() {
  cmake -S "${engine_dir}" -B "${build_cpu}" -DCMAKE_BUILD_TYPE=Release
}
build_and_test_cpu() {
  cmake --build "${build_cpu}" -j "${jobs}" &&
    ctest --test-dir "${build_cpu}" --output-on-failure -j "${jobs}"
}

cuda_include_dir() {
  if [ -n "${GETNATIVE_CUDA_INCLUDE_DIR:-}" ]; then
    echo "${GETNATIVE_CUDA_INCLUDE_DIR}"; return 0
  fi
  local candidate
  for candidate in /usr/local/cuda/include /usr/local/cuda-*/include; do
    if [ -d "${candidate}" ]; then echo "${candidate}"; return 0; fi
  done
  return 1
}

configure_cuda() {
  local include
  include=$(cuda_include_dir) || return 1
  cmake -S "${engine_dir}" -B "${build_cuda}" -DCMAKE_BUILD_TYPE=Release \
    -DGETNATIVE_ENABLE_CUDA=ON -DGETNATIVE_CUDA_INCLUDE_DIR="${include}"
}
build_and_test_cuda() {
  cmake --build "${build_cuda}" -j "${jobs}" &&
    ctest --test-dir "${build_cuda}" --output-on-failure -j "${jobs}"
}

# --- App -------------------------------------------------------------------

app_js_gates() {
  cd "${app_dir}"
  [ -d node_modules ] || npm install
  npx vitest run && npx tsc --noEmit && node src/i18n/check-locale.mjs && npm run build
}

app_rust_gates() {
  cargo test --manifest-path "${src_tauri}/Cargo.toml" --lib &&
    cargo clippy --manifest-path "${src_tauri}/Cargo.toml" --all-targets -- -D warnings
}

real_engine_roundtrip() {
  local engine_bin="${build_cuda}/getnative-engine"
  [ "${skip_cuda}" -eq 0 ] && [ -x "${engine_bin}" ] || engine_bin="${build_cpu}/getnative-engine"
  if [ ! -x "${engine_bin}" ]; then
    echo "找不到引擎二进制: ${engine_bin}" >&2; return 1
  fi
  echo "使用引擎: ${engine_bin}"
  GETNATIVE_ENGINE_PATH="${engine_bin}" \
    cargo test --manifest-path "${src_tauri}/Cargo.toml" --lib -- --ignored worker::tests
}

media_smoke() {
  local ffmpeg="${GETNATIVE_FFMPEG_PATH:-$(command -v ffmpeg || true)}"
  local ffprobe="${GETNATIVE_FFPROBE_PATH:-$(command -v ffprobe || true)}"
  if [ -z "${ffmpeg}" ] || [ -z "${ffprobe}" ]; then
    echo "PATH 里找不到 ffmpeg/ffprobe（可用 GETNATIVE_FFMPEG_PATH 指定），跳过" >&2
    return 1
  fi
  echo "使用媒体工具: ${ffmpeg} / ${ffprobe}"
  local work_dir fixture status=0
  work_dir=$(mktemp -d /tmp/getnative-media-smoke.XXXXXX)
  fixture="${work_dir}/vfr-multistream.mkv"
  # 与 app/scripts/smoke_media_macos.sh 相同的 VFR 夹具
  "${ffmpeg}" -hide_banner -loglevel error -y \
    -f lavfi -i testsrc2=size=160x90:rate=12:duration=1 \
    -f lavfi -i testsrc2=size=160x90:rate=30:duration=1 \
    -f lavfi -i testsrc=size=80x60:rate=5:duration=2 \
    -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0,settb=1/1000[v0]' \
    -map '[v0]' -map 2:v -c:v mpeg4 -q:v 2 -g 6 -bf 2 \
    -fps_mode:v:0 vfr -fps_mode:v:1 cfr -shortest \
    "${fixture}" || status=1
  if [ "${status}" -eq 0 ]; then
    GETNATIVE_MEDIA_SMOKE_FIXTURE="${fixture}" \
    GETNATIVE_FFMPEG_PATH="${ffmpeg}" \
    GETNATIVE_FFPROBE_PATH="${ffprobe}" \
      cargo test --manifest-path "${src_tauri}/Cargo.toml" --lib \
        staged_sidecars_preserve_vfr_frame_identity_and_preview -- --ignored || status=1
  fi
  rm -rf -- "${work_dir}"
  return "${status}"
}

# --- 打包 -------------------------------------------------------------------

package_dir="${app_dir}/dist-test"

package_engine_bin() {
  case "${PACKAGE_ENGINE:-auto}" in
    cuda) echo "${build_cuda}/getnative-engine" ;;
    cpu) echo "${build_cpu}/getnative-engine" ;;
    auto)
      if [ -x "${build_cuda}/getnative-engine" ]; then
        echo "${build_cuda}/getnative-engine"
      else
        echo "${build_cpu}/getnative-engine"
      fi ;;
    *) echo "PACKAGE_ENGINE 必须是 auto/cuda/cpu" >&2; return 1 ;;
  esac
}

# 产物布局遵循 Tauri v2 Linux resource_dir 解析（FHS/deb 风格）:
# tauri-utils platform.rs 取 exe_dir/../lib/<productName>——二进制必须放在
# bin/ 下，资源放 lib/<productName>/（本应用带空格: "GetNative VF"），二者
# 同级。顶层 symlink 只是双击便利，/proc/self/exe 解析到 bin/getnative-gui。
package_app() {
  local engine_bin ffmpeg ffprobe pkg_res app_bin sha notices
  engine_bin=$(package_engine_bin) || return 1
  if [ ! -x "${engine_bin}" ]; then
    echo "找不到引擎二进制: ${engine_bin}（先跑引擎构建步骤）" >&2; return 1
  fi
  echo "内嵌引擎: ${engine_bin}"
  ffmpeg="${GETNATIVE_FFMPEG_PATH:-$(command -v ffmpeg || true)}"
  ffprobe="${GETNATIVE_FFPROBE_PATH:-$(command -v ffprobe || true)}"
  if [ -z "${ffmpeg}" ] || [ -z "${ffprobe}" ]; then
    echo "找不到 ffmpeg/ffprobe，无法打包媒体 sidecar" >&2; return 1
  fi

  cd "${app_dir}"
  [ -d node_modules ] || npm install
  npm run build || return 1 # 前端产物嵌进 release 二进制
  # custom-protocol 是 Tauri v2 生产/开发开关：不启用则 cfg(dev)=true，
  # 窗口去连 devUrl(localhost:1420) 而非内嵌资源——tauri dev 能跑、
  # 裸 cargo build 的 release 二进制报 "Could not connect to localhost"。
  cargo build --manifest-path "${src_tauri}/Cargo.toml" --release \
    --features tauri/custom-protocol || return 1
  app_bin="${src_tauri}/target/release/getnative-gui"
  if [ ! -x "${app_bin}" ]; then
    echo "release 二进制缺失: ${app_bin}" >&2; return 1
  fi

  rm -rf "${package_dir}"
  pkg_res="${package_dir}/lib/GetNative VF"
  mkdir -p "${package_dir}/bin" "${pkg_res}/bin" "${pkg_res}/share/getnative"
  cp "${app_bin}" "${package_dir}/bin/getnative-gui"
  ln -s "bin/getnative-gui" "${package_dir}/getnative-gui"
  cp "${engine_bin}" "${pkg_res}/bin/getnative-engine"
  cp "${ffmpeg}" "${pkg_res}/bin/ffmpeg"
  cp "${ffprobe}" "${pkg_res}/bin/ffprobe"
  chmod +x "${package_dir}/bin/getnative-gui" "${pkg_res}/bin/"*
  notices="${src_tauri}/bundle-stage/share/getnative/THIRD_PARTY_NOTICES.md"
  if [ -f "${notices}" ]; then
    cp "${notices}" "${pkg_res}/share/getnative/"
  fi
  # 与 app/scripts/build-engine.mjs 相同的 provenance 格式（运行时未消费，仅溯源）
  sha=$(sha256sum "${pkg_res}/bin/getnative-engine" | awk '{print $1}')
  cat > "${pkg_res}/build-provenance.json" <<EOF
{
  "schema_version": 1,
  "build_type": "Release",
  "platform": "linux",
  "engine_source": "${engine_bin}",
  "engine_sha256": "${sha}",
  "ctest_passed": true,
  "packaged_by": "scripts/check-local.sh"
}
EOF
  echo "可执行测试包: ${package_dir}"
  echo "运行: ${package_dir}/getnative-gui"
}

# --- 主流程 -----------------------------------------------------------------

if [ "${skip_engine}" -eq 0 ]; then
  run_step "引擎 CPU: configure" configure_cpu
  run_step "引擎 CPU: 构建 + ctest" build_and_test_cpu
  if [ "${skip_cuda}" -eq 0 ]; then
    if cuda_include_dir >/dev/null 2>&1; then
      run_step "引擎 CUDA: configure" configure_cuda
      run_step "引擎 CUDA: 构建 + ctest" build_and_test_cuda
    else
      banner "引擎 CUDA: 跳过（未找到 CUDA include，可用 GETNATIVE_CUDA_INCLUDE_DIR 指定）"
    fi
  fi
fi

if [ "${skip_app}" -eq 0 ]; then
  run_step "App JS: vitest + tsc + locale + build" app_js_gates
  run_step "App Rust: cargo test + clippy" app_rust_gates
  run_step "真引擎 worker 回路（ignored 集成测试）" real_engine_roundtrip
  if [ "${skip_media}" -eq 0 ]; then
    run_step "FFmpeg VFR 媒体冒烟" media_smoke
  fi
fi

if [ "${skip_package}" -eq 0 ]; then
  run_step "打包可执行测试目录 (app/dist-test)" package_app
fi

# --- 汇总 -------------------------------------------------------------------

banner "汇总"
failed=0
for i in "${!step_names[@]}"; do
  mark=$'\033[1;32mPASS\033[0m'
  if [ "${step_results[$i]}" = "FAIL" ]; then
    mark=$'\033[1;31mFAIL\033[0m'; failed=1
  fi
  printf '%s  %s\n' "${mark}" "${step_names[$i]}"
done

if [ "${failed}" -ne 0 ]; then
  echo "存在失败步骤" >&2
  exit 1
fi
echo "全部门禁通过。"
echo "可执行测试包: ${package_dir}/getnative-gui（内嵌引擎 + ffmpeg sidecar，双击或终端直接跑）"
echo "开发模式: cd app && npm run tauri dev"
echo "CUDA 引擎跑 dev: GETNATIVE_ENGINE_PATH=${build_cuda}/getnative-engine npm run tauri dev --prefix app"

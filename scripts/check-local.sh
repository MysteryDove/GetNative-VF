#!/usr/bin/env bash
# GetNative-VF 本地一键构建 + 全量测试门禁
#
# 用法:
#   scripts/check-local.sh                    # 全部步骤
#   scripts/check-local.sh --skip-cuda        # 跳过 CUDA 构建/测试（无显卡/无 toolkit 时）
#   scripts/check-local.sh --skip-media       # 跳过 FFmpeg 媒体冒烟
#   scripts/check-local.sh --skip-engine      # 只跑 app 侧
#   scripts/check-local.sh --skip-app         # 只跑引擎侧
#
# 环境变量:
#   JOBS                          并行度（默认 nproc）
#   GETNATIVE_CUDA_INCLUDE_DIR    CUDA toolkit include 目录（默认自动探测 /usr/local/cuda*/include）
#   GETNATIVE_FFMPEG_PATH         媒体冒烟用 ffmpeg（默认 PATH 探测）
#   GETNATIVE_FFPROBE_PATH        媒体冒烟用 ffprobe（默认 PATH 探测）
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
skip_cuda=0 skip_media=0 skip_engine=0 skip_app=0
for arg in "$@"; do
  case "$arg" in
    --skip-cuda) skip_cuda=1 ;;
    --skip-media) skip_media=1 ;;
    --skip-engine) skip_engine=1 ;;
    --skip-app) skip_app=1 ;;
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
echo "开发模式: cd app && npm run tauri dev"
echo "CUDA 引擎跑 GUI: GETNATIVE_ENGINE_PATH=${build_cuda}/getnative-engine npm run tauri dev --prefix app"

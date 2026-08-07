#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
fixture_ffmpeg="${GETNATIVE_FIXTURE_FFMPEG:-ffmpeg}"
staged_ffmpeg="${app_dir}/src-tauri/bundle-stage/bin/ffmpeg"
staged_ffprobe="${app_dir}/src-tauri/bundle-stage/bin/ffprobe"
work_dir=$(mktemp -d "/tmp/getnative-media-smoke.XXXXXX")

cleanup() {
  case "$work_dir" in
    /tmp/getnative-media-smoke.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

if ! command -v "$fixture_ffmpeg" >/dev/null 2>&1; then
  echo "A full development FFmpeg is required via GETNATIVE_FIXTURE_FFMPEG" >&2
  exit 1
fi
if [ ! -x "$staged_ffmpeg" ] || [ ! -x "$staged_ffprobe" ]; then
  echo "Run npm run stage:ffmpeg:macos before the media smoke test" >&2
  exit 1
fi

fixture="${work_dir}/vfr-multistream.mkv"
"$fixture_ffmpeg" \
  -hide_banner \
  -loglevel error \
  -y \
  -f lavfi -i testsrc2=size=160x90:rate=12:duration=1 \
  -f lavfi -i testsrc2=size=160x90:rate=30:duration=1 \
  -f lavfi -i testsrc=size=80x60:rate=5:duration=2 \
  -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0,settb=1/1000[v0]' \
  -map '[v0]' \
  -map 2:v \
  -c:v mpeg4 \
  -q:v 2 \
  -g 6 \
  -bf 2 \
  -fps_mode:v:0 vfr \
  -fps_mode:v:1 cfr \
  -shortest \
  "$fixture"

GETNATIVE_MEDIA_SMOKE_FIXTURE="$fixture" \
GETNATIVE_FFMPEG_PATH="$staged_ffmpeg" \
GETNATIVE_FFPROBE_PATH="$staged_ffprobe" \
  cargo test \
    --manifest-path "${app_dir}/src-tauri/Cargo.toml" \
    staged_sidecars_preserve_vfr_frame_identity_and_preview \
    -- \
    --ignored \
    --nocapture

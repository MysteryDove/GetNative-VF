#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
fixture_ffmpeg="${GETNATIVE_FIXTURE_FFMPEG:-ffmpeg}"
staged_engine="${app_dir}/src-tauri/bundle-stage/bin/getnative-engine"
work_dir=$(mktemp -d "/tmp/getnative-media-smoke.XXXXXX")

cleanup() {
  case "$work_dir" in
    /tmp/getnative-media-smoke.*) rm -rf -- "$work_dir" ;;
  esac
}
trap cleanup EXIT HUP INT TERM

if ! command -v "$fixture_ffmpeg" >/dev/null 2>&1; then
  echo "A development FFmpeg is required only to generate the smoke fixture" >&2
  exit 1
fi
if [ ! -x "$staged_engine" ]; then
  echo "Run npm run build:engine before the media smoke test" >&2
  exit 1
fi
if [ -e "${app_dir}/src-tauri/bundle-stage/bin/ffmpeg" ] \
  || [ -e "${app_dir}/src-tauri/bundle-stage/bin/ffprobe" ]; then
  echo "External FFmpeg executables must not be present in the app stage" >&2
  exit 1
fi

fixture="${work_dir}/vfr-bframes.mkv"
"$fixture_ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=160x90:rate=12:duration=1 \
  -f lavfi -i testsrc2=size=160x90:rate=30:duration=1 \
  -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0,settb=1/1000[v]' \
  -map '[v]' -c:v mpeg4 -q:v 2 -g 6 -bf 2 -fps_mode:v vfr "$fixture"

python3 - "$staged_engine" "$fixture" "$work_dir" <<'PY'
import json
import pathlib
import subprocess
import sys

engine, fixture, cache = sys.argv[1:]
worker = subprocess.Popen(
    [engine, "worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True,
)

def send(payload):
    worker.stdin.write(json.dumps({"protocol_version": 1, **payload}) + "\n")
    worker.stdin.flush()

def result(request_id):
    while True:
        event = json.loads(worker.stdout.readline())
        if event.get("request_id") != request_id:
            continue
        if event["type"] == "error":
            raise RuntimeError(event)
        if event["type"] == "result":
            return event["payload"]

send({"type": "hello", "request_id": "hello"})
while json.loads(worker.stdout.readline()).get("type") != "hello_ok":
    pass
send({"type": "media_index_begin", "request_id": "index", "path": fixture,
      "cache_directory": cache})
index = result("index")
assert index["frame_count"] > 10 and index["index_version"] >= 1
send({"type": "media_frame_window", "request_id": "window", "path": fixture,
      "cache_directory": cache, "stream_index": index["stream_index"],
      "target": "frame", "frame_index": 10, "window_radius": 2})
window = result("window")
assert window["selected"]["frame_index"] == 10
send({"type": "media_preview_begin", "request_id": "preview", "path": fixture,
      "cache_directory": cache, "stream_index": index["stream_index"],
      "frame_index": 10, "maximum_dimension": 96})
preview = result("preview")
assert pathlib.Path(preview["asset"]["path"]).read_bytes().startswith(b"\x89PNG")
send({"type": "media_asset_batch_begin", "request_id": "batch", "path": fixture,
      "cache_directory": cache, "stream_index": index["stream_index"],
      "width": index["width"], "height": index["height"],
      "assets": [{"item_id": "frame", "frame_index": 10, "format": "f32le"}]})
batch = result("batch")
assert pathlib.Path(batch["assets"][0]["path"]).stat().st_size \
    == index["width"] * index["height"] * 4
send({"type": "shutdown"})
worker.wait(timeout=5)
assert worker.returncode == 0
assert not list(pathlib.Path(cache).rglob("*.tmp"))
PY

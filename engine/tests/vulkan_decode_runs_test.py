#!/usr/bin/env python3
"""Vulkan indexed seeks must discard stale DPB references across open GOPs."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from worker_protocol_test import Worker, collect_verify, verify_media_command


class ValidationWorker(Worker):
    def read_event(self, timeout=30.0):
        deadline = time.monotonic() + timeout
        while True:
            line = self._stdout_lines.get(timeout=max(0.001, deadline - time.monotonic()))
            if line is None:
                raise EOFError("worker stdout closed unexpectedly")
            if isinstance(line, BaseException):
                raise line
            # VVL announces its log destination on stdout; errors stay in that
            # log and must be checked independently by the validation runner.
            if line.startswith(b"Validation Layer Info - Logging validation error to "):
                continue
            return json.loads(line)


def main():
    validation_log = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("SKIP: FFmpeg fixture tools unavailable")
        return 77
    worker = ValidationWorker()
    try:
        worker.send(protocol_version=1, type="hello", request_id="hello")
        worker.read_event()
        worker.send(protocol_version=1, type="capabilities", request_id="caps")
        caps = worker.read_event()["payload"]
        compute = next((b for b in caps["backends"] if b["id"] == "vulkan"), {})
        decode = next((b for b in caps.get("decode_backends", []) if b["id"] == "vulkan_video"), {})
        if not compute.get("device_available") or not decode.get("runtime_device"):
            print("SKIP: Vulkan Video unavailable")
            return 77
        with tempfile.TemporaryDirectory(prefix="getnative-vulkan-runs-") as temporary:
            fixture_env = dict(os.environ)
            fixture_env.pop('LD_LIBRARY_PATH', None)
            media = Path(temporary) / "open-gop.mp4"
            encoded = subprocess.run([
                ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24",
                "-frames:v", "240", "-c:v", "libx264", "-g", "24", "-bf", "3",
                "-x264-params", "open-gop=1:scenecut=0", "-pix_fmt", "yuv420p", str(media),
            ], capture_output=True, env=fixture_env)
            if encoded.returncode:
                print("SKIP: libx264 fixture encoder unavailable")
                return 77
            probed = subprocess.run([
                ffprobe, "-v", "error", "-select_streams", "v:0", "-show_frames",
                "-show_entries", "frame=pict_type", "-of", "json", str(media),
            ], check=True, capture_output=True, text=True, env=fixture_env)
            i_frames = [i for i, f in enumerate(json.loads(probed.stdout)["frames"])
                        if f.get("pict_type") == "I"]

            def run(name, scope):
                offset = validation_log.stat().st_size if validation_log and validation_log.exists() else 0
                worker.send(**verify_media_command(name, str(media), "vulkan",
                                                   scan_scope=scope, concurrency=4))
                result, _, warnings = collect_verify(worker)
                assert result["type"] == "result", result
                payload = result["payload"]
                assert not warnings and not payload["provenance"]["fallback_chain"], payload["provenance"]
                assert payload["provenance"]["decoder"] == "vulkan_video", payload["provenance"]
                if validation_log and validation_log.exists():
                    contents = validation_log.read_bytes()
                    added = contents[offset:] if len(contents) >= offset else contents
                    assert b"Validation Error:" not in added, f"{name}: {added[:1000].decode(errors='replace')}"
                return sorted(payload["frames"], key=lambda f: f["seq"])

            baseline = run("full", {"selection": "all"})
            assert [f["frame_index"] for f in baseline] == list(range(240))
            for name, scope, indices in [
                ("every-seven", {"selection": "every_n", "every_n": 7}, list(range(0, 240, 7))),
                ("i-pictures", {"selection": "decoded_i_picture"}, i_frames),
                ("partial", {"selection": "all", "start_frame": 29, "end_frame": 199}, list(range(29, 200))),
            ]:
                actual = run(name, scope)
                expected = [dict(baseline[index], seq=seq) for seq, index in enumerate(indices)]
                assert actual == expected, f"{name}: frame identity or metric changed"
                print(f"PASS {name}: {len(actual)} exact frame records")
            assert run("full-reuse", {"selection": "all"}) == baseline
            print("PASS decoder reuse")
        return 0
    finally:
        if worker.process.poll() is None:
            worker.send(protocol_version=1, type="shutdown", request_id="shutdown")
            try:
                worker.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                worker.process.kill()
                worker.process.wait()


if __name__ == "__main__":
    raise SystemExit(main())

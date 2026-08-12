#!/usr/bin/env python3
"""Integration coverage for the in-process FFmpeg worker commands."""

import json
import os
import pathlib
import selectors
import shutil
import subprocess
import sys
import tempfile
from array import array

ENGINE = sys.argv[1]


class Worker:
    def __init__(self):
        self.process = subprocess.Popen(
            [ENGINE, "worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=False)
        self.buffer = b""

    def send(self, **payload):
        line = {"protocol_version": 1, **payload}
        self.process.stdin.write(json.dumps(line).encode() + b"\n")
        self.process.stdin.flush()

    def event(self, timeout=60):
        while b"\n" not in self.buffer:
            selector = selectors.DefaultSelector()
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout):
                raise TimeoutError("media worker event timed out")
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(self.process.stderr.read().decode(errors="replace"))
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def terminal(self, request_id, timeout=60):
        warnings = []
        while True:
            event = self.event(timeout)
            if event.get("request_id") != request_id:
                continue
            if event["type"] == "warning":
                warnings.append(event)
            if event["type"] in ("result", "error", "cancelled"):
                return event, warnings

    def close(self):
        self.send(type="shutdown", request_id="shutdown")
        while self.event()["type"] != "shutdown":
            pass
        assert self.process.wait(timeout=5) == 0


def encode(ffmpeg, output, frames=120, size="160x90"):
    return subprocess.run([
        ffmpeg, "-y", "-v", "error", "-f", "lavfi",
        "-i", f"testsrc2=size={size}:rate=24", "-frames:v", str(frames),
        "-c:v", "libx264", "-g", "24", "-bf", "3", "-pix_fmt", "yuv420p",
        str(output),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def begin(worker, request_id, kind, media, cache, **fields):
    worker.send(type=kind, request_id=request_id, path=str(media),
                cache_directory=str(cache), **fields)
    event, warnings = worker.terminal(request_id)
    assert event["type"] == "result", event
    return event["payload"], warnings


def main():
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        print("SKIP media worker integration: fixture FFmpeg unavailable")
        return 0
    with tempfile.TemporaryDirectory(prefix="getnative-media-worker-") as temporary:
        root = pathlib.Path(temporary)
        media, cache = root / "long-gop.mp4", root / "cache"
        if not encode(ffmpeg, media):
            print("SKIP media worker integration: libx264 unavailable")
            return 0

        worker = Worker()
        worker.send(type="hello", request_id="hello")
        hello = worker.event()
        assert hello["commands"]["media_index_begin"] is True

        indexed, _ = begin(worker, "index-1", "media_index_begin", media, cache,
                           decoder="software")
        index_path = pathlib.Path(indexed["index_path"])
        assert indexed["frame_count"] == 120 and indexed["rebuilt"] is True
        assert index_path.read_bytes()[:8] == b"GNVFLWI\0"
        cached, _ = begin(worker, "index-2", "media_index_begin", media, cache,
                          decoder="software")
        assert cached["rebuilt"] is False

        index_path.write_bytes(b"GNVFLWI\0corrupt")
        rebuilt, _ = begin(worker, "corrupt", "media_index_begin", media, cache,
                           decoder="software")
        assert rebuilt["rebuilt"] is True and index_path.stat().st_size > 1000

        window, _ = begin(
            worker, "window", "media_frame_window", media, cache,
            stream_index=indexed["stream_index"], target="frame", frame_index=119,
            window_radius=12)
        assert window["selected"]["frame_index"] == 119
        assert window["previous_keyframe"]["frame_index"] >= 96

        preview, _ = begin(
            worker, "preview", "media_preview_begin", media, cache,
            stream_index=indexed["stream_index"], frame_index=119,
            maximum_dimension=96)
        png = pathlib.Path(preview["asset"]["path"]).read_bytes()
        assert png[:8] == b"\x89PNG\r\n\x1a\n" and png[25] == 2
        assert preview["decoded_frames"] <= 30

        checksums, _ = begin(
            worker, "pixel-check", "media_asset_batch_begin", media, cache,
            stream_index=indexed["stream_index"], width=indexed["width"],
            height=indexed["height"],
            assets=[{"item_id": str(frame), "frame_index": frame,
                     "format": "f32le"} for frame in (0, 60, 119)])
        for asset in checksums["assets"]:
            frame = asset["frame_index"]
            baseline = subprocess.check_output([
                ffmpeg, "-v", "error", "-i", str(media), "-vf",
                f"select=eq(n\\,{frame})", "-frames:v", "1", "-pix_fmt",
                "grayf32le", "-f", "rawvideo", "pipe:1"])
            actual = pathlib.Path(asset["path"]).read_bytes()
            assert len(actual) == len(baseline)
            actual_values, baseline_values = array("f"), array("f")
            actual_values.frombytes(actual)
            baseline_values.frombytes(baseline)
            assert max(abs(left - right) for left, right in zip(
                actual_values, baseline_values)) <= 1e-6

        frames = list(range(95, 120))
        batch, _ = begin(
            worker, "batch", "media_asset_batch_begin", media, cache,
            stream_index=indexed["stream_index"],
            assets=[{"item_id": f"thumb-{frame}", "frame_index": frame,
                     "format": "png", "maximum_dimension": 80} for frame in frames])
        assert len(batch["assets"]) == 25 and batch["decoded_frames"] <= 48

        worker.send(
            type="media_asset_batch_begin", request_id="duplicate", path=str(media),
            cache_directory=str(cache), stream_index=indexed["stream_index"],
            assets=[
                {"item_id": "same", "frame_index": 2, "format": "png"},
                {"item_id": "middle", "frame_index": 3, "format": "png"},
                {"item_id": "same", "frame_index": 4, "format": "png"}])
        duplicate, _ = worker.terminal("duplicate")
        assert duplicate["type"] == "error" and duplicate["code"] == "bad_request"

        auto_media = root / "automatic.mp4"
        shutil.copy2(media, auto_media)
        automatic, _ = begin(worker, "auto", "media_index_begin", auto_media,
                             cache, decoder="auto")
        auto_window, _ = begin(
            worker, "auto-window", "media_frame_window", auto_media, cache,
            stream_index=automatic["stream_index"], target="frame", frame_index=60,
            window_radius=4)
        software_window, _ = begin(
            worker, "software-window", "media_frame_window", media, cache,
            stream_index=indexed["stream_index"], target="frame", frame_index=60,
            window_radius=4)
        identity = lambda frame: (
            frame["frame_index"], frame.get("pts"), frame.get("best_effort_timestamp"),
            frame["key_frame"], frame.get("picture_type"), frame["keyframe_anchor"])
        assert [identity(frame) for frame in auto_window["frames"]] \
            == [identity(frame) for frame in software_window["frames"]]

        changed_media = root / "changed.mp4"
        shutil.copy2(media, changed_media)
        before, _ = begin(worker, "changed-before", "media_index_begin",
                          changed_media, cache, decoder="software")
        with changed_media.open("ab") as output:
            output.write(b"source changed")
        after, _ = begin(worker, "changed-after", "media_index_begin",
                         changed_media, cache, decoder="software")
        assert after["rebuilt"] is True and after["fingerprint"] != before["fingerprint"]

        vfr_media = root / "vfr.mkv"
        vfr_encoded = subprocess.run([
            ffmpeg, "-y", "-v", "error",
            "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=12:duration=1",
            "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=30:duration=1",
            "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0,settb=1/1000[v]",
            "-map", "[v]", "-c:v", "libx264", "-g", "12", "-bf", "2",
            "-fps_mode:v", "vfr", str(vfr_media),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if vfr_encoded:
            vfr, _ = begin(worker, "vfr-index", "media_index_begin",
                           vfr_media, cache, decoder="software")
            vfr_window, _ = begin(
                worker, "vfr-window", "media_frame_window", vfr_media, cache,
                stream_index=vfr["stream_index"], target="frame",
                frame_index=vfr["frame_count"] // 2, window_radius=20)
            timestamps = [frame["timestamp_seconds"] for frame in vfr_window["frames"]
                          if frame.get("timestamp_seconds") is not None]
            steps = {round(right - left, 4)
                     for left, right in zip(timestamps, timestamps[1:])}
            assert len(steps) >= 2

        missing_media = root / "missing-timestamps.h264"
        missing_encoded = subprocess.run([
            ffmpeg, "-y", "-v", "error", "-f", "lavfi",
            "-i", "testsrc2=size=96x64:rate=24", "-frames:v", "30",
            "-c:v", "libx264", "-g", "15", "-bf", "2", "-f", "h264",
            str(missing_media),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if missing_encoded:
            missing, _ = begin(worker, "missing-index", "media_index_begin",
                               missing_media, cache, decoder="software")
            missing_window, _ = begin(
                worker, "missing-window", "media_frame_window", missing_media, cache,
                stream_index=missing["stream_index"], target="frame", frame_index=29,
                window_radius=2)
            assert any(frame.get("best_effort_timestamp") is None
                       for frame in missing_window["frames"])
            missing_preview, _ = begin(
                worker, "missing-preview", "media_preview_begin", missing_media, cache,
                stream_index=missing["stream_index"], frame_index=29,
                maximum_dimension=64)
            assert missing_preview["decoded_frames"] >= 30

        readonly = root / "readonly"
        readonly.mkdir()
        readonly_media = readonly / "readonly.mp4"
        shutil.copy2(media, readonly_media)
        readonly.chmod(0o555)
        try:
            fallback_cache = root / "fallback-cache"
            fallback, _ = begin(worker, "readonly", "media_index_begin",
                                readonly_media, fallback_cache, decoder="software")
            assert pathlib.Path(fallback["index_path"]).is_relative_to(fallback_cache)
        finally:
            readonly.chmod(0o755)

        long_media = root / "cancel.mp4"
        if encode(ffmpeg, long_media, frames=6000, size="64x64"):
            cancel_cache = root / "cancel-cache"
            command = dict(type="media_index_begin", path=str(long_media),
                           cache_directory=str(cancel_cache), decoder="software")
            worker.send(request_id="cancel-a", **command)
            worker.send(request_id="cancel-b", **command)
            accepted = {}
            while len(accepted) < 2:
                event = worker.event()
                if event["type"] == "accepted" and event.get("request_id") in {
                        "cancel-a", "cancel-b"}:
                    accepted[event["request_id"]] = event["job_id"]
            assert accepted["cancel-a"] == accepted["cancel-b"]
            worker.send(type="cancel", request_id="cancel-job",
                        job_id=accepted["cancel-a"])
            terminals = {}
            while len(terminals) < 2:
                event = worker.event()
                if event.get("request_id") in accepted and event["type"] in {
                        "cancelled", "result", "error"}:
                    terminals[event["request_id"]] = event
            assert all(event["type"] == "cancelled" for event in terminals.values())
            assert not list(root.rglob("*.tmp"))

        worker.close()
    print("PASS media worker integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

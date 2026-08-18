#!/usr/bin/env python3
"""Integration coverage for the in-process FFmpeg worker commands."""

import json
import os
import pathlib
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
from array import array

ENGINE = sys.argv[1]


class Worker:
    def __init__(self):
        self.process = subprocess.Popen(
            [ENGINE, "worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=False)
        self.stdout_lines = queue.Queue()

        def read_stdout():
            try:
                for line in self.process.stdout:
                    self.stdout_lines.put(line)
            except BaseException as error:
                self.stdout_lines.put(error)
            finally:
                self.stdout_lines.put(None)

        self.stdout_thread = threading.Thread(target=read_stdout, daemon=True)
        self.stdout_thread.start()

    def send(self, **payload):
        line = {"protocol_version": 1, **payload}
        self.process.stdin.write(json.dumps(line).encode() + b"\n")
        self.process.stdin.flush()

    def event(self, timeout=60):
        try:
            line = self.stdout_lines.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError("media worker event timed out") from error
        if line is None:
            raise RuntimeError(self.process.stderr.read().decode(errors="replace"))
        if isinstance(line, BaseException):
            raise line
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


def lwi_ascii_lines(index_path):
    """Yield LWI record lines without decoding ExtraData blobs as text."""
    data = index_path.read_bytes()
    offset = 0
    in_extra_data = False
    while offset < len(data):
        line_end = data.find(b"\n", offset)
        if line_end < 0:
            break
        raw_line = data[offset:line_end].rstrip(b"\r")
        offset = line_end + 1
        line = raw_line.decode("ascii", errors="replace")
        yield line
        if line.startswith("<ExtraDataList="):
            in_extra_data = True
            continue
        if line == "</ExtraDataList>":
            in_extra_data = False
            continue
        if not in_extra_data or not line.startswith("Size="):
            continue
        fields = dict(part.split("=", 1) for part in line.split(",") if "=" in part)
        blob_size = int(fields.get("Size", "0"))
        if blob_size < 0 or offset + blob_size > len(data):
            raise AssertionError("truncated LWI ExtraData blob")
        offset += blob_size
        if data[offset:offset + 2] == b"\r\n":
            offset += 2
        elif data[offset:offset + 1] in (b"\r", b"\n"):
            offset += 1


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

        rap_test = pathlib.Path(ENGINE).with_name(
            "getnative_media_index_compat_tests.exe" if os.name == "nt"
            else "getnative_media_index_compat_tests")
        if rap_test.is_file():
            subprocess.run([rap_test, "--verify-rap", media], check=True)
            subprocess.run([rap_test, "--verify-decode-planner", media], check=True)

        worker = Worker()
        worker.send(type="hello", request_id="hello")
        hello = worker.event()
        assert hello["commands"]["media_index_begin"] is True

        indexed, _ = begin(worker, "index-1", "media_index_begin", media, cache,
                           decoder="software")
        index_path = pathlib.Path(indexed["index_path"])
        assert indexed["frame_count"] == 120 and indexed["rebuilt"] is True
        assert index_path.name.endswith(".vf.lwi")
        assert index_path.read_bytes().startswith(b"<LSMASHWorksIndexVersion")
        assert indexed["index_mode"] == "packet_fast"
        assert indexed["selective_decodes"] == 0
        assert indexed["packet_count"] == 120
        cached, _ = begin(worker, "index-2", "media_index_begin", media, cache,
                          decoder="software")
        assert cached["rebuilt"] is False

        index_path.write_bytes(b"<LSMASHWorksIndexVersion=broken")
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
        assert preview["decoder"] == "software"
        assert preview["decoded_frames"] <= 30, preview

        software_preview, _ = begin(
            worker, "preview-software", "media_preview_begin", media, cache,
            stream_index=indexed["stream_index"], frame_index=118,
            maximum_dimension=96, decoder="software")
        assert software_preview["decoder"] == "software"

        smaller_preview, _ = begin(
            worker, "preview-smaller", "media_preview_begin", media, cache,
            stream_index=indexed["stream_index"], frame_index=118,
            maximum_dimension=80)
        shutil.copyfile(smaller_preview["asset"]["path"],
                        software_preview["asset"]["path"])
        resized_cache_hit, _ = begin(
            worker, "preview-cached-dimensions", "media_preview_begin", media, cache,
            stream_index=indexed["stream_index"], frame_index=118,
            maximum_dimension=96)
        assert resized_cache_hit["asset"]["from_cache"] is True
        assert (resized_cache_hit["asset"]["width"],
                resized_cache_hit["asset"]["height"]) == (
                    smaller_preview["asset"]["width"],
                    smaller_preview["asset"]["height"])

        for decoder in ("nvdec", "vulkan_video"):
            request_id = f"preview-unsupported-{decoder}"
            worker.send(
                type="media_preview_begin", request_id=request_id,
                path=str(media), cache_directory=str(cache),
                stream_index=indexed["stream_index"], frame_index=117,
                maximum_dimension=96, decoder=decoder)
            rejected, _ = worker.terminal(request_id)
            assert rejected["type"] == "error" and rejected["code"] == "unsupported", rejected

        worker.send(
            type="media_asset_batch_begin", request_id="batch-preview-unsupported",
            path=str(media), cache_directory=str(cache),
            stream_index=indexed["stream_index"], decoder="nvdec",
            assets=[{"item_id": "png", "frame_index": 1,
                     "format": "png", "maximum_dimension": 80}])
        rejected_batch, _ = worker.terminal("batch-preview-unsupported")
        assert (rejected_batch["type"] == "error"
                and rejected_batch["code"] == "unsupported"), rejected_batch

        cached_preview, _ = begin(
            worker, "preview-cache-hit", "media_preview_begin", media, cache,
            stream_index=indexed["stream_index"], frame_index=119,
            maximum_dimension=96)
        assert cached_preview["asset"]["from_cache"] is True
        assert cached_preview["decoded_frames"] == 0
        assert cached_preview["decoder"] == "software"

        unicode_root = root / "媒体-日本語-한글"
        unicode_root.mkdir()
        unicode_media = unicode_root / "视频-夢-영상.mp4"
        unicode_cache = unicode_root / "缓存-キャッシュ-캐시"
        shutil.copy2(media, unicode_media)
        unicode_indexed, _ = begin(
            worker, "unicode-index", "media_index_begin", unicode_media,
            unicode_cache, decoder="software")
        assert unicode_indexed["frame_count"] == 120
        assert pathlib.Path(unicode_indexed["index_path"]).is_file()
        unicode_preview, _ = begin(
            worker, "unicode-preview", "media_preview_begin", unicode_media,
            unicode_cache, stream_index=unicode_indexed["stream_index"],
            frame_index=119, maximum_dimension=96)
        unicode_png = pathlib.Path(unicode_preview["asset"]["path"])
        assert unicode_png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
        assert unicode_cache in unicode_png.parents

        # MPEG-TS has no keyframe index; a timestamp seek lands on the last
        # packet with dts <= target, which with B-frame reordering sits past
        # the keyframe. Previewing must still decode from the anchor GOP.
        ts_media = root / "bframes.m2ts"
        assert encode(ffmpeg, ts_media)
        ts_indexed, _ = begin(
            worker, "ts-index", "media_index_begin", ts_media,
            cache, decoder="software")
        assert ts_indexed["frame_count"] == 120
        for name, frame in (("ts-preview-start", 0), ("ts-preview-mid", 100)):
            ts_preview, _ = begin(
                worker, name, "media_preview_begin", ts_media, cache,
                stream_index=ts_indexed["stream_index"], frame_index=frame,
                maximum_dimension=96)
            ts_png = pathlib.Path(ts_preview["asset"]["path"]).read_bytes()
            assert ts_png[:8] == b"\x89PNG\r\n\x1a\n"
            assert ts_preview["decoded_frames"] <= 30

        # Corrupted packets inside the target GOP must not abort decoding:
        # the bad packet is dropped (ffmpeg CLI semantics) and later frames
        # still decode with verified timestamps. Size and mtime are preserved
        # so the cached index stays authoritative for packet positions.
        stat = ts_media.stat()
        records = []
        current = None
        for line in lwi_ascii_lines(pathlib.Path(ts_indexed["index_path"])):
            if line.startswith("Index="):
                fields = dict(p.split("=", 1) for p in line[6:].split(",") if "=" in p)
                current = {"pos": int(fields["POS"]), "pts": int(fields["PTS"])}
            elif line.startswith("Key=") and current is not None:
                current["pic"] = int(line[4:].split(",")[1].split("=")[1])
                records.append(current)
                current = None
        presentation = sorted(range(len(records)), key=lambda i: records[i]["pts"])
        b_pos = b_ordinal = None
        for ordinal, record_index in enumerate(presentation):
            if records[record_index]["pic"] == 3 and 40 < ordinal < 80:
                b_pos, b_ordinal = records[record_index]["pos"], ordinal
                break
        assert b_pos is not None
        with open(ts_media, "r+b") as damaged:
            damaged.seek(b_pos + 400)
            damaged.write(b"\xff" * 128)
        os.utime(ts_media, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        for name, frame in (("ts-preview-damaged", b_ordinal + 1),
                            ("ts-preview-damaged-later", b_ordinal + 2)):
            ts_preview, _ = begin(
                worker, name, "media_preview_begin", ts_media, cache,
                stream_index=ts_indexed["stream_index"], frame_index=frame,
                maximum_dimension=96)
            ts_png = pathlib.Path(ts_preview["asset"]["path"]).read_bytes()
            assert ts_png[:8] == b"\x89PNG\r\n\x1a\n"
            assert "discarded_packets" in ts_preview

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
            maximum_error = max(abs(left - right) for left, right in zip(
                actual_values, baseline_values))
            assert maximum_error <= 1e-6, (frame, maximum_error)

        # Request order is part of the response contract, but decode order must
        # be normalized internally for a persistent indexed session.
        frames = list(reversed(range(95, 120)))
        batch, _ = begin(
            worker, "batch", "media_asset_batch_begin", media, cache,
            stream_index=indexed["stream_index"],
            assets=[{"item_id": f"thumb-{frame}", "frame_index": frame,
                     "format": "png", "maximum_dimension": 80} for frame in frames])
        batch_decode_limit = 48 if batch["decoder"] == "software" else 72
        assert len(batch["assets"]) == 25, batch
        assert batch["decoder"] == "software"
        assert batch["decoded_frames"] <= batch_decode_limit, batch
        assert batch["decode_retries"] == 0, batch

        cached_batch, _ = begin(
            worker, "batch-cache-hit", "media_asset_batch_begin", media, cache,
            stream_index=indexed["stream_index"],
            assets=[{"item_id": f"thumb-{frame}", "frame_index": frame,
                     "format": "png", "maximum_dimension": 80} for frame in frames])
        assert all(asset["from_cache"] is True for asset in cached_batch["assets"])
        assert cached_batch["decoded_frames"] == 0

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
            # Raw elementary streams have no usable timestamps, but the LWI
            # packet position still permits a GOP-local byte seek.
            assert 1 <= missing_preview["decoded_frames"] <= 15

        blocked = root / "blocked-sidecar"
        blocked.mkdir()
        blocked_media = blocked / "blocked.mp4"
        shutil.copy2(media, blocked_media)
        pathlib.Path(f"{blocked_media}.vf.lwi").mkdir()
        fallback_cache = root / "fallback-cache"
        fallback, _ = begin(worker, "blocked", "media_index_begin",
                            blocked_media, fallback_cache, decoder="software")
        assert pathlib.Path(fallback["index_path"]).is_relative_to(fallback_cache)

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

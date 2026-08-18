#!/usr/bin/env python3
"""Spot-check indexed seek placement on the NAS m2ts regression sample."""

import json
import pathlib
import queue
import subprocess
import sys
import threading

ENGINE = sys.argv[1]
MEDIA = (
    r"\\TRUENAS\Dataset0\EBackup\Resource workspace\[190220] BanG Dream! "
    r"Pastel＊Palettes 4th シングル「天下卜ーイツ A to Z☆」[FLAC+CUE+LOG+BK+BDMV]"
    r"\BRMM_10164BD\BDMV\STREAM\00001.m2ts"
)
CACHE = sys.argv[2] if len(sys.argv) > 2 else r"C:\Users\lsy39\AppData\Local\Temp\gnvf-seek-check"
DECODER = sys.argv[3] if len(sys.argv) > 3 else "auto"


class Worker:
    def __init__(self):
        self.process = subprocess.Popen(
            [ENGINE, "worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=False)
        self.lines = queue.Queue()

        def pump():
            try:
                for line in self.process.stdout:
                    self.lines.put(line)
            finally:
                self.lines.put(None)

        threading.Thread(target=pump, daemon=True).start()

    def send(self, **payload):
        self.process.stdin.write(
            json.dumps({"protocol_version": 1, **payload}).encode() + b"\n")
        self.process.stdin.flush()

    def terminal(self, request_id):
        warnings = []
        while True:
            line = self.lines.get(timeout=300)
            assert line is not None, "worker stdout closed"
            event = json.loads(line)
            if event.get("request_id") != request_id:
                continue
            if event["type"] == "warning":
                warnings.append(event["message"])
                continue
            if event["type"] in ("result", "error", "cancelled", "hello_ok"):
                return event, warnings

    def close(self):
        self.send(type="shutdown", request_id="shutdown")
        while json.loads(self.lines.get(timeout=30))["type"] != "shutdown":
            pass
        assert self.process.wait(timeout=10) == 0


def main():
    worker = Worker()
    worker.send(type="hello", request_id="hello")
    worker.terminal("hello")

    worker.send(type="media_index_begin", request_id="index", path=MEDIA,
                cache_directory=CACHE, decoder=DECODER)
    indexed, warnings = worker.terminal("index")
    assert indexed["type"] == "result", indexed
    payload = indexed["payload"]
    frames = payload["frame_count"]
    stream = payload["stream_index"]
    print(f"index: {frames} frames, decoder={payload['decoder']}, "
          f"rebuilt={payload['rebuilt']}, warnings={warnings}")

    for target in (0, 100, 5000):
        worker.send(type="media_preview_begin", request_id=f"p{target}", path=MEDIA,
                    cache_directory=CACHE, stream_index=stream, frame_index=target,
                    maximum_dimension=640, decoder=DECODER)
        preview, warnings = worker.terminal(f"p{target}")
        assert preview["type"] == "result", preview
        body = preview["payload"]
        png = pathlib.Path(body["asset"]["path"]).read_bytes()
        assert png[:8] == b"\x89PNG\r\n\x1a\n"
        window = body.get("window") or {}
        selected = window.get("selected", {})
        anchor = selected.get("keyframe_anchor")
        expected_max = target - (anchor or 0) + 1
        ok = body["decoded_frames"] <= expected_max
        print(f"frame {target}: decoded={body['decoded_frames']} "
              f"(anchor={anchor}, expect<={expected_max}) decoder={body['decoder']} "
              f"cache={body['asset']['from_cache']} warnings={warnings} -> {'OK' if ok else 'BAD'}")
        assert ok, f"decoded frame count overshoot at {target}"
        assert selected.get("frame_index") == target

    # Open-GOP leading picture: frame 11773 is presented immediately before
    # the RAP at 11774 but is decoded only after that RAP is submitted. This
    # used to make the thumbnail batch fail with "all indexed GOP frames".
    if frames > 11774:
        target = 11773
        worker.send(type="media_preview_begin", request_id="open-gop-leading",
                    path=MEDIA, cache_directory=CACHE, stream_index=stream,
                    frame_index=target, maximum_dimension=640, decoder=DECODER)
        leading, warnings = worker.terminal("open-gop-leading")
        assert leading["type"] == "result", leading
        body = leading["payload"]
        selected = (body.get("window") or {}).get("selected", {})
        assert selected.get("frame_index") == target
        assert selected.get("keyframe_anchor", target) > target
        png = pathlib.Path(body["asset"]["path"]).read_bytes()
        assert png[:8] == b"\x89PNG\r\n\x1a\n"
        print(f"open-GOP leading frame {target}: anchor={selected['keyframe_anchor']}"
              f" decoder={body['decoder']} warnings={warnings} -> OK")

    # Second pass: identical frames must come from the disk cache untouched.
    for target in (0, 100, 5000):
        worker.send(type="media_preview_begin", request_id=f"c{target}", path=MEDIA,
                    cache_directory=CACHE, stream_index=stream, frame_index=target,
                    maximum_dimension=640, decoder=DECODER)
        preview, _ = worker.terminal(f"c{target}")
        body = preview["payload"]
        assert body["asset"]["from_cache"] is True and body["decoded_frames"] == 0, body
    print("cache hits: OK")
    worker.close()
    print("PASS NAS seek spot-check")


if __name__ == "__main__":
    raise SystemExit(main())

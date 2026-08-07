#!/usr/bin/env python3
"""Episode-scale verification benchmark over the worker protocol (E2 evidence).

Drives `getnative-engine worker` verify mode with a ring of 8 predecoded
1920x1080 f32le frame assets — the same shape as the fixed-recipe multi-frame
prototype — for 1,000-frame streams, serial (worker_count=1) versus
frame-parallel (worker_count=0), across representative kernels.

Usage: verify_episode_probe.py <engine-binary> [frame-count]
Output: one JSON object per configuration on stdout.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else None
FRAME_COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
WIDTH, HEIGHT = 1920, 1080
RING_SIZE = 8
CANDIDATE = "810"

KERNELS = [
    {"id": "bilinear"},
    {"id": "bicubic", "b": 0.0, "c": 0.5},
    {"id": "lanczos", "taps": 6},
]

FRAME_CACHE = os.path.join(tempfile.gettempdir(), "getnative-verify-episode-frames")


def ensure_ring():
    os.makedirs(FRAME_CACHE, exist_ok=True)
    paths = [os.path.join(FRAME_CACHE, f"ring-{index}.f32") for index in range(RING_SIZE)]
    if all(os.path.getsize(path) == WIDTH * HEIGHT * 4 for path in paths
           if os.path.exists(path)):
        if all(os.path.exists(path) for path in paths):
            return paths
    for index, path in enumerate(paths):
        tmp = path + ".tmp"
        with open(tmp, "wb") as output:
            for y in range(HEIGHT):
                row = struct.pack(
                    f"<{WIDTH}f",
                    *[(0.5 + 0.1 * ((x * 7 + y * 13 + index) % 17) / 16.0)
                      for x in range(WIDTH)],
                )
                output.write(row)
        os.replace(tmp, path)
    return paths


class Worker:
    def __init__(self):
        self.process = subprocess.Popen(
            [ENGINE, "worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self._stdout_buffer = b""

    def send(self, **command):
        self.process.stdin.write(json.dumps(command).encode() + b"\n")
        self.process.stdin.flush()

    def read_event(self, timeout=300.0):
        import selectors

        while b"\n" not in self._stdout_buffer:
            selector = selectors.DefaultSelector()
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout):
                raise TimeoutError("worker did not emit an event in time")
            chunk = os.read(self.process.stdout.fileno(), 1 << 20)
            if not chunk:
                raise EOFError("worker stdout closed unexpectedly")
            self._stdout_buffer += chunk
        line, self._stdout_buffer = self._stdout_buffer.split(b"\n", 1)
        return json.loads(line)


def run_configuration(paths, kernel, worker_count, frame_count):
    worker = Worker()
    worker.send(**{"protocol_version": 1, "type": "hello", "request_id": "h"})
    worker.read_event()
    worker.send(
        **{
            "protocol_version": 1,
            "type": "verify_begin",
            "request_id": "b",
            "geometry": {"width": WIDTH, "height": HEIGHT},
            "axis_mode": "h_only",
            "kernel": kernel,
            "candidate": CANDIDATE,
            "metric": {"p_norm": 1},
            "backend": "cpu",
            "worker_count": worker_count,
            "expected_frames": frame_count,
        }
    )
    accepted = worker.read_event()
    job = accepted["job_id"]
    effective_workers = accepted["worker_count"]

    started = time.monotonic()
    for seq in range(frame_count):
        worker.send(
            **{
                "protocol_version": 1,
                "type": "verify_frame",
                "request_id": f"f{seq}",
                "job_id": job,
                "seq": seq,
                "frame_asset": {
                    "path": paths[seq % RING_SIZE],
                    "format": "f32le",
                    "width": WIDTH,
                    "height": HEIGHT,
                },
            }
        )
    worker.send(**{"protocol_version": 1, "type": "verify_end",
                   "request_id": "e", "job_id": job, "total": frame_count})

    result_errors = {}
    terminal = None
    while True:
        event = worker.read_event()
        if event["type"] == "progress":
            for entry in event.get("results", []):
                result_errors[entry["seq"]] = entry["error"]
        elif event["type"] in ("result", "error", "cancelled"):
            terminal = event
            break
    wall_ms = (time.monotonic() - started) * 1000.0

    worker.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "s"})
    while worker.read_event()["type"] != "shutdown":
        pass
    worker.process.wait(timeout=30.0)

    if terminal["type"] != "result":
        raise RuntimeError(f"verify failed: {json.dumps(terminal)[:400]}")
    payload = terminal["payload"]
    telemetry = payload["telemetry"]
    if payload["frames_completed"] != frame_count or payload["frames_failed"] != 0:
        raise RuntimeError(f"frame accounting wrong: {json.dumps(payload)[:400]}")
    return {
        "kernel": kernel["id"] + (f"{kernel['taps']}" if "taps" in kernel else ""),
        "requested_worker_count": worker_count,
        "effective_worker_count": effective_workers,
        "frames": frame_count,
        "wall_ms": round(wall_ms, 3),
        "wall_fps": round(frame_count * 1000.0 / wall_ms, 1),
        "engine_stream_ms": telemetry["stream_ms"],
        "engine_fps": telemetry["fps"],
        "frame_load_ms": telemetry["frame_load_ms"],
        "frame_analyze_ms": telemetry["frame_analyze_ms"],
        "plan_ms": telemetry["plan_ms"],
        "plan_cache_hits": telemetry["plan_cache_hits"],
        "sample_error_seq0": result_errors.get(0),
        "sample_error_seq1": result_errors.get(1),
    }


def main():
    if not ENGINE:
        raise SystemExit("usage: verify_episode_probe.py <engine-binary> [frame-count]")
    paths = ensure_ring()
    for kernel in KERNELS:
        for worker_count in (1, 0):
            report = run_configuration(paths, kernel, worker_count, FRAME_COUNT)
            print(json.dumps(report), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

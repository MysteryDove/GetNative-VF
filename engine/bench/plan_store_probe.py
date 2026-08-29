#!/usr/bin/env python3
"""E4 cold plan store evidence: repeated 1,000-plan scans across processes.

Drives `getnative-engine worker` analyze jobs for the 1080 -> 700..1699
grid at Lanczos-6 and Lanczos-8 (the fixed-admission cliff shapes from
docs/cold-plan-cache-evaluation.md §2), with an isolated store dir:

  process A: cold build (builds=1000) and write-behind publish
  process B job 1: cross-process store fetch (effective hit rate)
  process B job 2: same-process rescan (L1 + L2 mix, zero rebuilds)

Usage: plan_store_probe.py <engine-binary> [frame.f32]
Output: JSON lines.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile

ENGINE = sys.argv[1] if len(sys.argv) > 1 else None
FRAME = sys.argv[2] if len(sys.argv) > 2 else None
WIDTH, HEIGHT = 1920, 1080
GRID = [f"{700 + i * 0.38:.2f}" for i in range(1000)]  # 700..1079.62, all < 1080
KERNELS = [{"id": "lanczos", "taps": 6}, {"id": "lanczos", "taps": 8}]


def ensure_frame():
    if FRAME:
        return FRAME
    path = os.path.join(tempfile.gettempdir(), "getnative-store-probe-frame.f32")
    if os.path.exists(path) and os.path.getsize(path) == WIDTH * HEIGHT * 4:
        return path
    tmp = path + ".tmp"
    with open(tmp, "wb") as output:
        for y in range(HEIGHT):
            output.write(struct.pack(
                f"<{WIDTH}f",
                *[(0.5 + 0.1 * ((x * 7 + y * 13) % 17) / 16.0) for x in range(WIDTH)]))
    os.replace(tmp, path)
    return path


class Worker:
    def __init__(self, store_dir):
        env = dict(os.environ)
        env["GETNATIVE_PLAN_CACHE_DIR"] = store_dir
        self.process = subprocess.Popen(
            [ENGINE, "worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        self._stdout_buffer = b""

    def send(self, **command):
        self.process.stdin.write(json.dumps(command).encode() + b"\n")
        self.process.stdin.flush()

    def read_event(self, timeout=600.0):
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

    def analyze(self, request_id, frame, kernel):
        self.send(**{
            "protocol_version": 1,
            "type": "analyze",
            "request_id": request_id,
            "mode": "height",
            "backend": "cpu",
            "frame_asset": {
                "path": frame, "format": "f32le", "width": WIDTH, "height": HEIGHT,
            },
            "axis_mode": "h_only",
            "kernel": kernel,
            "candidates": GRID,
            "metric": {"p_norm": 1},
        })
        while True:
            event = self.read_event()
            if event["type"] in ("result", "error", "cancelled"):
                return event

    def shutdown(self, request_id):
        self.send(**{"protocol_version": 1, "type": "shutdown", "request_id": request_id})
        while self.read_event()["type"] != "shutdown":
            pass
        self.process.wait(timeout=60.0)


def telemetry_of(result):
    telemetry = result["payload"]["telemetry"]
    keys = ("plan_build_count", "plan_cache_hits", "plan_store_hits",
            "plan_store_fetch_ms", "plan_ms", "total_ms", "plan_resident_entries")
    return {key: telemetry.get(key) for key in keys}


def main():
    if not ENGINE:
        raise SystemExit("usage: plan_store_probe.py <engine-binary> [frame.f32]")
    frame = ensure_frame()
    for kernel in KERNELS:
        label = f"lanczos{kernel['taps']}"
        with tempfile.TemporaryDirectory() as store_dir:
            worker_a = Worker(store_dir)
            worker_a.send(**{"protocol_version": 1, "type": "hello", "request_id": "h"})
            worker_a.read_event()
            cold = worker_a.analyze("a1", frame, kernel)
            worker_a.shutdown("a2")

            worker_b = Worker(store_dir)
            worker_b.send(**{"protocol_version": 1, "type": "hello", "request_id": "h"})
            worker_b.read_event()
            warm = worker_b.analyze("b1", frame, kernel)
            rescan = worker_b.analyze("b2", frame, kernel)
            worker_b.shutdown("b3")

            pack_bytes = sum(
                os.path.getsize(os.path.join(store_dir, name))
                for name in os.listdir(store_dir) if name.endswith(".gnpk"))
            errors_cold = [c["error"] for c in cold["payload"]["candidates"]]
            errors_warm = [c["error"] for c in warm["payload"]["candidates"]]
            errors_rescan = [c["error"] for c in rescan["payload"]["candidates"]]
            print(json.dumps({
                "kernel": label,
                "candidates": len(GRID),
                "process_a_cold": telemetry_of(cold),
                "process_b_warm": telemetry_of(warm),
                "process_b_rescan": telemetry_of(rescan),
                "pack_bytes": pack_bytes,
                "parity_cold_warm": errors_cold == errors_warm,
                "parity_warm_rescan": errors_warm == errors_rescan,
            }), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

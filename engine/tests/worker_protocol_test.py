#!/usr/bin/env python3
"""Integration tests for the getnative-engine worker protocol v1.

Drives a real worker process over pipes: handshake, capability gating,
height analysis with session plan-cache retention, cooperative cancel,
error paths, and shutdown semantics. See docs/worker-protocol-v1.md.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile

ENGINE = sys.argv[1]
FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"PASS {name}")
    else:
        print(f"FAIL {name} {detail}")
        FAILURES.append(name)


def write_frame(path, width, height):
    with open(path, "wb") as output:
        for y in range(height):
            for x in range(width):
                value = 0.5 + 0.1 * ((x * 7 + y * 13) % 17) / 16.0
                output.write(struct.pack("<f", value))


class Worker:
    def __init__(self):
        self.process = subprocess.Popen(
            [ENGINE, "worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def send(self, **command):
        self.process.stdin.write(json.dumps(command) + "\n")
        self.process.stdin.flush()

    def send_raw(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def read_event(self, timeout=30.0):
        import selectors

        selector = selectors.DefaultSelector()
        selector.register(self.process.stdout, selectors.EVENT_READ)
        if not selector.select(timeout):
            raise TimeoutError("worker did not emit an event in time")
        line = self.process.stdout.readline()
        if not line:
            raise EOFError("worker stdout closed unexpectedly")
        return json.loads(line)

    def wait_exit(self, timeout=10.0):
        return self.process.wait(timeout=timeout)


def analyze_command(request_id, frame_path, candidates, width=320, height=240, **overrides):
    command = {
        "protocol_version": 1,
        "type": "analyze",
        "request_id": request_id,
        "mode": "height",
        "backend": "cpu",
        "frame_asset": {
            "path": frame_path,
            "format": "f32le",
            "width": width,
            "height": height,
        },
        "axis_mode": "h_only",
        "kernel": {"id": "bicubic", "b": 0, "c": 0.5},
        "candidates": candidates,
        "metric": {"p_norm": 1},
    }
    command.update(overrides)
    return command


def main():
    with tempfile.TemporaryDirectory() as scratch:
        frame = os.path.join(scratch, "frame.f32")
        write_frame(frame, 320, 240)

        # --- Session 1: happy path, cache retention, cancel, shutdown -------
        worker = Worker()

        # analyze before hello must be rejected.
        worker.send(**analyze_command("r0", frame, ["200"]))
        event = worker.read_event()
        check("greeting-required", event["type"] == "error" and event["code"] == "protocol_error",
              json.dumps(event))

        worker.send(**{"protocol_version": 1, "type": "hello", "request_id": "r1"})
        event = worker.read_event()
        check("hello-ok", event["type"] == "hello_ok"
              and event["commands"]["analyze"] and event["commands"]["cancel"])

        worker.send(**{"protocol_version": 1, "type": "capabilities", "request_id": "r2"})
        event = worker.read_event()
        payload = event.get("payload", {})
        cpu = next((b for b in payload.get("backends", []) if b.get("id") == "cpu"), {})
        check("capabilities-analyze-gating",
              event["type"] == "capabilities"
              and payload.get("commands", {}).get("analyze") is True
              and cpu.get("analysis_command_available") is True)

        # Malformed JSON line.
        worker.send_raw("{not json")
        event = worker.read_event()
        check("bad-json", event["type"] == "error" and event["code"] == "bad_request")

        # Unknown command type.
        worker.send(**{"protocol_version": 1, "type": "bogus", "request_id": "rX"})
        event = worker.read_event()
        check("unknown-command", event["type"] == "error" and event["code"] == "bad_request")

        # Unsupported mode and bad frame asset.
        worker.send(**analyze_command("rB", frame, ["200"], mode="kernel"))
        event = worker.read_event()
        check("unsupported-mode", event["type"] == "error" and event["code"] == "unsupported")
        worker.send(**analyze_command("rC", os.path.join(scratch, "missing.f32"), ["200"]))
        accepted = worker.read_event()
        event = worker.read_event()
        check("frame-asset-error", accepted["type"] == "accepted"
              and event["type"] == "error" and event["code"] == "frame_asset_error",
              json.dumps(event))

        # Happy path with a duplicate candidate.
        worker.send(**analyze_command("r3", frame, ["200", "201", "202", "200"]))
        events = []
        while True:
            event = worker.read_event()
            events.append(event)
            if event["type"] in ("result", "error", "cancelled"):
                break
        kinds = [event["type"] for event in events]
        check("analyze-flow", kinds[0] == "accepted" and kinds[-1] == "result"
              and "progress" in kinds, json.dumps(kinds))
        result = events[-1]["payload"]
        ids = [candidate["id"] for candidate in result["candidates"]]
        errors = [candidate["error"] for candidate in result["candidates"]]
        check("result-order-and-duplicates",
              ids == ["200", "201", "202", "200"]
              and errors[0] == errors[3]
              and all(isinstance(value, float) for value in errors),
              json.dumps(result["candidates"]))
        telemetry = result["telemetry"]
        check("first-job-telemetry",
              telemetry["plan_build_count"] == 3 and telemetry["plan_cache_hits"] == 0,
              json.dumps(telemetry))

        # Second job reuses the warm session cache for candidate "200".
        worker.send(**analyze_command("r4", frame, ["200", "203"]))
        while True:
            event = worker.read_event()
            if event["type"] in ("result", "error", "cancelled"):
                break
        check("session-cache-hit", event["type"] == "result"
              and event["payload"]["telemetry"]["plan_cache_hits"] == 1
              and event["payload"]["telemetry"]["plan_build_count"] == 1,
              json.dumps(event.get("payload", {}).get("telemetry", {})))

        # Cooperative cancel after the job starts: wait for the first
        # progress event, then cancel; the job must end cancelled.
        # Candidate values must stay below the source height (240).
        many = [str(100 + i * 0.02) for i in range(6000)]
        worker.send(**analyze_command("r5", frame, many))
        seen_progress = False
        cancelled = None
        result = None
        failed = None
        target_job = None
        for _ in range(4000):
            event = worker.read_event(timeout=60.0)
            if event["type"] == "accepted":
                target_job = event["job_id"]
            if event["type"] == "progress" and not seen_progress:
                seen_progress = True
                worker.send(**{"protocol_version": 1, "type": "cancel",
                               "request_id": "r6", "job_id": event["job_id"]})
            if event["type"] == "cancelled" and event.get("job_id") == target_job:
                cancelled = event
                break
            if event["type"] == "result":
                result = event
                break
            if event["type"] == "error":
                failed = event
                break
        check("cancel-after-start", seen_progress and cancelled is not None
              and cancelled["job_id"] == target_job,
              f"seen_progress={seen_progress} result={result is not None} error={failed}")
        if cancelled is not None:
            partial = cancelled["partial"]
            partial_count = len(cancelled.get("payload", {}).get("candidates", []))
            check("cancel-partial-consistency",
                  (partial and partial_count > 0) or (not partial and partial_count == 0),
                  json.dumps({"partial": partial, "count": partial_count}))

        # Cancelling an unknown job id is a no-op with not_running.
        worker.send(**{"protocol_version": 1, "type": "cancel",
                       "request_id": "r7", "job_id": "job-99"})
        event = worker.read_event()
        check("cancel-unknown", event["type"] == "cancelled"
              and event["detail"] == "not_running" and event["partial"] is False)

        # Queued jobs are reported cancelled on shutdown.
        worker.send(**analyze_command("r8a", frame, many))
        worker.send(**analyze_command("r8b", frame, ["200"]))
        worker.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "r9"})
        terminal = []
        while True:
            event = worker.read_event()
            terminal.append(event)
            if event["type"] == "shutdown":
                break
        queued_cancelled = [event for event in terminal
                            if event["type"] == "cancelled" and event.get("detail") == "shutdown"]
        check("shutdown-drains-queue", terminal[-1]["type"] == "shutdown"
              and len(queued_cancelled) >= 1, json.dumps([e["type"] for e in terminal]))
        check("exit-code", worker.wait_exit() == 0)

        # --- Session 2: EOF without shutdown exits cleanly ------------------
        worker = Worker()
        worker.send(**{"protocol_version": 1, "type": "hello", "request_id": "s2"})
        worker.read_event()
        worker.process.stdin.close()
        check("eof-exit", worker.wait_exit() == 0)

    if FAILURES:
        print(f"{len(FAILURES)} worker protocol test(s) failed")
        return 1
    print("worker protocol tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

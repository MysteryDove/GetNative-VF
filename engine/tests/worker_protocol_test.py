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
        print(f"PASS {name}", flush=True)
    else:
        print(f"FAIL {name} {detail}", flush=True)
        FAILURES.append(name)


def write_frame(path, width, height, seed=0):
    with open(path, "wb") as output:
        for y in range(height):
            for x in range(width):
                value = 0.5 + 0.1 * ((x * 7 + y * 13 + seed) % 17) / 16.0
                output.write(struct.pack("<f", value))


class Worker:
    def __init__(self, store_dir=None):
        # stdout stays binary: events are framed with an internal byte buffer
        # (select + buffered readline loses events that arrive in one chunk).
        # Each worker gets an isolated plan-store dir: the persistent cache
        # would otherwise leak plans across sessions and break telemetry
        # expectations (and the user's real cache must never be touched).
        env = None
        if store_dir is not None:
            env = dict(os.environ)
            env["GETNATIVE_PLAN_CACHE_DIR"] = store_dir
        else:
            env = dict(os.environ)
            env["GETNATIVE_PLAN_CACHE"] = "off"
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

    def send_raw(self, line):
        self.process.stdin.write(line.encode() + b"\n")
        self.process.stdin.flush()

    def read_event(self, timeout=30.0):
        import selectors

        while b"\n" not in self._stdout_buffer:
            selector = selectors.DefaultSelector()
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout):
                raise TimeoutError("worker did not emit an event in time")
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("worker stdout closed unexpectedly")
            self._stdout_buffer += chunk
        line, self._stdout_buffer = self._stdout_buffer.split(b"\n", 1)
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


def verify_begin_command(request_id, candidate, width=320, height=240, **overrides):
    command = {
        "protocol_version": 1,
        "type": "verify_begin",
        "request_id": request_id,
        "geometry": {"width": width, "height": height},
        "axis_mode": "h_only",
        "kernel": {"id": "bicubic", "b": 0, "c": 0.5},
        "candidate": candidate,
        "metric": {"p_norm": 1},
        "backend": "cpu",
    }
    command.update(overrides)
    return command


def verify_frame_command(request_id, job_id, seq, frame_path, width=320, height=240):
    return {
        "protocol_version": 1,
        "type": "verify_frame",
        "request_id": request_id,
        "job_id": job_id,
        "seq": seq,
        "frame_asset": {
            "path": frame_path,
            "format": "f32le",
            "width": width,
            "height": height,
        },
    }


def run_analyze(worker, command):
    worker.send(**command)
    while True:
        event = worker.read_event()
        if event["type"] in ("result", "error", "cancelled"):
            return event


def collect_verify(worker, timeout=60.0):
    """Collect (terminal, streamed_results, warnings) until a terminal event."""
    results = {}
    warnings = []
    while True:
        event = worker.read_event(timeout=timeout)
        if event["type"] == "progress":
            for entry in event.get("results", []):
                if entry["seq"] in results:
                    raise AssertionError(f"duplicate verify seq {entry['seq']}")
                results[entry["seq"]] = entry["error"]
        elif event["type"] == "warning":
            warnings.append(event)
        elif event["type"] in ("result", "error", "cancelled"):
            return event, results, warnings


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
        worker.send(**analyze_command("rB", frame, ["200"], mode="width"))
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

        # --- Session 3: CUDA backend parity and source residency ------------
        worker = Worker()
        worker.send(**{"protocol_version": 1, "type": "hello", "request_id": "c1"})
        worker.read_event()
        worker.send(**{"protocol_version": 1, "type": "capabilities", "request_id": "c2"})
        event = worker.read_event()
        cuda_backend = next(
            (b for b in event["payload"]["backends"] if b.get("id") == "cuda"), {})
        cuda_usable = (cuda_backend.get("compiled") and cuda_backend.get("device_available"))

        if not cuda_usable:
            print("SKIP cuda-parity (CUDA backend not available)")
            print("SKIP cuda-source-residency (CUDA backend not available)")
        else:
            # 40 candidates span two 32-wide chunks, exercising the CUDA
            # candidate pipeline (parallel chunk execution, ordered results).
            candidates = [str(190 + i) for i in range(40)]

            def run_job(request_id, backend):
                worker.send(**analyze_command(request_id, frame, candidates,
                                              backend=backend))
                while True:
                    event = worker.read_event()
                    if event["type"] in ("result", "error", "cancelled"):
                        return event

            cpu_result = run_job("c3", "cpu")
            cuda_result = run_job("c4", "cuda")
            if cpu_result["type"] != "result" or cuda_result["type"] != "result":
                check("cuda-parity", False,
                      f"cpu={cpu_result['type']} cuda={cuda_result['type']}: "
                      + json.dumps(cuda_result)[:400])
            else:
                cpu_errors = [c["error"] for c in cpu_result["payload"]["candidates"]]
                cuda_errors = [c["error"] for c in cuda_result["payload"]["candidates"]]
                close = all(
                    abs(c - g) <= max(1e-6, abs(c) * 1e-6)
                    for c, g in zip(cpu_errors, cuda_errors))
                check("cuda-parity", close,
                      f"cpu={cpu_errors} cuda={cuda_errors}")

            first_telemetry = cuda_result["payload"]["telemetry"]
            second = run_job("c5", "cuda")
            second_telemetry = second["payload"]["telemetry"]
            check("cuda-source-residency",
                  first_telemetry.get("cuda_source_upload_bytes", 0) > 0
                  and second_telemetry.get("cuda_source_upload_bytes", -1) == 0
                  and second_telemetry.get("cuda_source_cache_hits", 0) >= 1,
                  json.dumps({"first": first_telemetry.get("cuda_source_upload_bytes"),
                              "second": second_telemetry.get("cuda_source_upload_bytes")}))

        worker.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "c9"})
        while worker.read_event()["type"] != "shutdown":
            pass
        worker.wait_exit()

        # --- Session 4: verify streaming (protocol v1.1) --------------------
        worker = Worker()
        worker.send(**{"protocol_version": 1, "type": "hello", "request_id": "v0"})
        event = worker.read_event()
        check("verify-advertised", event["type"] == "hello_ok"
              and event["commands"].get("verify_begin") is True
              and event["commands"].get("verify_frame") is True
              and event["commands"].get("verify_end") is True,
              json.dumps(event.get("commands", {})))

        frame2 = os.path.join(scratch, "frame2.f32")
        write_frame(frame2, 320, 240, seed=5)

        # Height-mode baselines for parity checks (same session warms the
        # session plan cache that the verify job must then hit).
        base1 = run_analyze(worker, analyze_command("v1", frame, ["200"]))
        base2 = run_analyze(worker, analyze_command("v2", frame2, ["200"]))
        err_frame = base1["payload"]["candidates"][0]["error"]
        err_frame2 = base2["payload"]["candidates"][0]["error"]

        worker.send(**verify_begin_command("v3", "200", expected_frames=6))
        accepted = worker.read_event()
        check("verify-accepted", accepted["type"] == "accepted"
              and accepted["mode"] == "verify"
              and accepted["suggested_in_flight"] > 0,
              json.dumps(accepted))
        job = accepted["job_id"]
        ring = [frame, frame2, frame, frame2, frame, frame2]
        for seq, path in enumerate(ring):
            worker.send(**verify_frame_command(f"v3f{seq}", job, seq, path))
        worker.send(**{"protocol_version": 1, "type": "verify_end",
                       "request_id": "v3e", "job_id": job, "total": 6})
        terminal, results, warnings = collect_verify(worker)
        telemetry = terminal.get("payload", {}).get("telemetry", {})
        check("verify-flow", terminal["type"] == "result"
              and terminal["payload"]["frames_completed"] == 6
              and terminal["payload"]["frames_failed"] == 0
              and len(results) == 6 and not warnings
              and telemetry.get("fps", 0) > 0,
              json.dumps({"terminal": terminal["type"],
                          "results": len(results), "warnings": warnings}))
        check("verify-parity", terminal["type"] == "result"
              and all(abs(results[seq] - (err_frame if seq % 2 == 0 else err_frame2))
                      <= 1e-12 for seq in range(6)),
              json.dumps({"results": results, "even": err_frame, "odd": err_frame2}))
        # The earlier height jobs built the 240->200 plan; verify must reuse it.
        check("verify-plan-cache-hit",
              telemetry.get("plan_cache_hits") == 1
              and telemetry.get("plan_build_count") == 0,
              json.dumps(telemetry))

        # h_plus_w verify parity against the height-mode two-axis path.
        base2d = run_analyze(worker, analyze_command("v4", frame, ["200"],
                                                     axis_mode="h_plus_w"))
        err_2d = base2d["payload"]["candidates"][0]["error"]
        worker.send(**verify_begin_command("v5", "200", axis_mode="h_plus_w"))
        job = worker.read_event()["job_id"]
        for seq in range(2):
            worker.send(**verify_frame_command(f"v5f{seq}", job, seq, frame))
        worker.send(**{"protocol_version": 1, "type": "verify_end",
                       "request_id": "v5e", "job_id": job, "total": 2})
        terminal, results, _ = collect_verify(worker)
        check("verify-h-plus-w", terminal["type"] == "result"
              and abs(results.get(0, -1) - err_2d) <= 1e-12
              and abs(results.get(1, -1) - err_2d) <= 1e-12,
              json.dumps({"results": results, "expected": err_2d}))

        # Mixed success: one missing asset must warn, null its result, and
        # let the job complete.
        worker.send(**verify_begin_command("v6", "200"))
        job = worker.read_event()["job_id"]
        missing = os.path.join(scratch, "missing-verify.f32")
        for seq, path in enumerate([frame, missing, frame2]):
            worker.send(**verify_frame_command(f"v6f{seq}", job, seq, path))
        worker.send(**{"protocol_version": 1, "type": "verify_end",
                       "request_id": "v6e", "job_id": job, "total": 3})
        terminal, results, warnings = collect_verify(worker)
        check("verify-mixed-success", terminal["type"] == "result"
              and terminal["payload"]["frames_completed"] == 2
              and terminal["payload"]["frames_failed"] == 1
              and results.get(1) is None
              and any(w.get("seq") == 1 and w.get("code") == "frame_asset_error"
                      for w in warnings),
              json.dumps({"terminal": terminal.get("payload", {}),
                          "results": results, "warnings": warnings}))

        # Stream integrity: seq gaps, unknown jobs, geometry mismatch, and a
        # declared-total mismatch all surface errors without killing the worker.
        worker.send(**verify_begin_command("v7", "200"))
        job = worker.read_event()["job_id"]

        def next_error():
            # Command errors come from the reader thread; asynchronous job
            # progress events may interleave and are skipped here.
            for _ in range(20):
                event = worker.read_event(timeout=60.0)
                if event["type"] == "error":
                    return event
            raise AssertionError("no error event arrived")

        worker.send(**verify_frame_command("v7f1", job, 1, frame))
        event = next_error()
        check("verify-frame-gap", event["code"] == "bad_request", json.dumps(event))
        worker.send(**verify_frame_command("v7fx", "job-99", 0, frame))
        event = next_error()
        check("verify-frame-unknown-job", event["code"] == "bad_request", json.dumps(event))
        frame_small = os.path.join(scratch, "frame-small.f32")
        write_frame(frame_small, 160, 120)
        worker.send(**verify_frame_command("v7fg", job, 0, frame_small,
                                           width=160, height=120))
        event = next_error()
        check("verify-geometry-mismatch", event["code"] == "bad_request", json.dumps(event))
        # The stream is still usable after rejected items.
        worker.send(**verify_frame_command("v7f0", job, 0, frame))
        worker.send(**{"protocol_version": 1, "type": "verify_end",
                       "request_id": "v7e", "job_id": job, "total": 5})
        # The mismatch cancels the job; the verify_end command errors too.
        # Event order between the error and the cancellation is unspecified.
        terminal_events = []
        for _ in range(40):
            event = worker.read_event(timeout=60.0)
            if event["type"] == "cancelled" and event.get("job_id") == job:
                terminal_events.append(event)
            elif event["type"] == "error" and event.get("request_id") == "v7e":
                terminal_events.append(event)
            if len(terminal_events) == 2:
                break
        kinds = sorted(event["type"] for event in terminal_events)
        cancelled = next((event for event in terminal_events
                          if event["type"] == "cancelled"), {})
        check("verify-total-mismatch", kinds == ["cancelled", "error"]
              and cancelled.get("detail") == "verify_total_mismatch",
              json.dumps(terminal_events))
        worker.send(**{"protocol_version": 1, "type": "verify_end",
                       "request_id": "v7e2", "job_id": job, "total": 1})
        event = worker.read_event()
        check("verify-end-closed-stream", event["type"] == "error"
              and event["code"] == "bad_request", json.dumps(event))

        # Cooperative cancel mid-stream: results streamed so far stay
        # consistent with the cancelled payload's frame counters.
        worker.send(**verify_begin_command("v8", "200"))
        job = worker.read_event()["job_id"]
        for seq in range(70):
            worker.send(**verify_frame_command(f"v8f{seq}", job, seq,
                                               ring[seq % 2]))
        streamed = {}
        cancelled = None
        for _ in range(400):
            event = worker.read_event(timeout=60.0)
            if event["type"] == "progress":
                for entry in event.get("results", []):
                    streamed[entry["seq"]] = entry["error"]
                if len(streamed) >= 1 and cancelled is None:
                    worker.send(**{"protocol_version": 1, "type": "cancel",
                                   "request_id": "v8c", "job_id": job})
                    cancelled = "sent"
            elif event["type"] in ("cancelled", "result", "error"):
                cancelled = event
                break
        check("verify-cancel-partial", cancelled is not None
              and cancelled["type"] == "cancelled"
              and cancelled.get("partial")
              == (cancelled.get("payload", {}).get("frames_completed", 0)
                  + cancelled.get("payload", {}).get("frames_failed", 0) > 0)
              and len(streamed) == (cancelled.get("payload", {}).get("frames_completed", 0)
                                    + cancelled.get("payload", {}).get("frames_failed", 0)),
              json.dumps({"terminal": cancelled, "streamed": len(streamed)}))

        # CUDA verify is a documented v1.1 gap, not a silent fallback.
        worker.send(**verify_begin_command("v9", "200", backend="cuda"))
        event = worker.read_event()
        check("verify-cuda-unsupported", event["type"] == "error"
              and event["code"] == "unsupported", json.dumps(event))

        worker.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "v10"})
        while worker.read_event()["type"] != "shutdown":
            pass
        check("verify-session-exit", worker.wait_exit() == 0)

        # --- Session 5: cross-process plan store reuse (E4) -----------------
        store_dir = os.path.join(scratch, "plan-store")
        grid_candidates = [str(200 + i) for i in range(21)]

        worker_a = Worker(store_dir=store_dir)
        worker_a.send(**{"protocol_version": 1, "type": "hello", "request_id": "s5a"})
        worker_a.read_event()
        result_a = run_analyze(worker_a, analyze_command("s5b", frame, grid_candidates))
        telemetry_a = result_a["payload"]["telemetry"]
        check("store-cold-build", result_a["type"] == "result"
              and telemetry_a["plan_build_count"] == 21
              and telemetry_a["plan_store_hits"] == 0,
              json.dumps(telemetry_a))
        worker_a.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "s5z"})
        while worker_a.read_event()["type"] != "shutdown":
            pass
        worker_a.wait_exit()

        import glob
        packs = glob.glob(os.path.join(store_dir, "*.gnpk"))
        check("store-pack-written", len(packs) == 1, json.dumps(packs))

        worker_b = Worker(store_dir=store_dir)
        worker_b.send(**{"protocol_version": 1, "type": "hello", "request_id": "s5c"})
        worker_b.read_event()
        result_b = run_analyze(worker_b, analyze_command("s5d", frame, grid_candidates))
        telemetry_b = result_b["payload"]["telemetry"]
        errors_a = [c["error"] for c in result_a["payload"]["candidates"]]
        errors_b = [c["error"] for c in result_b["payload"]["candidates"]]
        check("store-cross-process-hit", result_b["type"] == "result"
              and telemetry_b["plan_build_count"] == 0
              and telemetry_b["plan_store_hits"] == 21
              and telemetry_b["plan_cache_hits"] == 21,
              json.dumps(telemetry_b))
        check("store-cross-process-parity", errors_a == errors_b,
              json.dumps({"a": errors_a[:3], "b": errors_b[:3]}))
        worker_b.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "s5y"})
        while worker_b.read_event()["type"] != "shutdown":
            pass
        worker_b.wait_exit()

        # --- Session 6: kernel mode (protocol v1.1) --------------------------
        worker = Worker()
        worker.send(**{"protocol_version": 1, "type": "hello", "request_id": "k0"})
        worker.read_event()

        kernels = [{"id": "bilinear"}, {"id": "bicubic", "b": 0.0, "c": 0.5},
                   {"id": "bicubic", "b": 0.0, "c": 1.0}, {"id": "lanczos", "taps": 3},
                   {"id": "spline64"}]
        kernel_result = run_analyze(worker, {
            "protocol_version": 1, "type": "analyze", "request_id": "k1",
            "mode": "kernel", "backend": "cpu",
            "frame_asset": {"path": frame, "format": "f32le", "width": 320, "height": 240},
            "axis_mode": "h_only", "candidate": "200", "kernels": kernels,
            "metric": {"p_norm": 1}})
        entries = kernel_result.get("payload", {}).get("candidates", [])
        check("kernel-flow", kernel_result["type"] == "result"
              and kernel_result["payload"]["mode"] == "kernel"
              and kernel_result["payload"].get("candidate") == "200"
              and len(entries) == 5
              and [entry["id"] for entry in entries] == ["0", "1", "2", "3", "4"]
              and entries[3]["kernel"] == {"id": "lanczos", "taps": 3}
              and entries[1]["kernel"] == {"id": "bicubic", "b": 0.0, "c": 0.5},
              json.dumps(entries)[:300])

        # Cross-mode parity: kernel-mode lanczos(3)@200 equals the same
        # height-mode candidate bit-for-bit, and the shared plan was a
        # session-cache hit on whichever job ran second.
        height_l3 = run_analyze(worker, analyze_command(
            "k2", frame, ["200"], kernel={"id": "lanczos", "taps": 3}))
        height_err = height_l3["payload"]["candidates"][0]["error"]
        kernel_l3 = entries[3]["error"] if entries else None
        check("kernel-height-parity", kernel_l3 == height_err,
              f"kernel={kernel_l3} height={height_err}")
        check("kernel-cross-mode-cache", height_l3["payload"]["telemetry"]["plan_cache_hits"] == 1,
              json.dumps(height_l3["payload"]["telemetry"]))

        # Duplicate kernel specs get distinct index ids but one shared plan.
        dup_result = run_analyze(worker, {
            "protocol_version": 1, "type": "analyze", "request_id": "k3",
            "mode": "kernel", "backend": "cpu",
            "frame_asset": {"path": frame, "format": "f32le", "width": 320, "height": 240},
            "axis_mode": "h_only", "candidate": "200",
            "kernels": [{"id": "lanczos", "taps": 3}, {"id": "lanczos", "taps": 3}],
            "metric": {"p_norm": 1}})
        dup = dup_result.get("payload", {}).get("candidates", [])
        check("kernel-duplicates", dup_result["type"] == "result"
              and len(dup) == 2 and dup[0]["id"] == "0" and dup[1]["id"] == "1"
              and dup[0]["error"] == dup[1]["error"] == height_err
              and dup_result["payload"]["telemetry"]["plan_build_count"] == 0,
              json.dumps(dup_result.get("payload", {}))[:300])

        # h_plus_w kernel mode: secondary plans derive per kernel and match
        # the height-mode two-axis path bit-for-bit.
        height_2d = run_analyze(worker, analyze_command(
            "k4", frame, ["200"], axis_mode="h_plus_w",
            kernel={"id": "bicubic", "b": 0.0, "c": 0.5}))
        kernel_2d = run_analyze(worker, {
            "protocol_version": 1, "type": "analyze", "request_id": "k5",
            "mode": "kernel", "backend": "cpu",
            "frame_asset": {"path": frame, "format": "f32le", "width": 320, "height": 240},
            "axis_mode": "h_plus_w", "candidate": "200",
            "kernels": [{"id": "bicubic", "b": 0.0, "c": 0.5}, {"id": "spline36"}],
            "metric": {"p_norm": 1}})
        k2d_entries = kernel_2d.get("payload", {}).get("candidates", [])
        check("kernel-h-plus-w", kernel_2d["type"] == "result"
              and len(k2d_entries) == 2
              and k2d_entries[0]["error"]
                  == height_2d["payload"]["candidates"][0]["error"],
              json.dumps(k2d_entries)[:300])

        # Wire-level guards.
        worker.send(**{"protocol_version": 1, "type": "analyze", "request_id": "k6",
                       "mode": "kernel", "backend": "cpu",
                       "frame_asset": {"path": frame, "format": "f32le",
                                       "width": 320, "height": 240},
                       "axis_mode": "h_only", "kernels": kernels,
                       "metric": {"p_norm": 1}})
        event = worker.read_event()
        check("kernel-missing-candidate", event["type"] == "error"
              and event["code"] == "bad_request", json.dumps(event))
        worker.send(**{"protocol_version": 1, "type": "analyze", "request_id": "k7",
                       "mode": "kernel", "backend": "cpu",
                       "frame_asset": {"path": frame, "format": "f32le",
                                       "width": 320, "height": 240},
                       "axis_mode": "h_only", "candidate": "200",
                       "kernel": {"id": "bicubic"}, "kernels": kernels,
                       "metric": {"p_norm": 1}})
        event = worker.read_event()
        check("kernel-both-fields", event["type"] == "error"
              and event["code"] == "bad_request", json.dumps(event))
        worker.send(**{"protocol_version": 1, "type": "analyze", "request_id": "k8",
                       "mode": "kernel", "backend": "cpu",
                       "frame_asset": {"path": frame, "format": "f32le",
                                       "width": 320, "height": 240},
                       "axis_mode": "h_only", "candidate": "200", "kernels": [],
                       "metric": {"p_norm": 1}})
        event = worker.read_event()
        check("kernel-empty-list", event["type"] == "error"
              and event["code"] == "bad_request", json.dumps(event))

        worker.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "k9"})
        while worker.read_event()["type"] != "shutdown":
            pass
        check("kernel-session-exit", worker.wait_exit() == 0)

    if FAILURES:
        print(f"{len(FAILURES)} worker protocol test(s) failed")
        return 1
    print("worker protocol tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

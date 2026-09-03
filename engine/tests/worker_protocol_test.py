#!/usr/bin/env python3
"""Integration tests for the getnative-engine worker protocol v1.

Drives a real worker process over pipes: handshake, capability gating,
height analysis with session plan-cache retention, cooperative cancel,
error paths, and shutdown semantics. See docs/worker-protocol-v1.md.
"""

import json
import mmap
import os
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

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
    def __init__(self, store_dir=None, engine=ENGINE, use_default_store=False,
                 xdg_cache_home=None, env_overrides=None):
        # stdout stays binary: events are framed with an internal byte buffer
        # (select + buffered readline loses events that arrive in one chunk).
        # Each worker gets an isolated plan-store dir: the persistent cache
        # would otherwise leak plans across sessions and break telemetry
        # expectations (and the user's real cache must never be touched).
        env = None
        if store_dir is not None:
            env = dict(os.environ)
            env["GETNATIVE_PLAN_CACHE_DIR"] = store_dir
        elif use_default_store:
            env = dict(os.environ)
            env.pop("GETNATIVE_PLAN_CACHE", None)
            env.pop("GETNATIVE_PLAN_CACHE_DIR", None)
            if xdg_cache_home is not None:
                env["XDG_CACHE_HOME"] = xdg_cache_home
        else:
            env = dict(os.environ)
            env["GETNATIVE_PLAN_CACHE"] = "off"
        # This protocol test explicitly covers the shared device-input cache;
        # production defaults remain benchmark-gated off until the warm guard
        # is met on every formal kernel.
        env["GETNATIVE_CUDA_INPUT_CACHE_BYTES"] = str(512 * 1024 * 1024)
        if env_overrides:
            env.update(env_overrides)
        self.process = subprocess.Popen(
            [engine, "worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        self._stdout_lines = queue.Queue()

        def read_stdout():
            try:
                for line in self.process.stdout:
                    self._stdout_lines.put(line)
            except BaseException as error:
                self._stdout_lines.put(error)
            finally:
                self._stdout_lines.put(None)

        self._stdout_thread = threading.Thread(target=read_stdout, daemon=True)
        self._stdout_thread.start()

    def send(self, **command):
        self.process.stdin.write(json.dumps(command).encode() + b"\n")
        self.process.stdin.flush()

    def send_raw(self, line):
        self.process.stdin.write(line.encode() + b"\n")
        self.process.stdin.flush()

    def read_event(self, timeout=30.0):
        try:
            line = self._stdout_lines.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError("worker did not emit an event in time") from error
        if line is None:
            raise EOFError("worker stdout closed unexpectedly")
        if isinstance(line, BaseException):
            raise line
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


def verify_media_command(request_id, media_path, backend, width=128, height=96,
                         scan_scope=None, concurrency=None):
    command = {
        "protocol_version": 1,
        "type": "verify_media_begin",
        "request_id": request_id,
        "geometry": {"width": width, "height": height},
        "axis_mode": "h_only",
        "kernel": {"id": "bicubic", "b": 0, "c": 0.5},
        "candidate": "64",
        "metric": {
            "p_norm": 1,
            "crop_left": 2,
            "crop_right": 2,
            "crop_top": 2,
            "crop_bottom": 2,
            "threshold": 0.015,
        },
        "backend": backend,
        "media": {"path": media_path, "fingerprint": "", "stream_index": 0},
        "scan_scope": scan_scope or {"selection": "all"},
    }
    if concurrency is not None:
        command["concurrency"] = concurrency
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
    _, terminal = run_analyze_with_accepted(worker, command)
    return terminal


def run_analyze_with_accepted(worker, command):
    worker.send(**command)
    accepted = None
    while True:
        event = worker.read_event()
        if event["type"] == "accepted":
            accepted = event
        if event["type"] in ("result", "error", "cancelled"):
            return accepted, event


def collect_verify(worker, timeout=60.0):
    """Collect (terminal, streamed_results, warnings) until a terminal event."""
    results = {}
    warnings = []
    last_progress_coverage = None
    while True:
        event = worker.read_event(timeout=timeout)
        if event["type"] == "progress":
            if "coverage" in event:
                last_progress_coverage = event["coverage"]
            for entry in event.get("results", []):
                if entry["seq"] in results:
                    raise AssertionError(f"duplicate verify seq {entry['seq']}")
                results[entry["seq"]] = entry["error"]
        elif event["type"] == "warning":
            warnings.append(event)
        elif event["type"] in ("result", "error", "cancelled"):
            if last_progress_coverage is not None:
                event = {**event, "last_progress_coverage": last_progress_coverage}
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
        cuda = next((b for b in payload.get("backends", []) if b.get("id") == "cuda"), {})
        cuda_available = (cuda.get("compiled") is True
                          and cuda.get("device_available") is True
                          and cuda.get("analysis_command_available") is True)
        vulkan = next((b for b in payload.get("backends", [])
                       if b.get("id") == "vulkan"), {})
        vulkan_available = (vulkan.get("compiled") is True
                            and vulkan.get("device_available") is True
                            and vulkan.get("analysis_command_available") is True)
        metal = next((b for b in payload.get("backends", [])
                      if b.get("id") == "metal"), {})
        metal_available = (metal.get("compiled") is True
                           and metal.get("device_available") is True
                           and metal.get("analysis_command_available") is True)
        backend_ids = {backend.get("id") for backend in payload.get("backends", [])}
        check("capabilities-analyze-gating",
              event["type"] == "capabilities"
              and payload.get("commands", {}).get("analyze") is True
              and cpu.get("analysis_command_available") is True)
        check("capabilities-four-backends-and-auto-metadata",
              backend_ids == {"cpu", "metal", "cuda", "vulkan"}
              and cpu.get("auto_priority") == 100
              and (not cuda_available or cuda.get("auto_priority") == 10)
              and (not metal_available or metal.get("auto_priority") == 30)
              and (not vulkan_available
                   or vulkan.get("device_type") in {
                       "discrete_gpu", "integrated_gpu", "virtual_gpu", "cpu", "other"
                   })
              and (vulkan.get("auto_priority") != 20
                   or vulkan.get("device_type") == "discrete_gpu"),
              json.dumps(payload.get("backends", []))[:1000])
        check("capability-feature-flags",
              payload.get("features", {}).get("verify_frame_ring") is True
              and payload.get("features", {}).get("media_frame_batch") is False,
              json.dumps(payload.get("features", {})))

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
        check("accepted-reports-cpu-backend",
              events[0].get("backend") == "cpu" and "device" not in events[0],
              json.dumps(events[0]))
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

        # The complete profile/grid/base contract reaches the worker, and CPU
        # accepts a valid p=2 norm. The grid is deliberately one candidate so
        # the generated decimal sequence is checked against the request.
        profiled = run_analyze(worker, analyze_command(
            "r3p", frame, ["204"], axis_mode="h_only", profile_id="muf-d278cd3",
            endpoint_rule="exclusive_stop", base_height="201", base_width="321",
            grid={"start": "204", "stop": "205", "step": "1"},
            metric={"crop_left": 5, "crop_right": 5, "crop_top": 5,
                    "crop_bottom": 5, "threshold": 0.015, "p_norm": 2}))
        check("profile-grid-base-and-cpu-p2",
              profiled["type"] == "result"
              and profiled["payload"]["candidates"][0]["id"] == "204",
              json.dumps(profiled)[:500])

        # Auto follows the advertised CUDA/Vulkan/Metal p-norm ranges.
        for norm in range(1, 5):
            auto_accepted, auto_norm = run_analyze_with_accepted(worker, analyze_command(
                f"r3auto-p{norm}", frame, ["204"], backend="auto",
                metric={"p_norm": norm}))
            expected_auto_backend = (
                "cuda" if cuda_available
                else "vulkan" if vulkan.get("auto_priority") == 20
                else "metal" if metal.get("auto_priority") == 30
                else "cpu"
            )
            telemetry = auto_norm.get("payload", {}).get("telemetry", {})
            expected_device = (
                telemetry.get("cuda_device") if expected_auto_backend == "cuda"
                else telemetry.get("vulkan_device") if expected_auto_backend == "vulkan"
                else telemetry.get("metal_device") if expected_auto_backend == "metal"
                else None
            )
            check(f"auto-p{norm}-backend",
                  auto_norm["type"] == "result"
                  and telemetry.get("backend") == expected_auto_backend
                  and auto_accepted is not None
                  and auto_accepted.get("backend") == expected_auto_backend
                  and auto_accepted.get("device") == expected_device,
                  json.dumps(auto_norm)[:500])

        auto_p5 = run_analyze(worker, analyze_command(
            "r3auto-p5", frame, ["204"], backend="auto",
            metric={"p_norm": 5}))
        check("auto-p5-falls-back-to-cpu",
              auto_p5["type"] == "result"
              and auto_p5["payload"]["telemetry"]["backend"] == "cpu",
              json.dumps(auto_p5)[:500])

        if cuda_available:
            for norm in (2, 4):
                explicit_accepted, explicit = run_analyze_with_accepted(worker, analyze_command(
                    f"r3cuda-p{norm}", frame, ["204"], backend="cuda",
                    metric={"p_norm": norm}))
                telemetry = explicit.get("payload", {}).get("telemetry", {})
                check(f"cuda-p{norm}-accepted",
                      explicit["type"] == "result"
                      and telemetry.get("backend") == "cuda"
                      and explicit_accepted is not None
                      and explicit_accepted.get("backend") == "cuda"
                      and explicit_accepted.get("device") == telemetry.get("cuda_device"),
                      json.dumps(explicit)[:500])

        for norm in (5, 4294967295):
            worker.send(**analyze_command(
                f"r3cuda-p{norm}", frame, ["204"], backend="cuda",
                metric={"p_norm": norm}))
            event = worker.read_event()
            check(f"cuda-p{norm}-rejected-at-submit",
                  event["type"] == "error" and event["code"] == "unsupported",
                  json.dumps(event))

        if vulkan_available:
            cpu_p1 = run_analyze(worker, analyze_command(
                "r3vulkan-cpu", frame, ["204"], backend="cpu",
                metric={"p_norm": 1}))
            vulkan_p1 = run_analyze(worker, analyze_command(
                "r3vulkan", frame, ["204"], backend="vulkan",
                metric={"p_norm": 1}))
            cpu_error = cpu_p1.get("payload", {}).get("candidates", [{}])[0].get("error")
            vulkan_error = vulkan_p1.get("payload", {}).get("candidates", [{}])[0].get("error")
            tolerance = max(2e-7, 5e-4 * abs(cpu_error or 0.0))
            check("vulkan-p1-explicit",
                  vulkan_p1["type"] == "result"
                  and vulkan_p1["payload"]["telemetry"]["backend"] == "vulkan"
                  and abs(vulkan_error - cpu_error) <= tolerance,
                  json.dumps(vulkan_p1)[:500])
            for norm in (2, 4):
                explicit_accepted, explicit = run_analyze_with_accepted(worker, analyze_command(
                    f"r3vulkan-p{norm}", frame, ["204"], backend="vulkan",
                    metric={"p_norm": norm}))
                telemetry = explicit.get("payload", {}).get("telemetry", {})
                check(f"vulkan-p{norm}-accepted",
                      explicit["type"] == "result"
                      and telemetry.get("backend") == "vulkan"
                      and explicit_accepted is not None
                      and explicit_accepted.get("backend") == "vulkan"
                      and explicit_accepted.get("device") == telemetry.get("vulkan_device"),
                      json.dumps(explicit)[:500])

        for norm in (5, 4294967295):
            worker.send(**analyze_command(
                f"r3vulkan-p{norm}", frame, ["204"], backend="vulkan",
                metric={"p_norm": norm}))
            event = worker.read_event()
            check(f"vulkan-p{norm}-rejected-at-submit",
                  event["type"] == "error" and event["code"] == "unsupported",
                  json.dumps(event))

        cpu_max = run_analyze(worker, analyze_command(
            "r3cpu-pmax", frame, ["204"], backend="cpu",
            metric={"p_norm": 4294967295}))
        check("cpu-accepts-uint32-max-p-norm", cpu_max["type"] == "result",
              json.dumps(cpu_max)[:500])

        # Fractional active length and parity shift are part of the plan key:
        # equal destination canvases with different fractions must not reuse a
        # plan or collapse to the same metric.
        fractional_a = run_analyze(worker, analyze_command(
            "r3frac-a", frame, ["200.1"], axis_mode="h_only",
            profile_id="muf-d278cd3", base_height="201"))
        fractional_b = run_analyze(worker, analyze_command(
            "r3frac-b", frame, ["200.5"], axis_mode="h_only",
            profile_id="muf-d278cd3", base_height="201"))
        check("fractional-geometry-shift-is-live",
              fractional_a["type"] == "result" and fractional_b["type"] == "result"
              and fractional_a["payload"]["candidates"][0]["error"]
                   != fractional_b["payload"]["candidates"][0]["error"],
              json.dumps({"a": fractional_a, "b": fractional_b})[:500])

        # Stable bad_request responses for malformed profile and geometry.
        worker.send(**analyze_command("r3bad-profile", frame, ["204"], profile_id="missing"))
        event = worker.read_event()
        check("unknown-profile-bad-request",
              event["type"] == "error" and event["code"] == "bad_request",
              json.dumps(event))
        worker.send(**analyze_command("r3bad-base", frame, ["204"], base_height="200.5"))
        event = worker.read_event()
        check("fractional-base-rejected-by-tauri-contract",
              event["type"] == "error" and event["code"] == "bad_request",
              json.dumps(event))
        worker.send(**analyze_command(
            "r3bad-grid", frame, ["204"], profile_id="muf-d278cd3",
            grid={"start": "204", "stop": "205"}))
        event = worker.read_event()
        check("malformed-grid-bad-request",
              event["type"] == "error" and event["code"] == "bad_request",
              json.dumps(event))
        worker.send(**analyze_command(
            "r3implicit-kernel", frame, ["204"], kernel={"id": "bicubic"}))
        event = worker.read_event()
        check("implicit-kernel-parameters-bad-request",
              event["type"] == "error" and event["code"] == "bad_request",
              json.dumps(event))

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
        capabilities = event["payload"]
        cuda_backend = next(
            (b for b in capabilities["backends"] if b.get("id") == "cuda"), {})
        cuda_usable = (cuda_backend.get("compiled") and cuda_backend.get("device_available"))
        vulkan_backend = next(
            (b for b in capabilities["backends"] if b.get("id") == "vulkan"), {})
        vulkan_usable = (vulkan_backend.get("compiled")
                         and vulkan_backend.get("device_available"))

        if not cuda_usable:
            print("SKIP cuda-parity (CUDA backend not available)")
            print("SKIP cuda-source-residency (CUDA backend not available)")
        else:
            # 40 candidates span two 32-wide chunks, exercising the CUDA
            # candidate pipeline (parallel chunk execution, ordered results).
            candidates = [str(190 + i) for i in range(40)]

            def run_job(request_id, backend):
                worker.send(**analyze_command(request_id, frame, candidates,
                                              backend=backend, axis_mode="h_plus_w"))
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
                  and first_telemetry.get("cuda_source_upload_count") == 1
                  and first_telemetry.get("cuda_source_transpose_count") == 1
                  and second_telemetry.get("cuda_source_upload_bytes", -1) == 0
                  and second_telemetry.get("cuda_source_cache_hits", 0) >= 1,
                  json.dumps({"first": first_telemetry.get("cuda_source_upload_bytes"),
                              "second": second_telemetry.get("cuda_source_upload_bytes")}))

        if not vulkan_usable:
            print("SKIP vulkan-worker-pipeline (Vulkan backend not available)")
        else:
            # Two 32-candidate chunks exercise the unchanged worker pipeline;
            # only the per-chunk compute engine differs from CUDA.
            candidates = [str(190 + i) for i in range(40)]
            cpu_result = run_analyze(worker, analyze_command(
                "c-vulkan-cpu", frame, candidates,
                backend="cpu", axis_mode="h_plus_w"))
            vulkan_accepted, vulkan_result = run_analyze_with_accepted(worker, analyze_command(
                "c-vulkan", frame, candidates,
                backend="vulkan", axis_mode="h_plus_w"))
            if cpu_result["type"] != "result" or vulkan_result["type"] != "result":
                check("vulkan-worker-pipeline", False,
                      json.dumps(vulkan_result)[:500])
            else:
                cpu_errors = [c["error"] for c in cpu_result["payload"]["candidates"]]
                vulkan_errors = [c["error"]
                                  for c in vulkan_result["payload"]["candidates"]]
                close = all(abs(c - g) <= max(2e-7, 5e-4 * abs(c))
                            for c, g in zip(cpu_errors, vulkan_errors))
                telemetry = vulkan_result["payload"]["telemetry"]
                check("vulkan-worker-pipeline",
                      close and telemetry.get("backend") == "vulkan"
                      and vulkan_accepted is not None
                      and vulkan_accepted.get("backend") == "vulkan"
                      and vulkan_accepted.get("device") == telemetry.get("vulkan_device")
                      and telemetry.get("vulkan_command_buffer_submissions", 0) == 2
                      and telemetry.get("vulkan_tiles", 0) == 2,
                      json.dumps({"telemetry": telemetry,
                                  "cpu": cpu_errors[:3],
                                  "vulkan": vulkan_errors[:3]})[:800])

        auto_accepted, auto_result = run_analyze_with_accepted(
            worker,
            analyze_command("c-auto", frame, ["200"], backend="auto"),
        )
        metal_backend = next((backend for backend in capabilities.get("backends", [])
                              if backend.get("id") == "metal"), {})
        expected_auto = ("cuda" if cuda_usable
                         else "vulkan" if vulkan_backend.get("auto_priority") == 20
                         else "metal" if metal_backend.get("auto_priority") == 30
                         else "cpu")
        telemetry = auto_result.get("payload", {}).get("telemetry", {})
        device = (telemetry.get("cuda_device") if expected_auto == "cuda"
                  else telemetry.get("vulkan_device") if expected_auto == "vulkan"
                  else telemetry.get("metal_device") if expected_auto == "metal"
                  else None)
        check(
            "auto-cuda-vulkan-cpu-priority",
            auto_result["type"] == "result"
            and telemetry.get("backend") == expected_auto
            and auto_accepted is not None
            and auto_accepted.get("backend") == expected_auto
            and auto_accepted.get("device") == device,
            json.dumps(auto_result)[:500],
        )

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
              and event["commands"].get("verify_ring_attach") is True
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
              and accepted.get("backend") == "cpu"
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

        # File-asset producers wait for result progress before freeing files.
        # The progress batch must therefore never exceed suggested_in_flight.
        worker.send(**verify_begin_command(
            "vfb", "200", expected_frames=4, worker_count=1))
        accepted = worker.read_event()
        fallback_job = accepted["job_id"]
        check("verify-fallback-in-flight-hint",
              accepted.get("suggested_in_flight") == 2, json.dumps(accepted))
        for seq, path in enumerate([frame, frame2]):
            worker.send(**verify_frame_command(
                f"vfbf{seq}", fallback_job, seq, path))
        first_batch = None
        for _ in range(20):
            event = worker.read_event(timeout=60.0)
            if event.get("type") == "progress" and event.get("results"):
                first_batch = event["results"]
                break
        check("verify-fallback-progress-before-full-stream",
              first_batch is not None and len(first_batch) == 2,
              json.dumps(first_batch))
        for seq, path in enumerate([frame, frame2], start=2):
            worker.send(**verify_frame_command(
                f"vfbf{seq}", fallback_job, seq, path))
        worker.send(**{
            "protocol_version": 1, "type": "verify_end",
            "request_id": "vfbe", "job_id": fallback_job, "total": 4,
        })
        terminal, remaining, warnings = collect_verify(worker)
        check("verify-fallback-bounded-completes",
              terminal["type"] == "result" and len(remaining) == 2
              and not warnings,
              json.dumps({"terminal": terminal, "remaining": remaining})[:500])

        # Shared-memory ring: two slots wrap three times. Slot reuse is legal
        # only after verify_consumed and generations must increase.
        ring_path = os.path.join(scratch, "verify-ring.f32")
        frame_bytes = 320 * 240 * 4
        with open(ring_path, "w+b") as ring_file:
            ring_file.truncate(frame_bytes * 2)
            with mmap.mmap(ring_file.fileno(), frame_bytes * 2) as mapped:
                worker.send(**verify_begin_command("vr0", "200", expected_frames=6))
                ring_job = worker.read_event()["job_id"]
                worker.send(**{
                    "protocol_version": 1, "type": "verify_ring_attach",
                    "request_id": "vr-attach", "job_id": ring_job,
                    "path": ring_path, "slot_count": 2, "frame_bytes": frame_bytes,
                })
                ring_results = {}

                def wait_consumed(expected_seq):
                    for _ in range(50):
                        received = worker.read_event(timeout=60.0)
                        if received["type"] == "progress":
                            for row in received.get("results", []):
                                ring_results[row["seq"]] = row["error"]
                        if (received["type"] == "verify_consumed"
                                and received["seq"] == expected_seq):
                            return received
                    raise AssertionError("verify_consumed did not arrive")

                def next_ring_error():
                    for _ in range(20):
                        received = worker.read_event(timeout=60.0)
                        if received["type"] == "error":
                            return received
                    raise AssertionError("ring validation error did not arrive")

                for seq, path in enumerate(ring):
                    slot = seq % 2
                    generation = seq // 2 + 1
                    if seq == 2:
                        worker.send(**{
                            "protocol_version": 1, "type": "verify_frame",
                            "request_id": "vr-stale", "job_id": ring_job,
                            "seq": seq, "slot": slot, "generation": 1,
                        })
                        error = next_ring_error()
                        check("verify-ring-stale-generation",
                              error.get("code") == "bad_request", json.dumps(error))
                    if seq == 3:
                        worker.send(**{
                            "protocol_version": 1, "type": "verify_frame",
                            "request_id": "vr-order", "job_id": ring_job,
                            "seq": seq + 1, "slot": slot, "generation": generation,
                        })
                        error = next_ring_error()
                        check("verify-ring-out-of-order",
                              error.get("code") == "bad_request", json.dumps(error))
                    if seq == 4:
                        worker.send(**{
                            "protocol_version": 1, "type": "verify_frame",
                            "request_id": "vr-slot", "job_id": ring_job,
                            "seq": seq, "slot": 2, "generation": generation,
                        })
                        error = next_ring_error()
                        check("verify-ring-slot-range",
                              error.get("code") == "bad_request", json.dumps(error))
                    with open(path, "rb") as source:
                        mapped[slot * frame_bytes:(slot + 1) * frame_bytes] = source.read()
                    mapped.flush()
                    worker.send(**{
                        "protocol_version": 1, "type": "verify_frame",
                        "request_id": f"vrf{seq}", "job_id": ring_job,
                        "seq": seq, "slot": slot, "generation": generation,
                    })
                    consumed = wait_consumed(seq)
                    check(f"verify-ring-consumed-{seq}",
                          consumed.get("slot") == slot
                          and consumed.get("generation") == generation,
                          json.dumps(consumed))

                worker.send(**{
                    "protocol_version": 1, "type": "verify_end",
                    "request_id": "vre", "job_id": ring_job, "total": 6,
                })
                terminal, trailing, warnings = collect_verify(worker)
                ring_results.update(trailing)
                check("verify-ring-flow", terminal["type"] == "result"
                      and len(ring_results) == 6 and not warnings,
                      json.dumps({"terminal": terminal, "results": ring_results})[:500])
                check("verify-ring-parity",
                      all(abs(ring_results[seq]
                              - (err_frame if seq % 2 == 0 else err_frame2)) <= 1e-12
                          for seq in range(6)),
                      json.dumps(ring_results))

        # The engine accepts a one-slot ring even though the frontend defaults
        # to at least two slots. This exercises the strictest wrap/backpressure
        # boundary directly at the protocol layer.
        single_ring_path = os.path.join(scratch, "verify-ring-single.f32")
        with open(single_ring_path, "w+b") as ring_file:
            ring_file.truncate(frame_bytes)
            with mmap.mmap(ring_file.fileno(), frame_bytes) as mapped:
                worker.send(**verify_begin_command("vr1", "200", expected_frames=2))
                single_job = worker.read_event()["job_id"]
                worker.send(**{
                    "protocol_version": 1, "type": "verify_ring_attach",
                    "request_id": "vr1-attach", "job_id": single_job,
                    "path": single_ring_path, "slot_count": 1,
                    "frame_bytes": frame_bytes,
                })
                ring_results = {}
                for seq, path in enumerate([frame, frame2]):
                    with open(path, "rb") as source:
                        mapped[:] = source.read()
                    mapped.flush()
                    worker.send(**{
                        "protocol_version": 1, "type": "verify_frame",
                        "request_id": f"vr1f{seq}", "job_id": single_job,
                        "seq": seq, "slot": 0, "generation": seq + 1,
                    })
                    consumed = wait_consumed(seq)
                    check(f"verify-ring-single-consumed-{seq}",
                          consumed.get("slot") == 0
                          and consumed.get("generation") == seq + 1,
                          json.dumps(consumed))
                worker.send(**{
                    "protocol_version": 1, "type": "verify_end",
                    "request_id": "vr1e", "job_id": single_job, "total": 2,
                })
                terminal, trailing, warnings = collect_verify(worker)
                ring_results.update(trailing)
                check("verify-ring-single-slot",
                      terminal["type"] == "result" and len(ring_results) == 2
                      and not warnings,
                      json.dumps({"terminal": terminal, "results": ring_results})[:500])

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

        # Accelerator verify is a documented v1.1 gap, not a silent fallback.
        worker.send(**verify_begin_command("v9", "200", backend="cuda"))
        event = worker.read_event()
        check("verify-cuda-unsupported", event["type"] == "error"
              and event["code"] == "unsupported", json.dumps(event))
        worker.send(**verify_begin_command("v9-vulkan", "200", backend="vulkan"))
        event = worker.read_event()
        check("verify-vulkan-unsupported", event["type"] == "error"
              and event["code"] == "unsupported", json.dumps(event))

        worker.send(**{"protocol_version": 1, "type": "shutdown", "request_id": "v10"})
        while worker.read_event()["type"] != "shutdown":
            pass
        check("verify-session-exit", worker.wait_exit() == 0)

        # --- Session 4.5: in-engine media decode and optional zero-copy -----
        ffmpeg = shutil.which("ffmpeg")
        media_decode = capabilities.get("features", {}).get("verify_engine_decode")
        if ffmpeg is None or not media_decode:
            print("SKIP verify-media (FFmpeg CLI or engine media decode unavailable)")
        else:
            media_path = os.path.join(scratch, "verify-media-h264.mp4")
            encoded = subprocess.run([
                ffmpeg, "-y", "-v", "error",
                "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=6",
                "-frames:v", "30", "-c:v", "libx264", "-g", "6", "-bf", "2",
                "-pix_fmt", "yuv420p", media_path,
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            if not encoded:
                print("SKIP verify-media (FFmpeg H.264 encoder unavailable)")
            else:
                worker = Worker()
                worker.send(**{
                    "protocol_version": 1, "type": "hello",
                    "request_id": "vm-hello",
                })
                hello = worker.read_event()
                check("verify-media-advertised",
                      hello.get("commands", {}).get("verify_media_begin") is True,
                      json.dumps(hello))
                check("verify-media-concurrency-advertised",
                      hello.get("media_verify_concurrency")
                      == {"min": 1, "max": 16, "default": 8, "gpu_max": 8},
                      json.dumps(hello))

                for index, invalid in enumerate((0, 17, -1, 1.5, "two")):
                    worker.send(**verify_media_command(
                        f"vm-invalid-{index}", media_path, "cpu",
                        concurrency=invalid))
                    rejected = worker.read_event()
                    check(f"verify-media-concurrency-reject-{index}",
                          rejected["type"] == "error"
                          and rejected["code"] == "bad_request",
                          json.dumps(rejected))

                metal_capability = next(
                    (backend for backend in capabilities.get("backends", [])
                     if backend.get("id") == "metal"), {})
                if (metal_capability.get("compiled") is True
                        and metal_capability.get("device_available") is True):
                    worker.send(**verify_media_command(
                        "vm-metal-parse", media_path, "metal",
                        scan_scope={"selection": "all", "start_frame": 0,
                                    "end_frame": 0}, concurrency=1))
                    metal_first = worker.read_event()
                    check("verify-media-metal-parser",
                          not (metal_first.get("type") == "error"
                               and metal_first.get("message") == "unknown backend: metal"),
                          json.dumps(metal_first))
                    if metal_first.get("type") == "accepted":
                        collect_verify(worker)

                worker.send(**verify_media_command(
                    "vm-cpu", media_path, "cpu"))
                cpu_accepted = worker.read_event()
                cpu_terminal, cpu_results, cpu_warnings = collect_verify(worker)
                cpu_provenance = cpu_terminal.get("payload", {}).get("provenance", {})
                cpu_telemetry = cpu_terminal.get("payload", {}).get("telemetry", {})
                cpu_coverage = cpu_terminal.get("coverage", {})
                check("verify-media-software",
                      cpu_terminal["type"] == "result"
                      and len(cpu_results) == 30 and not cpu_warnings
                      and set(cpu_results) == set(range(30))
                      and cpu_provenance.get("decoder") == "software"
                      and cpu_accepted.get("concurrency") == 8
                      and cpu_telemetry.get("requested_concurrency") == 8
                      and cpu_telemetry.get("effective_concurrency") == 8
                      and cpu_coverage == {
                          "selection": "all", "eligible_frames": 30,
                          "selected_frames": 30, "processed_frames": 30,
                          "failed_frames": 0,
                      }
                      and cpu_terminal.get("last_progress_coverage") == cpu_coverage
                      and 1 <= cpu_telemetry.get("max_inflight", 0) <= 8,
                      json.dumps({"terminal": cpu_terminal,
                                  "warnings": cpu_warnings})[:800])

                for concurrency in (1, 8, 16):
                    worker.send(**verify_media_command(
                        f"vm-cpu-c{concurrency}", media_path, "cpu",
                        concurrency=concurrency))
                    accepted = worker.read_event()
                    terminal, results, warnings = collect_verify(worker)
                    telemetry = terminal.get("payload", {}).get("telemetry", {})
                    close = (results.keys() == cpu_results.keys()
                             and all(abs(results[seq] - cpu_results[seq]) <= 2e-6
                                     for seq in results))
                    check(f"verify-media-concurrency-{concurrency}",
                          accepted.get("concurrency") == concurrency
                          and terminal["type"] == "result" and close and not warnings
                          and telemetry.get("requested_concurrency") == concurrency
                          and telemetry.get("effective_concurrency") == concurrency
                          and 1 <= telemetry.get("max_inflight", 0) <= concurrency,
                           json.dumps({"accepted": accepted,
                                       "terminal": terminal})[:1000])

                worker.send(**verify_media_command(
                    "vm-cpu-i-pictures", media_path, "cpu",
                    concurrency=4,
                    scan_scope={"selection": "decoded_i_picture"}))
                i_accepted = worker.read_event()
                i_terminal, i_results, i_warnings = collect_verify(worker)
                i_telemetry = i_terminal.get("payload", {}).get("telemetry", {})
                i_coverage = i_terminal.get("coverage", {})
                check("verify-media-i-picture-indexed",
                      i_accepted.get("concurrency") == 4
                      and i_terminal["type"] == "result"
                      and len(i_results) == 5
                      and i_coverage == {
                          "selection": "decoded_i_picture", "eligible_frames": 30,
                          "selected_frames": 5, "processed_frames": 5,
                          "failed_frames": 0,
                      }
                      and not i_warnings
                      and i_telemetry.get("decoded_frames", 1000) <= 30,
                      json.dumps({"accepted": i_accepted,
                                  "terminal": i_terminal,
                                  "warnings": i_warnings})[:1000])

                every_n_scope = {
                    "selection": "every_n", "every_n": 4,
                    "start_frame": 3, "end_frame": 20,
                }
                worker.send(**verify_media_command(
                    "vm-cpu-every-n", media_path, "cpu",
                    scan_scope=every_n_scope))
                every_n_terminal, every_n_results, every_n_warnings = \
                    collect_verify(worker)
                check("verify-media-every-n-inclusive-coverage",
                      every_n_terminal["type"] == "result"
                      and len(every_n_results) == 5
                      and not every_n_warnings
                      and every_n_terminal.get("coverage") == {
                          "selection": "every_n", "eligible_frames": 18,
                          "selected_frames": 5, "processed_frames": 5,
                          "failed_frames": 0,
                      },
                      json.dumps({"terminal": every_n_terminal,
                                  "warnings": every_n_warnings})[:1000])

                cancel_path = os.path.join(scratch, "verify-media-cancel.mp4")
                cancel_encoded = subprocess.run([
                    ffmpeg, "-y", "-v", "error",
                    "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24",
                    "-frames:v", "600", "-c:v", "libx264", "-g", "24",
                    "-bf", "2", "-pix_fmt", "yuv420p", cancel_path,
                ], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL).returncode == 0
                if cancel_encoded:
                    worker.send(**verify_media_command(
                        "vm-cpu-cancel", cancel_path, "cpu", concurrency=1))
                    cancel_job_id = None
                    cancel_sent = False
                    cancel_results = {}
                    cancel_terminal = None
                    while cancel_terminal is None:
                        event = worker.read_event()
                        if event["type"] == "accepted":
                            cancel_job_id = event["job_id"]
                        elif event["type"] == "progress":
                            for entry in event.get("results", []):
                                cancel_results[entry["seq"]] = entry["error"]
                            if event.get("coverage") and not cancel_sent:
                                worker.send(
                                    protocol_version=1, type="cancel",
                                    request_id="vm-cpu-cancel-request",
                                    job_id=cancel_job_id)
                                cancel_sent = True
                        elif event["type"] in ("result", "error", "cancelled"):
                            cancel_terminal = event
                    cancel_coverage = cancel_terminal.get("coverage", {})
                    check("verify-media-cancelled-coverage",
                          cancel_sent
                          and cancel_terminal["type"] == "cancelled"
                          and cancel_coverage.get("selection") == "all"
                          and cancel_coverage.get("eligible_frames") == 600
                          and cancel_coverage.get("selected_frames") == 600
                          and cancel_coverage.get("processed_frames")
                              == len(cancel_results)
                          and cancel_coverage.get("failed_frames") == 0,
                          json.dumps({"terminal": cancel_terminal,
                                      "results": len(cancel_results)})[:1000])
                else:
                    print("SKIP verify-media-cancelled-coverage "
                          "(FFmpeg H.264 encoder unavailable)")

                decode_backends = {
                    row.get("id"): row
                    for row in capabilities.get("decode_backends", [])
                }
                accelerators = []
                if (cuda_usable
                        and decode_backends.get("nvdec", {}).get("runtime_device")):
                    accelerators.append(("cuda", "nvdec"))
                if (vulkan_usable
                        and decode_backends.get("vulkan_video", {}).get("runtime_device")):
                    accelerators.append(("vulkan", "vulkan_video"))

                late_scope = {
                    "selection": "all", "start_frame": 24, "end_frame": 29,
                }
                worker.send(**verify_media_command(
                    "vm-cpu-late", media_path, "cpu", scan_scope=late_scope))
                late_cpu_terminal, late_cpu_results, late_cpu_warnings = \
                    collect_verify(worker)
                late_cpu_telemetry = late_cpu_terminal.get(
                    "payload", {}).get("telemetry", {})
                check("verify-media-software-indexed-seek",
                      late_cpu_terminal["type"] == "result"
                      and len(late_cpu_results) == 6 and not late_cpu_warnings
                      and late_cpu_terminal.get("coverage") == {
                          "selection": "all", "eligible_frames": 6,
                          "selected_frames": 6, "processed_frames": 6,
                          "failed_frames": 0,
                      }
                      and late_cpu_telemetry.get("decoded_frames", 1000) <= 12,
                      json.dumps({"terminal": late_cpu_terminal,
                                  "warnings": late_cpu_warnings})[:1000])

                for backend, decoder in accelerators:
                    worker.send(**verify_media_command(
                        f"vm-{backend}", media_path, backend))
                    terminal, results, warnings = collect_verify(worker)
                    payload = terminal.get("payload", {})
                    provenance = payload.get("provenance", {})
                    telemetry = payload.get("telemetry", {})
                    close = (results.keys() == cpu_results.keys()
                             and all(abs(results[seq] - cpu_results[seq]) <= 2e-6
                                     for seq in results))
                    check(f"verify-media-{decoder}-zero-copy",
                          terminal["type"] == "result" and close and not warnings
                          and provenance.get("decoder") == decoder
                          and provenance.get("zero_copy") is True
                          and telemetry.get("host_frame_bytes") == 0
                          and telemetry.get("source_upload_bytes") == 0,
                          json.dumps({"provenance": provenance,
                                      "telemetry": telemetry,
                                      "warnings": warnings})[:1000])

                    worker.send(**verify_media_command(
                        f"vm-{backend}-late", media_path, backend,
                        scan_scope=late_scope))
                    late_terminal, late_results, late_warnings = collect_verify(worker)
                    late_payload = late_terminal.get("payload", {})
                    late_provenance = late_payload.get("provenance", {})
                    late_telemetry = late_payload.get("telemetry", {})
                    late_close = (
                        late_results.keys() == late_cpu_results.keys()
                        and all(abs(late_results[seq] - late_cpu_results[seq]) <= 2e-6
                                for seq in late_results)
                    )
                    check(f"verify-media-{decoder}-indexed-seek",
                          late_terminal["type"] == "result" and late_close
                          and not late_warnings
                          and late_provenance.get("decoder") == decoder
                          and late_telemetry.get("decoded_frames", 1000) <= 12,
                          json.dumps({"provenance": late_provenance,
                                      "telemetry": late_telemetry,
                                      "warnings": late_warnings})[:1000])

                    worker.send(**verify_media_command(
                        f"vm-{backend}-i-pictures", media_path, backend,
                        concurrency=4,
                        scan_scope={"selection": "decoded_i_picture"}))
                    accelerator_i_terminal, accelerator_i_results, \
                        accelerator_i_warnings = collect_verify(worker)
                    accelerator_i_payload = accelerator_i_terminal.get("payload", {})
                    accelerator_i_provenance = accelerator_i_payload.get(
                        "provenance", {})
                    accelerator_i_telemetry = accelerator_i_payload.get(
                        "telemetry", {})
                    accelerator_i_close = (
                        accelerator_i_results.keys() == i_results.keys()
                        and all(abs(accelerator_i_results[seq] - i_results[seq]) <= 2e-6
                                for seq in i_results)
                    )
                    check(f"verify-media-{decoder}-i-picture-indexed",
                          accelerator_i_terminal["type"] == "result"
                          and accelerator_i_close and not accelerator_i_warnings
                          and accelerator_i_terminal.get("coverage") == i_coverage
                          and accelerator_i_provenance.get("decoder") == decoder
                          and accelerator_i_provenance.get("zero_copy") is True
                          and accelerator_i_telemetry.get("decoded_frames", 1000) <= 20
                          and accelerator_i_telemetry.get("requested_concurrency") == 4
                          and accelerator_i_telemetry.get("effective_concurrency") == 4,
                          json.dumps({"terminal": accelerator_i_terminal,
                                      "warnings": accelerator_i_warnings})[:1000])

                hevc_accelerators = [
                    (backend, decoder)
                    for backend, decoder in accelerators
                    if "hevc" in decode_backends.get(decoder, {}).get("codecs", [])
                ]
                if hevc_accelerators:
                    hevc_path = os.path.join(scratch, "verify-media-hevc-main10.mkv")
                    hevc_encoded = subprocess.run([
                        ffmpeg, "-y", "-v", "error",
                        "-f", "lavfi", "-i", "testsrc2=size=192x160:rate=4",
                        "-frames:v", "4", "-c:v", "libx265", "-bf", "2",
                        "-pix_fmt", "yuv420p10le",
                        "-x265-params", "log-level=error:pools=1:frame-threads=1",
                        hevc_path,
                    ], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL).returncode == 0
                    if not hevc_encoded:
                        print("SKIP verify-media-p010 "
                              "(FFmpeg HEVC Main 10 encoder unavailable)")
                    else:
                        worker.send(**verify_media_command(
                            "vm-p010-cpu", hevc_path, "cpu", 192, 160))
                        p010_cpu_terminal, p010_cpu_results, p010_cpu_warnings = \
                            collect_verify(worker)
                        p010_cpu_provenance = p010_cpu_terminal.get(
                            "payload", {}).get("provenance", {})
                        check("verify-media-p010-software",
                              p010_cpu_terminal["type"] == "result"
                              and len(p010_cpu_results) == 4
                              and not p010_cpu_warnings
                              and p010_cpu_provenance.get("decoder") == "software"
                              and p010_cpu_provenance.get("bit_depth") == 10,
                              json.dumps({"terminal": p010_cpu_terminal,
                                          "warnings": p010_cpu_warnings})[:1000])

                        for backend, decoder in hevc_accelerators:
                            worker.send(**verify_media_command(
                                f"vm-p010-{backend}", hevc_path, backend,
                                192, 160))
                            terminal, results, warnings = collect_verify(worker)
                            payload = terminal.get("payload", {})
                            provenance = payload.get("provenance", {})
                            telemetry = payload.get("telemetry", {})
                            close = (
                                results.keys() == p010_cpu_results.keys()
                                and all(abs(results[seq] - p010_cpu_results[seq])
                                        <= 2e-6 for seq in results)
                            )
                            check(f"verify-media-p010-{decoder}-zero-copy",
                                  terminal["type"] == "result" and close
                                  and not warnings
                                  and provenance.get("decoder") == decoder
                                  and provenance.get("bit_depth") == 10
                                  and provenance.get("zero_copy") is True
                                  and telemetry.get("host_frame_bytes") == 0
                                  and telemetry.get("source_upload_bytes") == 0,
                                  json.dumps({"provenance": provenance,
                                              "telemetry": telemetry,
                                              "warnings": warnings})[:1000])

                worker.send(**{
                    "protocol_version": 1, "type": "shutdown",
                    "request_id": "vm-shutdown",
                })
                while worker.read_event()["type"] != "shutdown":
                    pass
                check("verify-media-session-exit", worker.wait_exit() == 0)

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

        # Linux follows XDG; other platforms retain the portable
        # executable-directory default.
        portable_dir = os.path.join(scratch, "portable-engine")
        os.makedirs(portable_dir)
        portable_engine = os.path.join(portable_dir, os.path.basename(ENGINE))
        shutil.copy2(ENGINE, portable_engine)
        default_cache_root = os.path.join(scratch, "xdg-cache")
        portable_worker = Worker(
            engine=portable_engine,
            use_default_store=True,
            xdg_cache_home=default_cache_root,
        )
        portable_worker.send(**{
            "protocol_version": 1, "type": "hello", "request_id": "s5e"})
        portable_worker.read_event()
        portable_result = run_analyze(
            portable_worker, analyze_command("s5f", frame, grid_candidates))
        portable_worker.send(**{
            "protocol_version": 1, "type": "shutdown", "request_id": "s5g"})
        while portable_worker.read_event()["type"] != "shutdown":
            pass
        portable_worker.wait_exit()
        if sys.platform.startswith("linux"):
            expected_store = os.path.join(
                default_cache_root, "io.getnative.vf", "axis-plans")
            default_name = "store-defaults-to-xdg-cache-directory"
        else:
            expected_store = portable_dir
            default_name = "store-defaults-to-executable-directory"
        portable_packs = glob.glob(os.path.join(expected_store, "*.gnpk"))
        check(default_name,
              portable_result["type"] == "result" and len(portable_packs) == 1,
              json.dumps(portable_packs))

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

#!/usr/bin/env python3
"""Reproducible FFmpeg-to-resident-worker pipeline probe.

The default matrix is the production performance gate: 1920x1080, 301
candidates, bilinear/bicubic/lanczos8, CPU 1/8/16/auto, available CUDA, and
ring verification at 1,000 and 10,000 frames. Use --quick for a bounded smoke
run. One JSON document is written to stdout; diagnostics go to stderr.
"""

import argparse
import collections
import hashlib
import json
import mmap
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


KERNELS = [
    {"id": "bilinear"},
    {"id": "bicubic", "b": 0.0, "c": 0.5},
    {"id": "lanczos", "taps": 8},
]


def process_memory(pid):
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as source:
            values = {}
            for line in source:
                if line.startswith(("VmRSS:", "VmHWM:")):
                    name, value, _ = line.split()
                    values[name[:-1]] = int(value) * 1024
            return values
    except OSError:
        return {}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", help="path to getnative-engine")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    parser.add_argument("--fixture", help="existing encoded video fixture")
    parser.add_argument("--output", help="also write the JSON report here")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--verify-frames", default="1000,10000")
    parser.add_argument("--worker-counts", default="1,8,16,0")
    parser.add_argument("--verify-transport", choices=("ring", "file", "both"),
                        default="both")
    parser.add_argument("--verify-file-limit", type=int, default=1000)
    parser.add_argument("--backend", choices=("all", "cpu", "cuda"), default="all")
    parser.add_argument("--p-norms", default="1,2,3,4",
                        help="comma-separated CUDA p-norm matrix")
    parser.add_argument("--skip-height", action="store_true")
    parser.add_argument("--skip-kernel", action="store_true")
    parser.add_argument("--skip-verify", action="store_true")
    parser.add_argument("--skip-media-batch", action="store_true")
    args = parser.parse_args()
    if not args.ffmpeg:
        parser.error("ffmpeg was not found; pass --ffmpeg")
    if args.samples < 1:
        parser.error("--samples must be positive")
    return args


def kernel_name(kernel):
    if kernel["id"] == "lanczos":
        return f"lanczos{kernel['taps']}"
    return kernel["id"]


def create_fixture(ffmpeg, path, width, height, frames):
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i",
        f"testsrc2=size={width}x{height}:rate=24000/1001",
        "-frames:v", str(frames), "-c:v", "ffv1", "-level", "3", path,
    ]
    subprocess.run(command, check=True)


def read_exact_into(stream, target):
    offset = 0
    while offset < len(target):
        count = stream.readinto(target[offset:])
        if not count:
            raise EOFError(f"FFmpeg ended after {offset} of {len(target)} bytes")
        offset += count


def decode_first_frame(ffmpeg, fixture, path, width, height):
    started = time.monotonic()
    with open(path, "wb") as output:
        subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-i", fixture,
            "-map", "0:v:0", "-frames:v", "1", "-f", "rawvideo",
            "-pix_fmt", "grayf32le", "-",
        ], stdout=output, check=True)
    expected = width * height * 4
    if os.path.getsize(path) != expected:
        raise RuntimeError("decoded frame size does not match fixture geometry")
    return (time.monotonic() - started) * 1000.0


def decode_target_frames(ffmpeg, fixture, output_path, indices, batched,
                         width, height):
    started = time.monotonic()
    with open(output_path, "wb") as output:
        groups = [indices] if batched else [[index] for index in indices]
        for group in groups:
            expression = "select='0" + "".join(
                f"+eq(n\\,{index})" for index in group) + "'"
            subprocess.run([
                ffmpeg, "-hide_banner", "-loglevel", "error", "-i", fixture,
                "-map", "0:v:0", "-vf", expression, "-fps_mode", "passthrough",
                "-f", "rawvideo", "-pix_fmt", "grayf32le", "-",
            ], stdout=output, check=True)
    expected = len(indices) * width * height * 4
    actual = os.path.getsize(output_path)
    if actual != expected:
        raise RuntimeError(f"media export wrote {actual} bytes, expected {expected}")
    digest = hashlib.sha256()
    with open(output_path, "rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return {
        "wall_ms": (time.monotonic() - started) * 1000.0,
        "ffmpeg_process_count": 1 if batched else len(indices),
        "output_bytes": actual,
        "checksum_sha256": digest.hexdigest(),
    }


def run_media_batch_probe(ffmpeg, fixture, scratch, width, height, samples,
                          fixture_frames):
    indices = [round(index * (fixture_frames - 1) / 7) for index in range(8)]
    serial = []
    batched = []
    for sample in range(samples):
        order = (False, True) if sample % 2 == 0 else (True, False)
        for use_batch in order:
            output_path = os.path.join(
                scratch, f"media-{'batch' if use_batch else 'serial'}-{sample}.f32le")
            report = decode_target_frames(
                ffmpeg, fixture, output_path, indices, use_batch, width, height)
            report["sample"] = sample
            (batched if use_batch else serial).append(report)
            os.remove(output_path)
    serial_median = statistics.median(row["wall_ms"] for row in serial)
    batch_median = statistics.median(row["wall_ms"] for row in batched)
    return {
        "frame_count": len(indices),
        "frame_indices": indices,
        "single_frame_path": "legacy_per_frame_export",
        "serial": serial,
        "batched": batched,
        "serial_median_ms": serial_median,
        "batched_median_ms": batch_median,
        "wall_improvement_percent": (
            (serial_median - batch_median) * 100.0 / serial_median),
        "checksum_match": (
            {row["checksum_sha256"] for row in serial}
            == {row["checksum_sha256"] for row in batched}),
    }


class Worker:
    def __init__(self, engine, query_capabilities=True, extra_environment=None):
        environment = dict(os.environ)
        environment["GETNATIVE_PLAN_CACHE"] = "off"
        environment.update(extra_environment or {})
        self.process = subprocess.Popen(
            [engine, "worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=environment, bufsize=0,
        )
        self.buffer = b""
        self.command_count = 0
        self.event_count = 0
        self.send({"protocol_version": 1, "type": "hello", "request_id": "hello"})
        hello = self.read_event()
        if hello.get("type") != "hello_ok":
            raise RuntimeError(f"worker handshake failed: {hello}")
        self.capability_ms = 0.0
        self.capabilities = {}
        if query_capabilities:
            capability_started = time.monotonic()
            self.send({"protocol_version": 1, "type": "capabilities", "request_id": "caps"})
            capabilities = self.read_event()
            self.capability_ms = (time.monotonic() - capability_started) * 1000.0
            self.capabilities = capabilities.get("payload", {})

    def send(self, command):
        self.process.stdin.write(json.dumps(command, separators=(",", ":")).encode() + b"\n")
        self.process.stdin.flush()
        self.command_count += 1

    def read_event(self):
        while b"\n" not in self.buffer:
            chunk = os.read(self.process.stdout.fileno(), 1 << 20)
            if not chunk:
                detail = self.process.stderr.read().decode(errors="replace")
                raise EOFError(f"worker stdout closed: {detail[-2000:]}")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        self.event_count += 1
        return json.loads(line)

    def rss_bytes(self):
        return process_memory(self.process.pid)

    def close(self):
        if self.process.poll() is not None:
            return
        self.send({"protocol_version": 1, "type": "shutdown", "request_id": "shutdown"})
        while self.read_event().get("type") != "shutdown":
            pass
        self.process.wait(timeout=30)


def checksum(value):
    material = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
    return hashlib.sha256(material.encode()).hexdigest()


def collect_job(worker, request_id, command):
    commands_before = worker.command_count
    events_before = worker.event_count
    started = time.monotonic()
    worker.send(command)
    accepted = None
    progress_events = 0
    terminal = None
    while terminal is None:
        event = worker.read_event()
        if event.get("request_id") != request_id and event.get("type") != "progress":
            continue
        if event.get("type") == "accepted":
            accepted = event
        elif event.get("type") == "progress":
            progress_events += 1
        elif event.get("type") in ("result", "error", "cancelled"):
            terminal = event
    wall_ms = (time.monotonic() - started) * 1000.0
    if terminal.get("type") != "result":
        raise RuntimeError(f"job failed: {json.dumps(terminal)[:1000]}")
    payload = terminal["payload"]
    telemetry = payload.get("telemetry", {})
    values = [entry.get("error") for entry in payload.get("candidates", [])]
    ranked = sorted(
        enumerate(payload.get("candidates", [])),
        key=lambda item: (item[1].get("error"), item[0]))
    return {
        "accepted_job_id": accepted.get("job_id") if accepted else None,
        "wall_ms": wall_ms,
        "capability_ms": worker.capability_ms,
        "capability_plus_job_wall_ms": worker.capability_ms + wall_ms,
        "rss": worker.rss_bytes(),
        "producer_rss": process_memory(os.getpid()),
        "command_count": worker.command_count - commands_before,
        "event_count": worker.event_count - events_before,
        "progress_event_count": progress_events,
        "checksum_sha256": checksum(values),
        "candidate_ranking": [entry.get("id") for _, entry in ranked],
        "telemetry": telemetry,
    }


def analyze_command(request_id, mode, frame, width, height, backend, workers,
                    kernel=None, candidates=None, p_norm=1):
    command = {
        "protocol_version": 1, "type": "analyze", "request_id": request_id,
        "mode": mode, "backend": backend,
        "frame_asset": {
            "path": frame, "format": "f32le", "width": width, "height": height,
        },
        "axis_mode": "h_only", "metric": {"p_norm": p_norm},
        "worker_count": workers,
    }
    if mode == "height":
        command.update({"kernel": kernel, "candidates": candidates})
    else:
        command.update({"candidate": str(int(height * 0.75)), "kernels": KERNELS})
    return command


def run_analyze_pair(engine, mode, frame, width, height, backend, workers,
                     kernel, candidates, samples, sample_offset=0,
                     extra_environment=None, p_norm=1):
    results = []
    for sample in range(sample_offset, sample_offset + samples):
        worker = Worker(engine, extra_environment=extra_environment)
        try:
            for phase in ("cold", "warm"):
                request_id = (
                    f"{mode}-{backend}-{workers}-{kernel_name(kernel)}-"
                    f"{sample}-{phase}")
                report = collect_job(worker, request_id, analyze_command(
                    request_id, mode, frame, width, height, backend, workers,
                    kernel=kernel, candidates=candidates, p_norm=p_norm))
                telemetry = report.pop("telemetry")
                results.append({
                    "mode": mode,
                    "phase": phase,
                    "sample": sample,
                    "backend": backend,
                    "resolved_backend": telemetry.get("backend", backend),
                    "p_norm": p_norm,
                    "kernel": kernel_name(kernel) if mode == "height" else "kernel-matrix",
                    "candidate_count": len(candidates) if mode == "height" else len(KERNELS),
                    "requested_worker_count": workers,
                    **report,
                    "timing_ms": {
                        "frontend_queue": 0.0,
                        "media_decode": 0.0,
                        "asset_wait": telemetry.get("asset_wait_ms", 0.0),
                        "worker_queue": telemetry.get("worker_queue_ms", 0.0),
                        "plan": telemetry.get("plan_ms", 0.0),
                        "candidate": telemetry.get("candidate_ms", telemetry.get("candidates_ms", 0.0)),
                        "cuda_kernel": telemetry.get("cuda_kernel_ms", 0.0),
                        "cuda_metric": telemetry.get("cuda_metric_ms", 0.0),
                        "readback": telemetry.get("cuda_result_readback_ms", 0.0),
                        "job_total": telemetry.get("job_total_ms", telemetry.get("total_ms", 0.0)),
                    },
                    "cache": {
                        "plan_hits": telemetry.get("plan_cache_hits", 0),
                        "plan_misses": telemetry.get("plan_cache_misses", 0),
                        "asset_hits": telemetry.get("asset_cache_hits", 0),
                        "asset_misses": telemetry.get("asset_cache_misses", 0),
                        "cuda_source_hits": telemetry.get("cuda_source_cache_hits", 0),
                        "cuda_source_misses": telemetry.get("cuda_source_cache_misses", 0),
                        "cuda_host_plan_hits": telemetry.get("cuda_host_plan_cache_hits", 0),
                        "cuda_host_plan_misses": telemetry.get("cuda_host_plan_cache_misses", 0),
                        "cuda_host_plan_bytes": telemetry.get("cuda_host_plan_cache_bytes", 0),
                    },
                    "upload_bytes": {
                        "source": telemetry.get("cuda_source_upload_bytes", 0),
                        "plan": telemetry.get("cuda_plan_upload_bytes", 0),
                    },
                })
        finally:
            worker.close()
    return results


def run_cuda_initialization_probe(engine, frame, width, height, candidates,
                                  samples):
    current = []
    legacy = []

    def current_once(sample):
        worker = Worker(engine)
        try:
            request_id = f"cuda-init-current-{sample}"
            report = collect_job(worker, request_id, analyze_command(
                request_id, "height", frame, width, height, "cuda", 0,
                kernel=KERNELS[0], candidates=candidates))
            return {
                "sample": sample,
                "capability_ms": report["capability_ms"],
                "first_job_ms": report["wall_ms"],
                "combined_ms": report["capability_plus_job_wall_ms"],
                "checksum_sha256": report["checksum_sha256"],
            }
        finally:
            worker.close()

    def legacy_once(sample):
        capability_started = time.monotonic()
        subprocess.run(
            [engine, "capabilities"], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=True)
        capability_ms = (time.monotonic() - capability_started) * 1000.0
        worker = Worker(engine, query_capabilities=False)
        try:
            request_id = f"cuda-init-legacy-{sample}"
            report = collect_job(worker, request_id, analyze_command(
                request_id, "height", frame, width, height, "cuda", 0,
                kernel=KERNELS[0], candidates=candidates))
            return {
                "sample": sample,
                "capability_ms": capability_ms,
                "first_job_ms": report["wall_ms"],
                "combined_ms": capability_ms + report["wall_ms"],
                "checksum_sha256": report["checksum_sha256"],
            }
        finally:
            worker.close()

    for sample in range(samples):
        if sample % 2 == 0:
            current.append(current_once(sample))
            legacy.append(legacy_once(sample))
        else:
            legacy.append(legacy_once(sample))
            current.append(current_once(sample))
    current_median = statistics.median(row["combined_ms"] for row in current)
    legacy_median = statistics.median(row["combined_ms"] for row in legacy)
    return {
        "current_retained": current,
        "legacy_temporary_probe": legacy,
        "current_median_ms": current_median,
        "legacy_median_ms": legacy_median,
        "wall_improvement_percent": (
            (legacy_median - current_median) * 100.0 / legacy_median),
        "checksum_match": (
            {row["checksum_sha256"] for row in current}
            == {row["checksum_sha256"] for row in legacy}),
    }


def run_cuda_input_cache_probe(engine, frame, width, height, candidates,
                               samples):
    rows = {"on": [], "off": []}
    environments = {
        "on": {"GETNATIVE_CUDA_INPUT_CACHE_BYTES": str(512 * 1024 * 1024)},
        "off": {"GETNATIVE_CUDA_INPUT_CACHE_BYTES": "0"},
    }
    for kernel in KERNELS:
        for sample in range(samples):
            order = ("on", "off") if sample % 2 == 0 else ("off", "on")
            for mode in order:
                rows[mode].extend(run_analyze_pair(
                    engine, "height", frame, width, height, "cuda", 0,
                    kernel, candidates, 1, sample_offset=sample,
                    extra_environment=environments[mode]))
    comparisons = []
    for kernel in KERNELS:
        name = kernel_name(kernel)
        for phase in ("cold", "warm"):
            enabled = [row for row in rows["on"]
                       if row["kernel"] == name and row["phase"] == phase]
            disabled = [row for row in rows["off"]
                        if row["kernel"] == name and row["phase"] == phase]
            on_wall = statistics.median(row["wall_ms"] for row in enabled)
            off_wall = statistics.median(row["wall_ms"] for row in disabled)
            comparisons.append({
                "kernel": name, "phase": phase,
                "enabled_median_ms": on_wall,
                "disabled_median_ms": off_wall,
                "enabled_delta_percent": (on_wall - off_wall) * 100.0 / off_wall,
                "enabled_source_upload_bytes": statistics.median(
                    row["upload_bytes"]["source"] for row in enabled),
                "disabled_source_upload_bytes": statistics.median(
                    row["upload_bytes"]["source"] for row in disabled),
                "checksum_match": (
                    {row["checksum_sha256"] for row in enabled}
                    == {row["checksum_sha256"] for row in disabled}),
            })
    return {"comparisons": comparisons}


def consume_verify_event(event, free_slots, generations, errors):
    if event.get("type") == "verify_consumed":
        slot = event["slot"]
        if event.get("generation") == generations[slot]:
            free_slots.append(slot)
    elif event.get("type") == "progress":
        for entry in event.get("results", []):
            errors[entry["seq"]] = entry.get("error")


def run_verify_once(worker, ffmpeg, fixture, width, height, frame_count,
                    slot_count, sample):
    request_id = f"verify-{frame_count}-{sample}"
    commands_before = worker.command_count
    events_before = worker.event_count
    worker.send({
        "protocol_version": 1, "type": "verify_begin", "request_id": request_id,
        "geometry": {"width": width, "height": height}, "axis_mode": "h_only",
        "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
        "candidate": str(int(height * 0.75)),
        "metric": {"p_norm": 1}, "backend": "cpu", "worker_count": 0,
        "expected_frames": frame_count,
    })
    accepted = worker.read_event()
    if accepted.get("type") != "accepted":
        raise RuntimeError(f"verify was not accepted: {accepted}")
    job_id = accepted["job_id"]
    slots = max(1, min(64, slot_count or accepted.get("suggested_in_flight", 8)))
    frame_bytes = width * height * 4
    errors = {}
    ring_wait_ms = 0.0
    terminal = None
    started = time.monotonic()

    with tempfile.TemporaryDirectory(prefix="getnative-probe-ring-") as scratch:
        ring_path = os.path.join(scratch, "frames.ring")
        with open(ring_path, "w+b") as ring_file:
            ring_file.truncate(slots * frame_bytes)
            ring = mmap.mmap(ring_file.fileno(), slots * frame_bytes, access=mmap.ACCESS_WRITE)
            worker.send({
                "protocol_version": 1, "type": "verify_ring_attach",
                "request_id": f"{request_id}-attach", "job_id": job_id,
                "path": ring_path, "slot_count": slots, "frame_bytes": frame_bytes,
            })
            decoder = subprocess.Popen([
                ffmpeg, "-hide_banner", "-loglevel", "error", "-stream_loop", "-1",
                "-i", fixture, "-map", "0:v:0", "-frames:v", str(frame_count),
                "-f", "rawvideo", "-pix_fmt", "grayf32le", "-",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            decode_started = time.monotonic()
            free_slots = collections.deque(range(slots))
            generations = [0] * slots
            try:
                for seq in range(frame_count):
                    wait_started = time.monotonic()
                    while not free_slots:
                        event = worker.read_event()
                        consume_verify_event(event, free_slots, generations, errors)
                        if event.get("type") in ("result", "error", "cancelled"):
                            terminal = event
                            break
                    ring_wait_ms += (time.monotonic() - wait_started) * 1000.0
                    if terminal:
                        break
                    slot = free_slots.popleft()
                    generations[slot] += 1
                    view = memoryview(ring)[slot * frame_bytes:(slot + 1) * frame_bytes]
                    read_exact_into(decoder.stdout, view)
                    view.release()
                    worker.send({
                        "protocol_version": 1, "type": "verify_frame",
                        "request_id": f"{request_id}-frame-{seq}", "job_id": job_id,
                        "seq": seq, "slot": slot, "generation": generations[slot],
                    })
                decode_ms = (time.monotonic() - decode_started) * 1000.0
                if terminal is None:
                    worker.send({
                        "protocol_version": 1, "type": "verify_end",
                        "request_id": f"{request_id}-end", "job_id": job_id,
                        "total": frame_count,
                    })
                while terminal is None:
                    event = worker.read_event()
                    consume_verify_event(event, free_slots, generations, errors)
                    if event.get("type") in ("result", "error", "cancelled"):
                        terminal = event
            finally:
                decoder.kill()
                decoder.wait()
                ring.close()

    wall_ms = (time.monotonic() - started) * 1000.0
    if terminal.get("type") != "result":
        raise RuntimeError(f"verify failed: {json.dumps(terminal)[:1000]}")
    payload = terminal["payload"]
    telemetry = payload.get("telemetry", {})
    ordered_errors = [errors.get(index) for index in range(frame_count)]
    return {
        "mode": "verify", "transport": "ring",
        "phase": "cold" if sample == 0 else "warm",
        "sample": sample, "backend": "cpu", "kernel": "bicubic",
        "frame_count": frame_count, "slot_count": slots,
        "effective_worker_count": accepted.get("worker_count"),
        "wall_ms": wall_ms, "wall_fps": frame_count * 1000.0 / wall_ms,
        "rss": worker.rss_bytes(),
        "producer_rss": process_memory(os.getpid()),
        "command_count": worker.command_count - commands_before,
        "producer_control_command_count": 3,
        "frontend_ipc_command_count_estimate": 3,
        "engine_frame_event_count": frame_count,
        "event_count": worker.event_count - events_before,
        "checksum_sha256": checksum(ordered_errors),
        "timing_ms": {
            "frontend_queue": 0.0, "media_decode": decode_ms,
            "asset_wait": telemetry.get("asset_wait_ms", 0.0),
            "worker_queue": telemetry.get("worker_queue_ms", 0.0),
            "plan": telemetry.get("plan_ms", 0.0),
            "candidate": telemetry.get("candidate_ms", telemetry.get("frame_analyze_ms", 0.0)),
            "readback": 0.0,
            "ring_wait": ring_wait_ms,
            "job_total": telemetry.get("job_total_ms", telemetry.get("stream_ms", 0.0)),
        },
        "cache": {
            "plan_hits": telemetry.get("plan_cache_hits", 0),
            "plan_misses": telemetry.get("plan_cache_misses", 0),
        },
        "upload_bytes": {"source": 0, "plan": 0},
    }


def run_verify_file_once(worker, ffmpeg, fixture, width, height, frame_count,
                         sample):
    request_id = f"verify-file-{frame_count}-{sample}"
    commands_before = worker.command_count
    events_before = worker.event_count
    worker.send({
        "protocol_version": 1, "type": "verify_begin", "request_id": request_id,
        "geometry": {"width": width, "height": height}, "axis_mode": "h_only",
        "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
        "candidate": str(int(height * 0.75)),
        "metric": {"p_norm": 1}, "backend": "cpu", "worker_count": 0,
        "expected_frames": frame_count,
    })
    accepted = worker.read_event()
    if accepted.get("type") != "accepted":
        raise RuntimeError(f"file verify was not accepted: {accepted}")
    job_id = accepted["job_id"]
    in_flight = max(1, min(64, accepted.get("suggested_in_flight", 8)))
    frame_bytes = width * height * 4
    errors = {}
    pending = {}
    wait_ms = 0.0
    terminal = None
    started = time.monotonic()

    with tempfile.TemporaryDirectory(prefix="getnative-probe-assets-") as scratch:
        decoder = subprocess.Popen([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-stream_loop", "-1",
            "-i", fixture, "-map", "0:v:0", "-frames:v", str(frame_count),
            "-f", "rawvideo", "-pix_fmt", "grayf32le", "-",
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        decode_started = time.monotonic()
        pixels = bytearray(frame_bytes)

        def consume(event):
            nonlocal terminal
            if event.get("type") == "progress":
                for row in event.get("results", []):
                    seq = row["seq"]
                    errors[seq] = row.get("error")
                    path = pending.pop(seq, None)
                    if path:
                        try:
                            os.remove(path)
                        except FileNotFoundError:
                            pass
            elif event.get("type") in ("result", "error", "cancelled"):
                terminal = event

        try:
            for seq in range(frame_count):
                wait_started = time.monotonic()
                while len(pending) >= in_flight:
                    consume(worker.read_event())
                    if terminal:
                        break
                wait_ms += (time.monotonic() - wait_started) * 1000.0
                if terminal:
                    break
                pixel_view = memoryview(pixels)
                read_exact_into(decoder.stdout, pixel_view)
                pixel_view.release()
                temporary = os.path.join(scratch, f"frame-{seq}.tmp")
                asset = os.path.join(scratch, f"frame-{seq}.f32le")
                with open(temporary, "wb") as output:
                    output.write(pixels)
                os.replace(temporary, asset)
                pending[seq] = asset
                worker.send({
                    "protocol_version": 1, "type": "verify_frame",
                    "request_id": f"{request_id}-frame-{seq}", "job_id": job_id,
                    "seq": seq,
                    "frame_asset": {
                        "path": asset, "format": "f32le",
                        "width": width, "height": height,
                    },
                })
            decode_ms = (time.monotonic() - decode_started) * 1000.0
            if terminal is None:
                worker.send({
                    "protocol_version": 1, "type": "verify_end",
                    "request_id": f"{request_id}-end", "job_id": job_id,
                    "total": frame_count,
                })
            while terminal is None:
                consume(worker.read_event())
        finally:
            decoder.kill()
            decoder.wait()

    wall_ms = (time.monotonic() - started) * 1000.0
    if terminal.get("type") != "result":
        raise RuntimeError(f"file verify failed: {json.dumps(terminal)[:1000]}")
    payload = terminal["payload"]
    telemetry = payload.get("telemetry", {})
    ordered_errors = [errors.get(index) for index in range(frame_count)]
    return {
        "mode": "verify", "transport": "frame_asset",
        "phase": "cold" if sample == 0 else "warm", "sample": sample,
        "backend": "cpu", "kernel": "bicubic", "frame_count": frame_count,
        "slot_count": in_flight,
        "effective_worker_count": accepted.get("worker_count"),
        "wall_ms": wall_ms, "wall_fps": frame_count * 1000.0 / wall_ms,
        "rss": worker.rss_bytes(),
        "producer_rss": process_memory(os.getpid()),
        "command_count": worker.command_count - commands_before,
        "producer_control_command_count": 2,
        "frontend_ipc_command_count_estimate": frame_count * 2 + 4,
        "engine_frame_event_count": frame_count,
        "event_count": worker.event_count - events_before,
        "checksum_sha256": checksum(ordered_errors),
        "timing_ms": {
            "frontend_queue": 0.0, "media_decode": decode_ms,
            "asset_wait": telemetry.get("asset_wait_ms", 0.0),
            "worker_queue": telemetry.get("worker_queue_ms", 0.0),
            "plan": telemetry.get("plan_ms", 0.0),
            "candidate": telemetry.get("candidate_ms", telemetry.get("frame_analyze_ms", 0.0)),
            "readback": 0.0, "ring_wait": wait_ms,
            "job_total": telemetry.get("job_total_ms", telemetry.get("stream_ms", 0.0)),
        },
        "cache": {
            "plan_hits": telemetry.get("plan_cache_hits", 0),
            "plan_misses": telemetry.get("plan_cache_misses", 0),
        },
        "upload_bytes": {"source": 0, "plan": 0},
    }


def summarize_verify(runs, width, height):
    verify = [row for row in runs if row.get("mode") == "verify"]
    comparisons = []
    for frame_count in sorted({row["frame_count"] for row in verify}):
        ring = [row for row in verify
                if row["frame_count"] == frame_count and row["transport"] == "ring"]
        files = [row for row in verify
                 if row["frame_count"] == frame_count
                 and row["transport"] == "frame_asset"]
        if not ring or not files:
            continue
        ring_wall = statistics.median(row["wall_ms"] for row in ring)
        file_wall = statistics.median(row["wall_ms"] for row in files)
        comparisons.append({
            "frame_count": frame_count,
            "ring_median_ms": ring_wall,
            "frame_asset_median_ms": file_wall,
            "ring_fps_improvement_percent": (file_wall - ring_wall) * 100.0 / ring_wall,
            "checksum_match": (
                {row["checksum_sha256"] for row in ring}
                == {row["checksum_sha256"] for row in files}),
            "ring_frontend_ipc_commands": ring[0]["frontend_ipc_command_count_estimate"],
            "frame_asset_frontend_ipc_commands": files[0]["frontend_ipc_command_count_estimate"],
        })
    ring_runs = [row for row in verify if row["transport"] == "ring"]
    stability = []
    for row in ring_runs:
        theoretical = row["slot_count"] * width * height * 4
        engine_peak = row.get("rss", {}).get("VmHWM")
        producer_peak = row.get("producer_rss", {}).get("VmHWM")
        stability.append({
            "frame_count": row["frame_count"], "sample": row["sample"],
            "rss_bytes": row.get("rss", {}).get("VmRSS"),
            "engine_peak_rss_bytes": engine_peak,
            "producer_peak_rss_bytes": producer_peak,
            "ring_theoretical_bytes": theoretical,
            "producer_peak_to_ring_ratio": (
                producer_peak / theoretical if producer_peak else None),
        })
    return {"comparisons": comparisons, "ring_stability": stability}


def main():
    args = parse_args()
    width, height = (320, 240) if args.quick else (1920, 1080)
    samples = 1 if args.quick else args.samples
    verify_counts = [8] if args.quick else [int(value) for value in args.verify_frames.split(",")]
    worker_counts = [0] if args.quick else [
        int(value) for value in args.worker_counts.split(",")]
    if not worker_counts or any(value < 0 for value in worker_counts):
        raise ValueError("--worker-counts must contain non-negative integers")
    p_norms = [int(value) for value in args.p_norms.split(",")]
    if not p_norms or any(value < 1 or value > 4 for value in p_norms):
        raise ValueError("--p-norms must contain integers in 1..4")
    candidate_start = 160.0 if args.quick else 760.0
    candidates = [f"{candidate_start + index * 0.25:.2f}" for index in range(301)]
    if args.quick:
        candidates = candidates[:7]

    with tempfile.TemporaryDirectory(prefix="getnative-pipeline-probe-") as scratch:
        fixture = args.fixture or os.path.join(scratch, "fixture.mkv")
        fixture_frames = 12 if args.quick else 48
        if not args.fixture:
            create_fixture(args.ffmpeg, fixture, width, height, fixture_frames)
        frame = os.path.join(scratch, "frame.f32le")
        decode_ms = decode_first_frame(args.ffmpeg, fixture, frame, width, height)
        fixture_hash = hashlib.sha256(open(fixture, "rb").read()).hexdigest()
        runs = []
        media_batch = None
        if not args.skip_media_batch:
            media_batch = run_media_batch_probe(
                args.ffmpeg, fixture, scratch, width, height, samples, fixture_frames)

        if not args.skip_height and args.backend != "cuda":
            for kernel in KERNELS:
                for sample in range(samples):
                    offset = sample % len(worker_counts)
                    ordered_workers = worker_counts[offset:] + worker_counts[:offset]
                    for workers in ordered_workers:
                        runs.extend(run_analyze_pair(
                            args.engine, "height", frame, width, height, "cpu", workers,
                            kernel, candidates, 1, sample_offset=sample))

        if not args.skip_kernel and args.backend != "cuda":
            for sample in range(samples):
                offset = sample % len(worker_counts)
                ordered_workers = worker_counts[offset:] + worker_counts[:offset]
                for workers in ordered_workers:
                    runs.extend(run_analyze_pair(
                        args.engine, "kernel", frame, width, height, "cpu", workers,
                        KERNELS[0], candidates, 1, sample_offset=sample))

        capability_worker = Worker(args.engine)
        cuda_available = any(
            backend.get("id") == "cuda" and backend.get("device_available")
            for backend in capability_worker.capabilities.get("backends", []))
        capability_worker.close()
        cuda_initialization = None
        cuda_input_cache = None
        if (cuda_available and not args.quick and not args.skip_height
                and args.backend != "cpu"):
            cuda_initialization = run_cuda_initialization_probe(
                args.engine, frame, width, height, candidates, samples)
            cuda_input_cache = run_cuda_input_cache_probe(
                args.engine, frame, width, height, candidates, samples)
        if cuda_available and not args.quick and not args.skip_height and args.backend != "cpu":
            for p_norm in p_norms:
                for kernel in KERNELS:
                    runs.extend(run_analyze_pair(
                        args.engine, "height", frame, width, height, "cuda", 0,
                        kernel, candidates, samples, p_norm=p_norm))

        if not args.skip_verify:
            worker = Worker(args.engine)
            try:
                for frame_count in verify_counts:
                    for sample in range(samples):
                        if args.verify_transport in ("ring", "both"):
                            runs.append(run_verify_once(
                                worker, args.ffmpeg, fixture, width, height,
                                frame_count, 0, sample))
                        if (args.verify_transport in ("file", "both")
                                and frame_count <= args.verify_file_limit):
                            runs.append(run_verify_file_once(
                                worker, args.ffmpeg, fixture, width, height,
                                frame_count, sample))
            finally:
                worker.close()

        report = {
            "schema_version": 1,
            "probe": "getnative_pipeline_probe",
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "host": {
                "platform": platform.platform(), "processor": platform.processor(),
                "logical_cpus": os.cpu_count(),
            },
            "fixture": {
                "path": os.path.abspath(fixture), "sha256": fixture_hash,
                "width": width, "height": height, "first_frame_decode_ms": decode_ms,
                "first_frame_sha256": hashlib.sha256(open(frame, "rb").read()).hexdigest(),
            },
            "matrix": {
                "samples": samples, "candidate_count": len(candidates),
                "worker_counts": worker_counts, "verify_frame_counts": verify_counts,
                "p_norms": p_norms,
                "verify_transport": args.verify_transport,
                "verify_file_limit": args.verify_file_limit,
                "cuda_available": cuda_available, "quick": args.quick,
            },
            "runs": runs,
            "media_batch": media_batch,
            "cuda_initialization": cuda_initialization,
            "cuda_input_cache": cuda_input_cache,
            "verify_summary": summarize_verify(runs, width, height),
        }
        encoded = json.dumps(report, indent=2, sort_keys=True)
        print(encoded)
        if args.output:
            with open(args.output, "w", encoding="utf-8") as output:
                output.write(encoded + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

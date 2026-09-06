#!/usr/bin/env python3
"""Bounded real-media Verify benchmark; retains provenance and per-frame results.

Run identical arguments against baseline and candidate binaries. The first job
warms the index/engine; subsequent jobs are measurements. GPU telemetry fields
may be sums across workers and must not be added to infer job wall time.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('media', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--backend', choices=['cuda', 'vulkan', 'metal', 'cpu'], default='vulkan')
    parser.add_argument('--frames', type=int, default=5001)
    parser.add_argument('--start-frame', type=int, default=0)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--concurrency', type=int, default=8)
    parser.add_argument('--axes', choices=['h_only', 'w_only', 'h_plus_w'], default='h_plus_w')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--candidate', default='864')
    parser.add_argument('--kernel', default='{"id":"bilinear"}', help='Kernel JSON')
    parser.add_argument('--norm', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    if args.frames < 1 or args.repeats < 1 or args.start_frame < 0:
        parser.error('frames and repeats must be positive; start-frame must be nonnegative')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = dict(protocol_version=1, type='verify_media_begin',
                   geometry=dict(width=args.width, height=args.height),
                   axis_mode=args.axes, candidate=args.candidate,
                   kernel=json.loads(args.kernel),
                   metric=dict(p_norm=args.norm, crop_left=5, crop_right=5,
                               crop_top=5, crop_bottom=5, threshold=0.015),
                   backend=args.backend, concurrency=args.concurrency,
                   media=dict(path=str(args.media.resolve()), fingerprint='', stream_index=0,
                              cache_directory=str(args.media.parent / 'verify-cache')),
                   scan_scope=dict(selection='all', start_frame=args.start_frame,
                                   end_frame=args.start_frame+args.frames-1))
    report = dict(command=command, engine=str(args.engine.resolve()),
                  stage_profile=os.environ.get('GETNATIVE_GPU_STAGE_PROFILE', ''),
                  jobs=[])
    with args.output.with_suffix('.stderr.log').open('w') as errors:
        process = subprocess.Popen([str(args.engine.resolve()), 'worker'],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors, text=True)
        events = queue.Queue()

        def reader():
            for line in process.stdout:
                events.put(line)
            events.put(None)

        threading.Thread(target=reader, daemon=True).start()

        def send(payload):
            process.stdin.write(json.dumps(payload)+'\n')
            process.stdin.flush()

        def read(deadline):
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('worker job deadline exceeded')
            line = events.get(timeout=remaining)
            if line is None:
                raise RuntimeError('worker closed stdout; see stderr log')
            try:
                return json.loads(line)
            except json.JSONDecodeError as error:
                args.output.with_suffix('.protocol-error.log').write_text(line)
                raise RuntimeError(f'Non-protocol worker output: {line[:500]}') from error

        try:
            send(dict(protocol_version=1, type='hello', request_id='hello'))
            report['hello'] = read(time.monotonic()+args.timeout)
            for repeat in range(args.repeats+1):
                started = time.monotonic()
                request = dict(command, request_id=f'benchmark-{repeat}')
                send(request)
                progress, warnings = [], []
                while True:
                    event = read(started+args.timeout)
                    if event.get('request_id') != request['request_id']:
                        continue
                    if event['type'] == 'progress':
                        progress.append(dict(at_ms=(time.monotonic()-started)*1000,
                                             completed=event.get('completed'),
                                             detail=event.get('detail')))
                    if event['type'] == 'warning':
                        warnings.append(event)
                    if event['type'] in ('result', 'error', 'cancelled'):
                        break
                job = dict(warmup=repeat == 0, wall_ms=(time.monotonic()-started)*1000,
                           terminal=event, progress=progress, warnings=warnings)
                report['jobs'].append(job)
                args.output.write_text(json.dumps(report, ensure_ascii=False)+'\n')
                if event['type'] != 'result':
                    raise RuntimeError(json.dumps(event))
                payload = event['payload']
                frames = payload['frames']
                if (payload['frames_completed'] != args.frames
                        or payload['frames_failed'] != 0
                        or len(frames) != args.frames
                        or sorted(f['frame_index'] for f in frames)
                            != list(range(args.start_frame, args.start_frame + args.frames))):
                    raise RuntimeError('Incomplete or duplicate frame coverage')
                provenance = payload['provenance']
                telemetry = payload['telemetry']
                expected_decoder = dict(cuda='nvdec', vulkan='vulkan_video', metal='videotoolbox', cpu='software')[args.backend]
                if (provenance['decoder'] != expected_decoder
                        or provenance.get('fallback_chain')
                        or (args.backend != 'cpu' and not provenance['zero_copy'])):
                    raise RuntimeError(f'Unexpected decode route: {provenance}')
                print(json.dumps(dict(repeat=repeat, warmup=repeat == 0,
                                      wall_ms=job['wall_ms'], telemetry=telemetry,
                                      provenance=provenance)), flush=True)
        finally:
            if process.poll() is None:
                try:
                    send(dict(protocol_version=1, type='shutdown', request_id='shutdown'))
                    process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()

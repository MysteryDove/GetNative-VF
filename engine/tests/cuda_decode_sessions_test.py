#!/usr/bin/env python3
"""Compare engines built with internal fixed single/dual decode configurations."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

from worker_protocol_test import Worker, collect_verify, verify_media_command

BACKEND = sys.argv[3] if len(sys.argv) > 3 else 'cuda'
DECODER = {'cuda': 'nvdec', 'vulkan': 'vulkan_video'}[BACKEND]


def close(worker):
    if worker.process.poll() is None:
        worker.send(protocol_version=1, type='shutdown', request_id='shutdown')
        try:
            worker.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            worker.process.kill()
            worker.process.wait()


def open_worker(sessions):
    worker = Worker(engine=sys.argv[1] if sessions == 1 else sys.argv[2])
    try:
        worker.send(protocol_version=1, type='hello', request_id='hello')
        worker.read_event()
    except BaseException:
        close(worker)
        raise
    return worker


def run(worker, media, scope=None, concurrency=8):
    worker.send(**verify_media_command('verify', str(media), BACKEND,
                                      concurrency=concurrency, scan_scope=scope))
    terminal, _, warnings = collect_verify(worker)
    assert terminal['type'] == 'result', terminal
    payload = terminal['payload']
    assert not warnings and not payload['provenance']['fallback_chain'], warnings
    assert payload['provenance']['decoder'] == DECODER
    assert payload['provenance']['zero_copy'] is True
    assert payload['frames_failed'] == 0
    rows = sorted(payload['frames'], key=lambda row: row['seq'])
    assert [row['seq'] for row in rows] == list(range(len(rows)))
    assert len({row['frame_index'] for row in rows}) == len(rows)
    return rows, payload['telemetry']['decode_sessions']


def main():
    if len(sys.argv) < 3 or not sys.argv[2]:
        print('SKIP: internally fixed dual-tier comparison engine not provided')
        return 77
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print('SKIP: ffmpeg unavailable')
        return 77
    first, second = open_worker(1), None
    try:
        first.send(protocol_version=1, type='capabilities', request_id='caps')
        capabilities = first.read_event()['payload']
        cuda = next((b for b in capabilities['backends'] if b['id'] == BACKEND), {})
        nvdec = next((b for b in capabilities.get('decode_backends', [])
                      if b['id'] == DECODER), {})
        if not cuda.get('device_available') or not nvdec.get('runtime_device'):
            print(f'SKIP: {BACKEND}/{DECODER} device unavailable')
            return 77
        with tempfile.TemporaryDirectory() as temporary:
            fixture_env = dict(os.environ)
            fixture_env.pop('LD_LIBRARY_PATH', None)
            media = Path(temporary) / 'long-gop.mp4'
            encoded = subprocess.run([
                ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=128x96:rate=24',
                '-frames:v', '2400', '-c:v', 'libx264', '-g', '48', '-bf', '3',
                '-x264-params', 'open-gop=0:scenecut=0', '-pix_fmt', 'yuv420p', str(media),
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=fixture_env)
            if encoded.returncode:
                print('SKIP: libx264 unavailable')
                return 77
            second = open_worker(2)
            for name, scope, concurrency, sessions in [
                ('full', None, 8, 2),
                ('partial-GOP ends', {'selection': 'all', 'start_frame': 53, 'end_frame': 2199}, 8, 2),
                ('sparse', {'selection': 'every_n', 'every_n': 2}, 8, 1),
                ('short', {'selection': 'all', 'start_frame': 0, 'end_frame': 255}, 8, 1),
                ('serial analysis', None, 1, 1),
            ]:
                baseline, used = run(first, media, scope, concurrency)
                actual, selected = run(second, media, scope, concurrency)
                assert used == 1 and selected == sessions, (name, used, selected)
                assert actual == baseline, f'{name}: frame identity or metric changed'
                print(f'PASS {name}: {len(actual)} identical frames, sessions={selected}')
            second.send(**verify_media_command('cancel-job', str(media), BACKEND, concurrency=8))
            cancelled = False
            while True:
                event = second.read_event()
                if event['type'] == 'accepted':
                    job = event['job_id']
                if event['type'] == 'progress' and event.get('completed', 0) >= 32 and not cancelled:
                    second.send(protocol_version=1, type='cancel', request_id='cancel', job_id=job)
                    cancelled = True
                if event['type'] in ('result', 'error', 'cancelled'):
                    assert event['type'] == 'cancelled', event
                    break
            actual, selected = run(second, media)
            baseline, _ = run(first, media)
            assert selected == 2 and actual == baseline
            print('PASS cancellation and decoder/analysis reuse')
        return 0
    finally:
        close(first)
        if second is not None:
            close(second)


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Exact hardware Verify parity between internally fixed one/two-session engines."""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('dual', type=Path)
    parser.add_argument('--backend', choices=['cuda', 'vulkan'], required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--validation-log', type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
    from worker_protocol_test import Worker, collect_verify, verify_media_command
    if args.validation_log:
        from vulkan_decode_runs_test import ValidationWorker
        Worker = ValidationWorker

    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(backend=args.backend, cases=[], binaries={})
    report['runtime_library_path'] = os.environ.get('LD_LIBRARY_PATH', '')
    for name in ['baseline', 'dual']:
        binary = getattr(args, name).resolve()
        report['binaries'][name] = dict(path=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    workers = []
    try:
        for binary in [args.baseline, args.dual]:
            worker = Worker(engine=str(binary.resolve()))
            workers.append(worker)
            worker.send(protocol_version=1, type='hello', request_id='hello')
            worker.read_event()
        with tempfile.TemporaryDirectory(prefix='getnative-hardware-matrix-') as scratch:
            fixture_env = dict(os.environ)
            fixture_env.pop('LD_LIBRARY_PATH', None)
            for bits, codec in [(8, 'libx264'), (10, 'libx265')]:
                media = Path(scratch) / f'closed-{bits}.mkv'
                parameters = ['-x264-params', 'open-gop=0:scenecut=0:threads=2'] if bits == 8 else [
                    '-x265-params', 'open-gop=0:scenecut=0:pools=2:frame-threads=1']
                subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    'testsrc2=size=256x192:rate=24', '-frames:v', '1200', '-c:v', codec,
                    '-g', '48', '-bf', '3', '-pix_fmt', 'yuv420p' if bits == 8 else 'yuv420p10le',
                    *parameters, str(media)], check=True, capture_output=True, timeout=120, env=fixture_env)
                probed = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                    '-show_frames', '-show_entries', 'frame=pict_type', '-of', 'json', str(media)],
                    check=True, capture_output=True, text=True, timeout=60, env=fixture_env)
                i_frames = [i for i, frame in enumerate(json.loads(probed.stdout)['frames'])
                            if frame['pict_type'] == 'I']
                kernels = [dict(id='bilinear'), dict(id='lanczos', taps=3)]
                modes = [(norm, axis, kernel, 'all', None, 2)
                    for norm, axis, kernel in itertools.product(
                        range(1, 5), ['h_only', 'w_only', 'h_plus_w'], kernels)]
                modes.extend([
                    (4, 'h_plus_w', kernels[1], 'partial',
                     dict(selection='all', start_frame=53, end_frame=1199), 2),
                    (4, 'h_plus_w', kernels[1], 'every_n', dict(selection='every_n', every_n=7), 1),
                    (4, 'h_plus_w', kernels[1], 'i_frames', dict(selection='decoded_i_picture'), 1),
                ])
                for norm, axis, kernel, selection, scope, expected_sessions in modes:
                    case = dict(bits=bits, norm=norm, axis=axis, kernel=kernel, selection=selection)
                    rows = []
                    for worker, expected in zip(workers, [1, expected_sessions]):
                        command = verify_media_command('matrix', str(media), args.backend,
                                                       width=256, height=192,
                                                       scan_scope=scope, concurrency=4)
                        command.update(axis_mode=axis, kernel=kernel,
                                       candidate='192' if axis == 'w_only' else '128')
                        command['metric']['p_norm'] = norm
                        worker.send(**command)
                        terminal, _, warnings = collect_verify(worker, timeout=60)
                        if args.validation_log and args.validation_log.exists():
                            validation = args.validation_log.read_text()
                            assert 'Validation Error:' not in validation, validation[-6000:]
                        assert terminal['type'] == 'result', (case, terminal)
                        payload = terminal['payload']
                        assert not warnings and not payload['provenance']['fallback_chain'], (case, warnings)
                        assert payload['provenance']['decoder'] == ('nvdec' if args.backend == 'cuda' else 'vulkan_video')
                        assert payload['provenance']['bit_depth'] == bits, (case, payload['provenance'])
                        assert payload['provenance']['zero_copy']
                        assert payload['frames_failed'] == 0
                        assert payload['telemetry']['decode_sessions'] == expected, (case, payload['telemetry'])
                        frames = sorted(payload['frames'], key=lambda f: f['seq'])
                        assert frames and [f['seq'] for f in frames] == list(range(len(frames)))
                        assert len({f['frame_index'] for f in frames}) == len(frames)
                        wanted = {'all': range(1200), 'partial': range(53, 1200),
                                  'every_n': range(0, 1200, 7), 'i_frames': i_frames}[selection]
                        assert [f['frame_index'] for f in frames] == list(wanted), case
                        rows.append(frames)
                    if rows[0] != rows[1]:
                        mismatch = next((i for i, (left, right) in enumerate(zip(rows[0], rows[1])) if left != right), None)
                        case['mismatch_seq'] = mismatch
                        case['baseline_record'] = rows[0][mismatch] if mismatch is not None else None
                        case['dual_record'] = rows[1][mismatch] if mismatch is not None else None
                        report['cases'].append(case)
                        args.output.write_text(json.dumps(report, indent=2))
                        raise AssertionError((case, 'exact frame mismatch'))
                    case.update(frames=len(rows[0]), sessions=expected_sessions,
                                records_sha256=hashlib.sha256(json.dumps(rows[0], sort_keys=True).encode()).hexdigest())
                    report['cases'].append(case)
                    args.output.write_text(json.dumps(report, indent=2))
                    print(json.dumps(case), flush=True)
        report['passed'] = True
        args.output.write_text(json.dumps(report, indent=2))
    finally:
        for worker in workers:
            if worker.process.poll() is None:
                try:
                    worker.send(protocol_version=1, type='shutdown', request_id='shutdown')
                    worker.process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    worker.process.kill()
                    worker.process.wait()


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Interleaved same-backend fixed/automatic Verify runs with exact frame parity."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--automatic', type=Path, required=True)
    p.add_argument('--media', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--backend', choices=['cuda', 'vulkan', 'metal'], required=True)
    p.add_argument('--frames', type=int, required=True)
    p.add_argument('--pairs', type=int, default=3)
    args = p.parse_args()
    if args.pairs < 1 or args.frames < 1:
        p.error('frames and pairs must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(backend=args.backend, frames=args.frames, runs=[], binaries={})
    report['runtime_library_path'] = os.environ.get('LD_LIBRARY_PATH', '')
    for name, binary in [('baseline', args.baseline), ('automatic', args.automatic)]:
        report['binaries'][name] = dict(path=str(binary.resolve()), sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
        if sys.platform.startswith('linux'):
            linked = subprocess.run(['ldd', str(binary.resolve())], check=True,
                                    capture_output=True, text=True)
            libraries = {}
            for line in linked.stdout.splitlines():
                library, separator, target = line.strip().partition(' => ')
                if separator and library.startswith(('libavcodec.', 'libavformat.', 'libavutil.', 'libswscale.')):
                    path = Path(target.split()[0]).resolve(strict=True)
                    libraries[library] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            report['binaries'][name]['ffmpeg_runtime'] = libraries
    expected = None
    for pair in range(args.pairs):
        order = ['baseline', 'automatic'] if pair % 2 == 0 else ['automatic', 'baseline']
        for name in order:
            output = args.output / f'{pair}-{name}.json'
            command = [sys.executable, str(Path(__file__).with_name('media_verify_benchmark.py')),
                       str(getattr(args, name)), str(args.media), '--backend', args.backend,
                       '--frames', str(args.frames), '--repeats', '1', '--output', str(output)]
            memory_file = args.output / f'{pair}-{name}-card-memory.csv'
            monitor = None
            with memory_file.open('w') as memory, output.with_suffix('.runner.log').open('w') as log:
                if args.backend != 'metal':
                    try:
                        monitor = subprocess.Popen(['nvidia-smi', '--query-gpu=timestamp,memory.used',
                            '--format=csv,noheader,nounits', '-lms', '200'], stdout=memory, stderr=subprocess.DEVNULL)
                    except FileNotFoundError:
                        pass
                started = time.monotonic()
                try:
                    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=600)
                finally:
                    if monitor:
                        monitor.terminate()
                        monitor.wait(timeout=10)
            measured = json.loads(output.read_text())
            for job in measured['jobs']:
                rows = sorted(job['terminal']['payload']['frames'], key=lambda frame: frame['seq'])
                if expected is None:
                    expected = rows
                if rows != expected:
                    raise RuntimeError(f'{name} pair {pair}: exact frame parity failed')
            job = measured['jobs'][-1]
            telemetry = job['terminal']['payload']['telemetry']
            entry = dict(pair=pair, mode=name, job_total_ms=telemetry['job_total_ms'],
                         wall_ms=job['wall_ms'], runner_ms=(time.monotonic()-started)*1000,
                         sessions=telemetry['decode_sessions'], changes=telemetry.get('decode_session_changes', []))
            samples = []
            for line in memory_file.read_text().splitlines():
                try:
                    samples.append(float(line.rsplit(',', 1)[1]))
                except (ValueError, IndexError):
                    pass
            entry['whole_card_peak_mib_including_warmup'] = max(samples) if samples else None
            report['runs'].append(entry)
            (args.output / 'summary.json').write_text(json.dumps(report, indent=2))
            print(json.dumps(entry), flush=True)
    report['summary'] = {}
    for name in ['baseline', 'automatic']:
        values = [r['job_total_ms'] for r in report['runs'] if r['mode'] == name]
        median = statistics.median(values)
        report['summary'][name] = dict(median_ms=median, min_ms=min(values), max_ms=max(values),
                                     spread_percent=(max(values)-min(values))/median*100,
                                     median_fps=args.frames*1000/median)
    ratio = report['summary']['automatic']['median_ms'] / report['summary']['baseline']['median_ms']
    report['summary']['automatic_time_ratio'] = ratio
    report['summary']['no_more_than_three_percent_regression'] = ratio <= 1.03
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report['summary']), flush=True)
    return 0 if ratio <= 1.03 else 1


if __name__ == '__main__':
    raise SystemExit(main())

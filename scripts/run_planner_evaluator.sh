#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$repo_root"

assert_no_competing_benchmark() {
    competing_benchmarks=$(ps -axo comm= | awk '/getnative_.*benchmark$/ { print }')
    if [ -n "$competing_benchmarks" ]; then
        printf '%s\n%s\n' \
            "planner evaluator refuses to run beside another GetNative benchmark:" \
            "$competing_benchmarks" >&2
        exit 1
    fi
}

branch=$(git branch --show-current)
if [ "$branch" != "perf/pre-gpu-stage1" ]; then
    echo "planner evaluator requires perf/pre-gpu-stage1, got: $branch" >&2
    exit 1
fi

assert_no_competing_benchmark

cmake -S engine -B build/stage0-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DGETNATIVE_ENABLE_METAL=ON \
    -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
cmake --build build/stage0-release --parallel
ctest --test-dir build/stage0-release --output-on-failure

mkdir -p artifacts/stage0
run_dir="artifacts/stage0/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir "$run_dir"

uptime > "$run_dir/host-load-before.txt"
ps -axo pid,pcpu,etime,comm > "$run_dir/host-processes-before.txt"

assert_no_competing_benchmark
build/stage0-release/getnative_core_benchmark \
    --planner-mode serial --samples 21 \
    --json-out "$run_dir/core-serial.json" --assert
assert_no_competing_benchmark
build/stage0-release/getnative_metal_benchmark \
    --full --planner-mode serial --samples 21 \
    --json-out "$run_dir/metal-serial.json" --assert

uptime > "$run_dir/host-load-after.txt"
ps -axo pid,pcpu,etime,comm > "$run_dir/host-processes-after.txt"

printf '%s\n' "$run_dir"

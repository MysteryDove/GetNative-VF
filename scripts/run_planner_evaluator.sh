#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$repo_root"

stage=${1:-stage0}
case "$stage" in
stage0 | stage1 | cache-baseline) ;;
*)
    echo "usage: $0 [stage0|stage1|cache-baseline]" >&2
    exit 1
    ;;
esac

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

if [ "$stage" = "cache-baseline" ]; then
    cmake -S engine -B build/cache-baseline-release \
        -DCMAKE_BUILD_TYPE=Release \
        -DGETNATIVE_ENABLE_METAL=ON \
        -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
    cmake --build build/cache-baseline-release --parallel
    ctest --test-dir build/cache-baseline-release --output-on-failure

    mkdir -p artifacts/cache-baseline
    run_dir="artifacts/cache-baseline/$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir "$run_dir"

    uptime > "$run_dir/host-load-before.txt"
    ps -axo pid,pcpu,etime,comm > "$run_dir/host-processes-before.txt"

    assert_no_competing_benchmark
    build/cache-baseline-release/getnative_metal_benchmark \
        --full --compare-cross-call-cache --samples 21 \
        --json-out "$run_dir/metal-cache-paired.json" --assert

    uptime > "$run_dir/host-load-after.txt"
    ps -axo pid,pcpu,etime,comm > "$run_dir/host-processes-after.txt"

    jq -e '
        .sample_count == 21
        and .cache_baseline_status == "MEASURED"
        and (.cache_baseline_decision == "PROCEED_SESSION_CACHE_STAGE"
             or .cache_baseline_decision == "STOP_NO_MEASURED_CACHE_BENEFIT")
        and (.pair_order | length) == 21
        and (.cold_batch.metrics.plan_ms.raw | length) == 21
        and (.warm_cache.metrics.plan_ms.raw | length) == 21
        and (.paired.plan_delta.raw | length) == 21
        and (.paired.metal_total_delta.raw | length) == 21
        and (.paired.controlled_metal_total_delta.raw | length) == 21
        and (.retained_cache.setup_only_serial_prewarm_ms.raw | length) == 21
        and (.amortized_video.frames | length) == 5
        and .paired.plan_delta.mad <= 0.025
        and .paired.controlled_metal_total_delta.mad <= 0.025
        and .cold_batch.planner.physical_build_count == .fixture.candidates
        and .warm_cache.planner.physical_build_count == 0
        and .warm_cache.planner.ready_hit_count == .fixture.candidates
        and .retained_cache.entry_count == .fixture.candidates
        and .retained_cache.logical_plan_bytes > 0
        and .correctness.warm_requests_are_ready_hits
        and .correctness.strict_tolerance
        and .correctness.assertions
    ' "$run_dir/metal-cache-paired.json" >/dev/null

    if find "$run_dir" -maxdepth 1 -name '*.tmp' -print -quit | grep -q .; then
        echo "cache baseline left a temporary artifact" >&2
        exit 1
    fi

    printf '%s\n' "$run_dir"
    exit 0
fi

if [ "$stage" = "stage1" ]; then
    cmake -S engine -B build/stage1-release \
        -DCMAKE_BUILD_TYPE=Release \
        -DGETNATIVE_ENABLE_METAL=ON \
        -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
    cmake --build build/stage1-release --parallel
    ctest --test-dir build/stage1-release --output-on-failure

    cmake -S engine -B build/stage1-cpu \
        -DCMAKE_BUILD_TYPE=Release \
        -DGETNATIVE_ENABLE_METAL=OFF \
        -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
    cmake --build build/stage1-cpu --parallel
    ctest --test-dir build/stage1-cpu --output-on-failure

    cmake -S engine -B build/stage1-tsan \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DGETNATIVE_ENABLE_METAL=OFF \
        -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=OFF \
        -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
        -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
    cmake --build build/stage1-tsan --parallel
    TSAN_OPTIONS=halt_on_error=1 \
        ctest --test-dir build/stage1-tsan --output-on-failure \
            -R '^getnative_axis_planner_tests$'

    mkdir -p artifacts/stage1
    run_dir="artifacts/stage1/$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir "$run_dir"

    uptime > "$run_dir/host-load-before.txt"
    ps -axo pid,pcpu,etime,comm > "$run_dir/host-processes-before.txt"

    assert_no_competing_benchmark
    build/stage1-release/getnative_core_benchmark \
        --compare-planner-modes --samples 21 \
        --json-out "$run_dir/core-paired.json" --assert
    assert_no_competing_benchmark
    build/stage1-release/getnative_metal_benchmark \
        --full --compare-planner-modes --samples 21 \
        --json-out "$run_dir/metal-paired.json" --assert

    uptime > "$run_dir/host-load-after.txt"
    ps -axo pid,pcpu,etime,comm > "$run_dir/host-processes-after.txt"

    printf '%s\n' "$run_dir"
    exit 0
fi

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

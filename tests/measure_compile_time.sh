#!/usr/bin/env bash
# Phase 3 compile-time measurement gate.
# Runs each workload N times through both QBE and LLVM, prints median wall
# times and the LLVM/QBE ratio. Used as a one-off Phase 3 gate (per the
# acceptance criteria in LLVM_MIGRATION_PLAN.md).
#
# Usage:
#   tests/measure_compile_time.sh
#
# Output is plain text on stdout suitable for pasting into CHANGELOG.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIX="$SCRIPT_DIR/../build/mix"
N=5

if [ ! -x "$MIX" ]; then
    echo "error: mix binary not found at $MIX" >&2
    exit 2
fi

# (label, source_file)
WORKLOADS=(
    "hello                examples/hello.mix"
    "001_arithmetic       tests/programs/001_arithmetic.mix"
    "007_fibonacci        tests/programs/007_fibonacci.mix"
    "012_for_range        tests/programs/012_for_range.mix"
    "big_scalar           tests/scratch/big_scalar.mix"
    "big_shapes           tests/scratch/big_shapes.mix"
)

# nanoseconds since epoch via Python (more portable than `date +%N` on macOS).
nanos() { python3 -c 'import time; print(int(time.time()*1e9))'; }

median() {
    # Reads N numbers, prints the median (no awk dependency on bc).
    python3 - "$@" <<'PY'
import sys
xs = sorted(int(x) for x in sys.argv[1:])
n = len(xs)
print(xs[n//2] if n % 2 else (xs[n//2-1]+xs[n//2])//2)
PY
}

run_one() {
    local backend="$1"
    local src="$2"
    local out
    out=$(mktemp)
    local t0 t1
    t0=$(nanos)
    "$MIX" build --backend "$backend" -o "$out" "$src" >/dev/null 2>&1
    t1=$(nanos)
    rm -f "$out"
    echo $((t1 - t0))
}

printf "%-22s %12s %12s %8s\n" "workload" "qbe(ms)" "llvm(ms)" "ratio"
printf "%-22s %12s %12s %8s\n" "----------------------" "------------" "------------" "--------"

for entry in "${WORKLOADS[@]}"; do
    label="${entry%% *}"
    src="${entry#"${entry%% *}"}"
    src="${src#"${src%%[! ]*}"}"   # trim leading spaces

    qbe_samples=()
    for ((i=0; i<N; i++)); do qbe_samples+=("$(run_one qbe  "$src")"); done
    llvm_samples=()
    for ((i=0; i<N; i++)); do llvm_samples+=("$(run_one llvm "$src")"); done

    qbe_med=$(median "${qbe_samples[@]}")
    llvm_med=$(median "${llvm_samples[@]}")
    qbe_ms=$(python3 -c "print(f'{$qbe_med/1e6:.1f}')")
    llvm_ms=$(python3 -c "print(f'{$llvm_med/1e6:.1f}')")
    ratio=$(python3 -c "print(f'{$llvm_med/$qbe_med:.1f}x')")

    printf "%-22s %12s %12s %8s\n" "$label" "$qbe_ms" "$llvm_ms" "$ratio"
done

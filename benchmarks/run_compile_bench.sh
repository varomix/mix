#!/usr/bin/env bash
# Compile-time benchmark: how fast does mix compile representative programs?
# Useful for spotting compiler regressions.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MIXC="$PROJECT_DIR/build/mix"

if [ ! -f "$MIXC" ]; then
    (cd "$PROJECT_DIR" && make)
fi

PROGRAMS=(
    "$PROJECT_DIR/tests/programs/048_std_collections.mix"
    "$PROJECT_DIR/tests/programs/061_stdlib.mix"
    "$PROJECT_DIR/tests/programs/065_generic_monomorphization.mix"
    "$PROJECT_DIR/benchmarks/bench_generic_stack.mix"
)
RUNS=5

median() {
    printf '%s\n' "$@" | sort -n | awk 'BEGIN{c=0} {a[c++]=$0} END{print a[int(c/2)]}'
}

time_cmd() {
    perl -MTime::HiRes=time -e '
        my $start = time();
        system($ARGV[0] . " > /dev/null 2>&1");
        printf("%.3f\n", time() - $start);
    ' "$1"
}

echo "=============================================="
echo "  MIX Compile-time Benchmarks"
echo "=============================================="
echo ""
printf "%-50s %10s\n" "Program" "Time (s)"
printf "%-50s %10s\n" "-------" "--------"

OUT_BIN="$SCRIPT_DIR/_compile_bench.tmp"
for prog in "${PROGRAMS[@]}"; do
    name=$(basename "$prog" .mix)
    times=()
    for ((r=0; r<RUNS; r++)); do
        t=$(time_cmd "$MIXC build $prog -o $OUT_BIN")
        times+=("$t")
    done
    med=$(median "${times[@]}")
    printf "%-50s %10s\n" "$name" "$med"
done
rm -f "$OUT_BIN"

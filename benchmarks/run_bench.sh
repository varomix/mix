#!/usr/bin/env bash
# Benchmark runner: compiles MIX and C versions, runs each 5 times, reports median
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MIXC="$PROJECT_DIR/build/mix"
BENCH_DIR="$SCRIPT_DIR"

# Build mixc if needed
if [ ! -f "$MIXC" ]; then
    echo "Building mix..."
    (cd "$PROJECT_DIR" && make)
fi

BENCHMARKS="bench_sum_squares bench_fib bench_list bench_string bench_generic_stack"
RUNS=5

# Get median of values (pipe sorted floats, pick middle)
median() {
    local sorted
    sorted=$(printf '%s\n' "$@" | sort -n)
    local count=$#
    local mid=$(( (count + 1) / 2 ))
    echo "$sorted" | sed -n "${mid}p"
}

# Time a command using high-res perl timer, returns seconds with 3 decimals
time_cmd() {
    local cmd="$1"
    perl -MTime::HiRes=time -e '
        my $start = time();
        system($ARGV[0] . " > /dev/null 2>&1");
        my $elapsed = time() - $start;
        printf("%.3f\n", $elapsed);
    ' "$cmd"
}

echo "=============================================="
echo "  MIX Performance Benchmarks"
echo "=============================================="
echo ""

# Also output JSON for the HTML page
JSON_FILE="$BENCH_DIR/results.json"
echo "[" > "$JSON_FILE"
first_json=true

printf "%-20s %10s %10s %8s\n" "Benchmark" "MIX (s)" "C (s)" "Ratio"
printf "%-20s %10s %10s %8s\n" "---------" "-------" "-----" "-----"

for bench in $BENCHMARKS; do
    mix_src="$BENCH_DIR/${bench}.mix"
    c_src="$BENCH_DIR/${bench}.c"
    mix_bin="$BENCH_DIR/${bench}_mix"
    c_bin="$BENCH_DIR/${bench}_c"

    # Compile MIX version
    "$MIXC" "$mix_src" -o "$mix_bin" 2>/dev/null

    # Compile C version with -O2
    cc -O2 "$c_src" -o "$c_bin"

    # Run MIX N times
    mix_times=()
    for ((r=0; r<RUNS; r++)); do
        t=$(time_cmd "$mix_bin")
        mix_times+=("$t")
    done
    mix_median=$(median "${mix_times[@]}")

    # Run C N times
    c_times=()
    for ((r=0; r<RUNS; r++)); do
        t=$(time_cmd "$c_bin")
        c_times+=("$t")
    done
    c_median=$(median "${c_times[@]}")

    # Compute ratio (MIX / C)
    if (( $(echo "$c_median > 0" | bc -l) )); then
        ratio=$(echo "scale=2; $mix_median / $c_median" | bc -l)
    else
        ratio="N/A"
    fi

    printf "%-20s %10s %10s %8sx\n" "$bench" "$mix_median" "$c_median" "$ratio"

    # JSON entry
    if [ "$first_json" = true ]; then
        first_json=false
    else
        echo "," >> "$JSON_FILE"
    fi
    cat >> "$JSON_FILE" <<JSONEOF
  {"name": "$bench", "mix": $mix_median, "c": $c_median, "ratio": $ratio}
JSONEOF

    # Cleanup binaries
    rm -f "$mix_bin" "$c_bin"
done

echo "" >> "$JSON_FILE"
echo "]" >> "$JSON_FILE"

echo ""
echo "Ratio = MIX time / C time (lower is better, 1.0 = same speed)"
echo "C compiled with -O2"
echo "Results saved to $JSON_FILE"

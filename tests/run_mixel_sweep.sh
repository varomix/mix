#!/usr/bin/env bash
# Mixel compile sweep. Compiles every main.mix under mixel/examples and
# reports pass/fail. Used as a Phase 0 guardrail and a per-phase backend
# parity check for the LLVM migration.
#
# Usage:
#   tests/run_mixel_sweep.sh            # default backend (llvm)
#   tests/run_mixel_sweep.sh c          # C backend
#
# Exit code: 0 if all demos compile, 1 otherwise.

set -u

BACKEND="${1:-llvm}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIX_LANG_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MIX_DEV_DIR="$(cd "$MIX_LANG_DIR/.." && pwd)"
MIX_BIN="$MIX_LANG_DIR/build/mix"
EXAMPLES_DIR="$MIX_DEV_DIR/mixel/examples"

if [ ! -x "$MIX_BIN" ]; then
    echo "error: mix binary not found at $MIX_BIN" >&2
    echo "       run 'make' in $MIX_LANG_DIR first" >&2
    exit 2
fi

if [ ! -d "$EXAMPLES_DIR" ]; then
    echo "error: mixel examples dir not found at $EXAMPLES_DIR" >&2
    exit 2
fi

pass=0
fail=0
failed_names=()

# Sort so the report is stable across runs.
mains=$(find "$EXAMPLES_DIR" -name "main.mix" | sort)

for main_file in $mains; do
    demo_dir="$(dirname "$main_file")"
    # Show a stable, readable name relative to mixel/examples.
    rel_name="${demo_dir#$EXAMPLES_DIR/}"

    pushd "$demo_dir" > /dev/null
    if "$MIX_BIN" build --backend "$BACKEND" main.mix > /tmp/mixel_sweep.log 2>&1; then
        echo "PASS  $rel_name"
        pass=$((pass + 1))
        # Clean up the produced binary so subsequent runs do not pick up
        # stale artifacts and so the working tree stays tidy.
        rm -f main main.o
    else
        echo "FAIL  $rel_name"
        echo "----- log -----"
        cat /tmp/mixel_sweep.log
        echo "---------------"
        fail=$((fail + 1))
        failed_names+=("$rel_name")
    fi
    popd > /dev/null
done

total=$((pass + fail))
echo
echo "Mixel sweep results (backend=$BACKEND): $pass passed, $fail failed (of $total)"

if [ $fail -ne 0 ]; then
    echo "Failed demos:"
    for name in "${failed_names[@]}"; do
        echo "  - $name"
    done
    exit 1
fi
exit 0

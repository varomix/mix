#!/bin/bash
# MIX language end-to-end test runner.
#
# Backend selection (LLVM is the default; the others stay while we wind
# QBE down and keep C as a small-target fallback):
#   ./tests/run_tests.sh                    # llvm (default)
#   MIX_BACKEND=qbe ./tests/run_tests.sh    # legacy QBE, parity oracle
#   MIX_BACKEND=c   ./tests/run_tests.sh    # C fallback

MIXC="./build/mix"
TEST_DIR="tests/programs"
EXPECTED_DIR="tests/programs/expected"
BUILD_DIR="build/test_output"

backend_flag=""
if [ -n "${MIX_BACKEND:-}" ]; then
    backend_flag="--backend $MIX_BACKEND"
fi

mkdir -p "$BUILD_DIR"

pass=0
fail=0
errors=""

for src in "$TEST_DIR"/*.mix; do
    name=$(basename "$src" .mix)
    num="${name%%_*}"
    expected="$EXPECTED_DIR/$num.txt"
    binary="$BUILD_DIR/$name"

    # Check for companion link flags file (e.g., 055_use_c.link)
    # Lines containing .c files are compiled to .o first and added via LDFLAGS
    extra_flags=""
    extra_ldflags=""
    link_file="$TEST_DIR/$name.link"
    if [ -f "$link_file" ]; then
        while IFS= read -r line; do
            line=$(echo "$line" | xargs)  # trim whitespace
            [ -z "$line" ] && continue
            if [[ "$line" == *.c ]]; then
                obj="${BUILD_DIR}/$(basename "$line" .c).o"
                cc -c "$line" -o "$obj" 2>/dev/null
                extra_ldflags="$extra_ldflags $obj"
            else
                extra_flags="$extra_flags $line"
            fi
        done < "$link_file"
    fi

    # Compile (LDFLAGS passes extra object files to the linker)
    compile_out=$(LDFLAGS="$extra_ldflags $LDFLAGS" "$MIXC" $backend_flag "$src" -o "$binary" $extra_flags 2>&1)
    if [ $? -ne 0 ]; then
        echo "FAIL  $name  (compile error)"
        echo "      $compile_out"
        fail=$((fail + 1))
        errors="$errors\n  $name: compile error"
        continue
    fi

    # Run
    actual=$(timeout 5 "$binary" 2>&1)
    exit_code=$?

    if [ ! -f "$expected" ]; then
        # No expected file — just check it runs without crashing
        if [ $exit_code -eq 0 ]; then
            echo "PASS  $name  (no expected output, exit 0)"
            pass=$((pass + 1))
        else
            echo "FAIL  $name  (exit code $exit_code)"
            fail=$((fail + 1))
            errors="$errors\n  $name: exit code $exit_code"
        fi
        continue
    fi

    # Compare output
    expected_content=$(cat "$expected")
    if [ "$actual" = "$expected_content" ]; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name"
        echo "      expected: $(head -1 "$expected")..."
        echo "      actual:   $(echo "$actual" | head -1)..."
        fail=$((fail + 1))
        errors="$errors\n  $name: output mismatch"
    fi
done

echo ""
echo "Results: $pass passed, $fail failed"

if [ $fail -gt 0 ]; then
    echo -e "Failures:$errors"
    exit 1
fi

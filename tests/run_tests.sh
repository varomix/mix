#!/bin/bash
# MIX language end-to-end test runner

MIXC="./build/mix"
TEST_DIR="tests/programs"
EXPECTED_DIR="tests/programs/expected"
BUILD_DIR="build/test_output"

mkdir -p "$BUILD_DIR"

pass=0
fail=0
errors=""

for src in "$TEST_DIR"/*.mix; do
    name=$(basename "$src" .mix)
    num="${name%%_*}"
    expected="$EXPECTED_DIR/$num.txt"
    binary="$BUILD_DIR/$name"

    # Compile
    compile_out=$("$MIXC" "$src" -o "$binary" 2>&1)
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

#!/bin/bash
# Test that type errors are properly caught by the compiler
# Each .mix file in tests/errors/ should FAIL to compile
# The expected error pattern is in the first line comment: // ERROR: <pattern>

MIXC="./build/mix"
TEST_DIR="tests/errors"
pass=0
fail=0

for src in "$TEST_DIR"/*.mix; do
    name=$(basename "$src" .mix)
    expected_err=$(head -1 "$src" | sed 's|^// ERROR: ||')

    output=$("$MIXC" "$src" -o /tmp/mix_errtest_bin 2>&1)
    rc=$?

    if [ $rc -eq 0 ]; then
        echo "FAIL  $name  (compiled successfully, should have errored)"
        fail=$((fail + 1))
    elif echo "$output" | grep -q "$expected_err"; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name  (wrong error)"
        echo "      expected pattern: $expected_err"
        echo "      got: $(echo "$output" | grep 'error:' | head -1)"
        fail=$((fail + 1))
    fi
    rm -f /tmp/mix_errtest_bin
done

echo ""
echo "Results: $pass passed, $fail failed"
[ $fail -gt 0 ] && exit 1
exit 0

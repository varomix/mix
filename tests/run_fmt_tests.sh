#!/bin/bash
# Smoke tests for `mix fmt`:
#   1. Format a sample → confirm output is non-empty.
#   2. Format twice → output must be byte-identical (idempotence).
#   3. Format a small set of programs and verify behavior is preserved.

MIXC="./build/mix"
pass=0
fail=0

ok() { echo "PASS  $1"; pass=$((pass + 1)); }
ng() { echo "FAIL  $1"; fail=$((fail + 1)); }

# --- Idempotence: format twice produces identical output ---
sample='tests/programs/063_optional_result_patterns.mix'
out1=$("$MIXC" fmt "$sample" 2>/dev/null)
out2=$(echo "$out1" | "$MIXC" fmt 2>/dev/null)

[ -n "$out1" ] && ok "fmt: produces non-empty output" || ng "fmt: produces non-empty output"
[ "$out1" = "$out2" ] && ok "fmt: idempotent" || ng "fmt: idempotent"

# --- Preserves comments ---
echo "$out1" | grep -q "// Pattern arms over optional and result" \
    && ok "fmt: preserves leading comment" \
    || ng "fmt: preserves leading comment"

# --- Restores string-literal quotes ---
echo "$out1" | grep -q '"alice"' \
    && ok "fmt: keeps string literal quotes" \
    || ng "fmt: keeps string literal quotes"

# --- Unary minus glued to operand ---
echo "$out1" | grep -q 'print(-1)' \
    && ok "fmt: unary minus stays glued to operand" \
    || ng "fmt: unary minus stays glued to operand"

# --- Behavior preservation: in-place format and re-run a few simple tests ---
for name in 002_hello 044_match_expr 060_var_shadowing 062_assign_vs_decl; do
    src="tests/programs/${name}.mix"
    expected="tests/programs/expected/${name%%_*}.txt"
    [ -f "$src" ] || continue
    [ -f "$expected" ] || continue

    cp "$src" /tmp/mix_fmt_test.mix
    "$MIXC" fmt -w /tmp/mix_fmt_test.mix 2>/dev/null
    actual=$("$MIXC" run /tmp/mix_fmt_test.mix 2>&1)
    expected_text=$(cat "$expected")
    if [ "$actual" = "$expected_text" ]; then
        ok "fmt preserves behavior: $name"
    else
        ng "fmt preserves behavior: $name"
    fi
done
rm -f /tmp/mix_fmt_test.mix

echo ""
echo "Results: $pass passed, $fail failed"
[ $fail -gt 0 ] && exit 1
exit 0

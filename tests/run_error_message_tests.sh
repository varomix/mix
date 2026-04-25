#!/bin/bash
# Tests for improved error messages: colors, gutter, did-you-mean, error limit, NO_COLOR

MIXC="./build/mix"
pass=0
fail=0

ok() {
    echo "PASS  $1"
    pass=$((pass + 1))
}

ng() {
    echo "FAIL  $1"
    fail=$((fail + 1))
}

# --- Did you mean? (variable) ---
output=$("$MIXC" tests/errors/err_did_you_mean_var.mix -o /tmp/mix_errtest_bin 2>&1)

echo "$output" | grep -q "undefined variable 'scroe'" \
    && ok "did-you-mean variable: reports undefined" \
    || ng "did-you-mean variable: reports undefined"

echo "$output" | grep -q "did you mean 'score'" \
    && ok "did-you-mean variable: suggests 'score'" \
    || ng "did-you-mean variable: suggests 'score'"

# --- Did you mean? (function) ---
output=$("$MIXC" tests/errors/err_did_you_mean_func.mix -o /tmp/mix_errtest_bin 2>&1)

echo "$output" | grep -q "undefined function 'prnt'" \
    && ok "did-you-mean function: reports undefined" \
    || ng "did-you-mean function: reports undefined"

echo "$output" | grep -q "did you mean 'print'" \
    && ok "did-you-mean function: suggests 'print'" \
    || ng "did-you-mean function: suggests 'print'"

# --- Line number gutter format ---
output=$("$MIXC" tests/errors/err_did_you_mean_var.mix -o /tmp/mix_errtest_bin 2>&1)

echo "$output" | grep -qE '^ *[0-9]+ \|' \
    && ok "gutter: shows line number with pipe" \
    || ng "gutter: shows line number with pipe"

# --- Caret on its own line below gutter ---
echo "$output" | grep -qE '^ *\|.*\^' \
    && ok "gutter: caret aligned under gutter" \
    || ng "gutter: caret aligned under gutter"

# --- Error limit ---
output=$("$MIXC" tests/errors/err_error_limit.mix -o /tmp/mix_errtest_bin 2>&1)

echo "$output" | grep -q "too many errors emitted, stopping" \
    && ok "error-limit: stops after 10 errors" \
    || ng "error-limit: stops after 10 errors"

error_count=$(echo "$output" | grep -c "error:.*undefined")
[ "$error_count" -le 10 ] \
    && ok "error-limit: at most 10 undefined-error lines" \
    || ng "error-limit: at most 10 undefined-error lines (got $error_count)"

# --- NO_COLOR disables escape codes ---
output=$(NO_COLOR=1 "$MIXC" tests/errors/err_did_you_mean_var.mix -o /tmp/mix_errtest_bin 2>&1)

if printf '%s' "$output" | cat -v | grep -q '\^\['; then
    ng "NO_COLOR: no ANSI escape codes"
else
    ok "NO_COLOR: no ANSI escape codes"
fi

# --- Pipe disables escape codes (stderr piped through cat) ---
output=$("$MIXC" tests/errors/err_did_you_mean_var.mix -o /tmp/mix_errtest_bin 2>&1 | cat)

if printf '%s' "$output" | cat -v | grep -q '\^\['; then
    ng "pipe: no ANSI escape codes"
else
    ok "pipe: no ANSI escape codes"
fi

# --- No suggestion for very different names ---
cat > /tmp/mix_nosuggest.mix << 'MIXEOF'
main()
    print(xyzzyplugh)
MIXEOF
output=$("$MIXC" /tmp/mix_nosuggest.mix -o /tmp/mix_errtest_bin 2>&1)

if echo "$output" | grep -q "did you mean"; then
    ng "no-suggest: no suggestion for very different name"
else
    ok "no-suggest: no suggestion for very different name"
fi

# --- QBE stderr captured (non-verbose, valid program) ---
cat > /tmp/mix_valid.mix << 'MIXEOF'
main()
    print("ok")
MIXEOF
output=$("$MIXC" /tmp/mix_valid.mix -o /tmp/mix_errtest_bin 2>&1)

if echo "$output" | grep -q '/tmp/mix_'; then
    ng "qbe-capture: no /tmp paths in normal output"
else
    ok "qbe-capture: no /tmp paths in normal output"
fi

# --- Match exhaustiveness warning ---
cat > /tmp/mix_nonexh.mix << 'MIXEOF'
shape Color
    Red(v: int)
    Green(v: int)
    Blue(v: int)

main()
    c = Red(v: 1)
    match c
        Red(r) => print(r)
        Green(g) => print(g)
MIXEOF
output=$("$MIXC" /tmp/mix_nonexh.mix -o /tmp/mix_errtest_bin 2>&1)

echo "$output" | grep -q "non-exhaustive match" \
    && ok "exhaustive: warns on missing variant" \
    || ng "exhaustive: warns on missing variant"

echo "$output" | grep -q "'Blue' not handled" \
    && ok "exhaustive: names the missing variant" \
    || ng "exhaustive: names the missing variant"

cat > /tmp/mix_exh_wc.mix << 'MIXEOF'
shape Color
    Red(v: int)
    Green(v: int)
    Blue(v: int)

main()
    c = Red(v: 1)
    match c
        Red(r) => print(r)
        _ => print(0)
MIXEOF
output=$("$MIXC" /tmp/mix_exh_wc.mix -o /tmp/mix_errtest_bin 2>&1)

if echo "$output" | grep -q "non-exhaustive match"; then
    ng "exhaustive: wildcard suppresses warning"
else
    ok "exhaustive: wildcard suppresses warning"
fi

rm -f /tmp/mix_nonexh.mix /tmp/mix_exh_wc.mix

# --- Both backends produce same error for type mismatch ---
output_qbe=$("$MIXC" tests/errors/err_str_to_int_param.mix -o /tmp/mix_errtest_bin 2>&1)
output_c=$(MIX_BACKEND=c "$MIXC" tests/errors/err_str_to_int_param.mix -o /tmp/mix_errtest_bin 2>&1)

[ "$output_qbe" = "$output_c" ] \
    && ok "both-backends: identical error output for type mismatch" \
    || ng "both-backends: identical error output for type mismatch"

rm -f /tmp/mix_errtest_bin /tmp/mix_nosuggest.mix /tmp/mix_valid.mix

echo ""
echo "Results: $pass passed, $fail failed"
[ $fail -gt 0 ] && exit 1
exit 0

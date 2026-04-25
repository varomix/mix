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

# --- Preserves intentional blank lines inside function bodies ---
cat > /tmp/mix_fmt_blanks.mix << 'MIXEOF'
main()
    a = 1
    b = 2

    c = 3
    print(a + b + c)
MIXEOF
out=$("$MIXC" fmt /tmp/mix_fmt_blanks.mix)
# count blank lines (just an empty grep match within the body)
blank_count=$(echo "$out" | grep -c "^$")
[ "$blank_count" -ge 1 ] && ok "fmt: preserves intentional blank lines" \
    || ng "fmt: preserves intentional blank lines"
rm -f /tmp/mix_fmt_blanks.mix

# --- `not X` keeps a space (was concatenating into `notX`) ---
echo 'main()
    while not done()
        print("loop")
done() -> bool
    true' > /tmp/mix_not_kw.mix
out=$("$MIXC" fmt /tmp/mix_not_kw.mix)
echo "$out" | grep -q 'while not done' \
    && ok "fmt: keyword 'not' keeps space before operand" \
    || ng "fmt: keyword 'not' keeps space before operand"
rm -f /tmp/mix_not_kw.mix

# --- Type-keyword constructors: `int(0)`, `set{...}` ---
echo 'main()
    n = int(0)
    s = set{"a", "b"}
    print(n)
    print(s.len)' > /tmp/mix_typector.mix
out=$("$MIXC" fmt /tmp/mix_typector.mix)
echo "$out" | grep -q 'int(0)' \
    && ok "fmt: type-keyword constructor binds to (" \
    || ng "fmt: type-keyword constructor binds to ("
echo "$out" | grep -q 'set{"a"' \
    && ok "fmt: set literal binds keyword to {" \
    || ng "fmt: set literal binds keyword to {"
rm -f /tmp/mix_typector.mix

# --- Mid-expression comments survive across line breaks inside brackets ---
cat > /tmp/mix_midcomm.mix << 'MIXEOF'
sumof(a: int, b: int, c: int) -> int
    a + b + c

main()
    print(sumof(1,
                // doc for b
                2,
                3))
MIXEOF
out=$("$MIXC" fmt /tmp/mix_midcomm.mix)
echo "$out" | grep -q "// doc for b" \
    && ok "fmt: mid-expression comment preserved" \
    || ng "fmt: mid-expression comment preserved"
out2=$(echo "$out" | "$MIXC" fmt)
[ "$out" = "$out2" ] \
    && ok "fmt: mid-expression comment idempotent" \
    || ng "fmt: mid-expression comment idempotent"
rm -f /tmp/mix_midcomm.mix

# --- Real-world: stdlib + examples must already be canonically formatted ---
"$MIXC" fmt --check lib/std/ > /dev/null 2>&1
[ "$?" = "0" ] && ok "fmt: stdlib is canonically formatted" \
    || ng "fmt: stdlib is canonically formatted"

"$MIXC" fmt --check examples/*.mix > /dev/null 2>&1
[ "$?" = "0" ] && ok "fmt: examples are canonically formatted" \
    || ng "fmt: examples are canonically formatted"

# --- Keywords as method names: `s.repeat(n)` keeps no space before ( ---
echo 'main()
    print("ab".repeat(3))' > /tmp/mix_kwmethod.mix
out=$("$MIXC" fmt /tmp/mix_kwmethod.mix)
echo "$out" | grep -q '"ab".repeat(3)' \
    && ok "fmt: keyword used as method name keeps no space before (" \
    || ng "fmt: keyword used as method name keeps no space before ("
rm -f /tmp/mix_kwmethod.mix

# --- `done -1` keeps unary minus glued ---
echo 'sign(x: int) -> int
    if x < 0
        done -1
    1' > /tmp/mix_done_neg.mix
out=$("$MIXC" fmt /tmp/mix_done_neg.mix)
echo "$out" | grep -q 'done -1' \
    && ok "fmt: unary minus after done stays glued" \
    || ng "fmt: unary minus after done stays glued"
rm -f /tmp/mix_done_neg.mix

# --- --check on a non-formatted file: exits 1, prints path ---
echo 'main()
    x  =  1+2
    print(x)' > /tmp/mix_unfmt.mix
out=$("$MIXC" fmt --check /tmp/mix_unfmt.mix 2>&1); rc=$?
[ "$rc" = "1" ] && ok "fmt --check: exits 1 on diffs" || ng "fmt --check: exits 1 on diffs"
echo "$out" | grep -q "mix_unfmt.mix" && ok "fmt --check: prints path" || ng "fmt --check: prints path"

# --- --check on an already-formatted file: silent + exit 0 ---
"$MIXC" fmt -w /tmp/mix_unfmt.mix
out=$("$MIXC" fmt --check /tmp/mix_unfmt.mix 2>&1); rc=$?
[ "$rc" = "0" ] && ok "fmt --check: exits 0 when clean" || ng "fmt --check: exits 0 when clean"
[ -z "$out" ] && ok "fmt --check: silent when clean" || ng "fmt --check: silent when clean"

# --- --diff prints unified-style output ---
echo 'main()
    x  =  1+2' > /tmp/mix_unfmt.mix
out=$("$MIXC" fmt --diff /tmp/mix_unfmt.mix 2>&1)
echo "$out" | grep -q '^---' && ok "fmt --diff: emits diff header" || ng "fmt --diff: emits diff header"
echo "$out" | grep -q '^-    x  =  1+2' && ok "fmt --diff: shows removal" || ng "fmt --diff: shows removal"
echo "$out" | grep -q '^+    x = 1 + 2' && ok "fmt --diff: shows addition" || ng "fmt --diff: shows addition"
rm -f /tmp/mix_unfmt.mix

# --- Recursive directory walk: --check reports each unformatted file ---
mkdir -p /tmp/mix_fmt_dir/sub
echo 'main()
    print(1+2)' > /tmp/mix_fmt_dir/a.mix
echo 'main()
    print(3+4)' > /tmp/mix_fmt_dir/sub/b.mix
out=$("$MIXC" fmt --check /tmp/mix_fmt_dir/ 2>&1); rc=$?
[ "$rc" = "1" ] && ok "fmt: walks directories" || ng "fmt: walks directories"
echo "$out" | grep -q '/a.mix' && ok "fmt: reports top-level file" || ng "fmt: reports top-level file"
echo "$out" | grep -q '/sub/b.mix' && ok "fmt: recurses into subdirs" || ng "fmt: recurses into subdirs"
rm -rf /tmp/mix_fmt_dir

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

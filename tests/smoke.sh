#!/usr/bin/env sh
# Smoke test: build examples, run them, and check that invalid programs fail.
# Run from the repository root via `make test`.
set -eu

VM=./binvm
TMP=${TMPDIR:-/tmp}/binvm-test.$$
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP"

pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf 'ok   %s\n' "$1"
}

not_ok() {
    fail=$((fail + 1))
    printf 'FAIL %s\n' "$1"
    [ $# -gt 1 ] && printf '     %s\n' "$2"
}

# expect_output <name> <source string> <expected stdout>
expect_output() {
    printf '%s\n' "$2" > "$TMP/$1.txt"
    if ! "$VM" compile "$TMP/$1.txt" -o "$TMP/$1.bin" > /dev/null 2>&1; then
        not_ok "$1" "compile failed"
        return 0
    fi
    got=$("$VM" run "$TMP/$1.bin" 2>/dev/null || true)
    if [ "$got" = "$3" ]; then
        ok "$1"
    else
        not_ok "$1" "expected [$3] got [$got]"
    fi
}

# expect_fail <name> <source string>
expect_fail() {
    printf '%s\n' "$2" > "$TMP/$1.txt"
    if "$VM" validate "$TMP/$1.txt" > /dev/null 2>&1; then
        not_ok "$1" "expected rejection, but it validated"
    else
        ok "$1"
    fi
}

INC=00000100
OUT=00010000
HALT=11111111

# 1. 65 x INC, OUT, HALT -> 'A'
{
    i=0
    while [ $i -lt 65 ]; do
        echo "$INC"
        i=$((i + 1))
    done
    echo "$OUT"
    echo "$HALT"
} > "$TMP/inc_out.txt"
if "$VM" compile "$TMP/inc_out.txt" -o "$TMP/inc_out.bin" > /dev/null 2>&1 &&
   [ "$("$VM" run "$TMP/inc_out.bin")" = "A" ]; then
    ok "inc_out"
else
    not_ok "inc_out"
fi

# 2. shipped examples
if "$VM" compile examples/hello.txt -o "$TMP/hello.bin" > /dev/null 2>&1 &&
   [ "$("$VM" run "$TMP/hello.bin")" = "Hi!" ]; then
    ok "examples/hello.txt"
else
    not_ok "examples/hello.txt"
fi

if "$VM" compile examples/loop.txt -o "$TMP/loop.bin" > /dev/null 2>&1 &&
   [ "$("$VM" run "$TMP/loop.bin")" = "AAA" ]; then
    ok "examples/loop.txt"
else
    not_ok "examples/loop.txt"
fi

# 3. comments may contain arbitrary text, including UTF-8
printf '%s\n' '; 中英文 mixed text, 带标点! ;
00000100
11111111' > "$TMP/comment_utf8.txt"
if "$VM" validate "$TMP/comment_utf8.txt" > /dev/null 2>&1; then
    ok "comment_utf8"
else
    not_ok "comment_utf8"
fi

# 4. negative cases
expect_fail "invalid_encoding" "00000011
$HALT"
expect_fail "bits_split_by_space" "0000 0100
$HALT"
expect_fail "unmatched_jump" "01000000
$HALT"
expect_fail "missing_halt" "$INC"
expect_fail "unclosed_comment" "; never closed
$INC
$HALT"
expect_fail "letters_outside_comment" "abcd
$HALT"

# 5. IN with no input must fail cleanly instead of reading garbage
printf '%s\n%s\n' "00100000" "$HALT" > "$TMP/in.txt"
if "$VM" compile "$TMP/in.txt" -o "$TMP/in.bin" > /dev/null 2>&1 &&
   ! "$VM" run "$TMP/in.bin" < /dev/null > /dev/null 2>&1; then
    ok "in_eof"
else
    not_ok "in_eof"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

#!/bin/sh
# tests/check_seed.sh —— prove the self-compiling sinterc seed fixpoint
#
#   sh tests/check_seed.sh [sinter] [sinterc]
set -u
SINTER=${1:-${SINTER:-/tmp/cbin/sinter}}
SINTERC=${2:-${SINTERC:-/tmp/cbin/sinterc}}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${TMPDIR:-/tmp}/sinter_seed_$$
case "$SINTER" in /*) ;; *) SINTER="$PWD/$SINTER" ;; esac
case "$SINTERC" in /*) ;; *) SINTERC="$PWD/$SINTERC" ;; esac
[ -x "$SINTER" ] || { echo "错误: sinter 不可执行（$SINTER）"; exit 1; }
[ -x "$SINTERC" ] || { echo "错误: sinterc 不可执行（$SINTERC）"; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "skip: cc not found"; exit 0; }
mkdir -p "$WORK"
note() { printf '%s %s\n' "$1" "$2"; }
fail() { note FAIL "$1"; exit 1; }

if ! ( cd "$ROOT/src/seed" && SINTERC="$SINTERC" "$SINTER" build -j 2 ) >"$WORK/build.log" 2>&1; then
    fail "build seed"
fi
SEED="$ROOT/src/seed/build/cmake/sinterc"
[ -x "$SEED" ] || fail "seed binary missing"
RUNTIME="$ROOT/src/seed/src/sinter_runtime.c"
INC="$ROOT/src/seed/src"

cd "$ROOT/src/seed"
if ! "$SEED" src/main.sinter -o "$WORK/stage1.c" >"$WORK/s1.log" 2>&1; then
    fail "stage0 -> stage1 C"
fi
if ! cc -std=c99 -I "$INC" "$WORK/stage1.c" "$RUNTIME" -o "$WORK/stage1" -lm -pthread >"$WORK/s1cc.log" 2>&1; then
    fail "compile stage1"
fi
if ! "$WORK/stage1" src/main.sinter -o "$WORK/stage2.c" >"$WORK/s2.log" 2>&1; then
    fail "stage1 -> stage2 C"
fi
diff -q "$WORK/stage1.c" "$WORK/stage2.c" >/dev/null || fail "stage1/stage2 C differs"
note PASS "stage1/stage2 generated C identical"

if ! cc -std=c99 -I "$INC" "$WORK/stage2.c" "$RUNTIME" -o "$WORK/stage2" -lm -pthread >"$WORK/s2cc.log" 2>&1; then
    fail "compile stage2"
fi
if ! "$WORK/stage2" src/main.sinter -o "$WORK/stage3.c" >"$WORK/s3.log" 2>&1; then
    fail "stage2 -> stage3 C"
fi
diff -q "$WORK/stage2.c" "$WORK/stage3.c" >/dev/null || fail "stage2/stage3 C differs"
note PASS "stage2/stage3 generated C identical"

if ! "$WORK/stage2" tests/fib.sinter -o "$WORK/fib.c" >"$WORK/fib.log" 2>&1; then
    fail "stage2 compile fib"
fi
if ! cc -std=c99 -I "$INC" "$WORK/fib.c" "$RUNTIME" -o "$WORK/fib" -lm -pthread >"$WORK/fibcc.log" 2>&1; then
    fail "compile fib"
fi
"$WORK/fib"
rc=$?
[ "$rc" -eq 55 ] || fail "fib exit code $rc != 55"
note PASS "stage2 compiles and runs fib (exit 55)"

if ! "$WORK/stage2" tests/hello.sinter -o "$WORK/hello.c" >"$WORK/hello.log" 2>&1; then
    fail "stage2 compile hello"
fi
if ! cc -std=c99 -I "$INC" "$WORK/hello.c" "$RUNTIME" -o "$WORK/hello" -lm -pthread >"$WORK/hellocc.log" 2>&1; then
    fail "compile hello"
fi
"$WORK/hello" >"$WORK/hello.out"
diff -u "$ROOT/src/seed/tests/expected/hello.txt" "$WORK/hello.out" >/dev/null || fail "hello output"
note PASS "stage2 compiles and runs hello"

if ! "$WORK/stage2" tests/class.sinter -o "$WORK/class.c" >"$WORK/class.log" 2>&1; then
    fail "stage2 compile class"
fi
if ! cc -std=c99 -I "$INC" "$WORK/class.c" "$RUNTIME" -o "$WORK/class" -lm -pthread >"$WORK/classcc.log" 2>&1; then
    fail "compile class"
fi
"$WORK/class" >"$WORK/class.out"
diff -u "$ROOT/src/seed/tests/expected/class.txt" "$WORK/class.out" >/dev/null || fail "class output"
note PASS "stage2 compiles and runs class"

echo
echo "通过 5，失败 0"

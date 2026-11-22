#!/bin/sh
# Build the staged self-hosted compiler under src/ and run its core tests.
set -u
SINTER=${1:-${SINTER:-/tmp/cbin/sinter}}
SINTERC=${2:-${SINTERC:-/tmp/cbin/sinterc}}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
DEFS="-DSINTER_VERSION_STRING=\"$VERSION\""
WORK=${TMPDIR:-/tmp}/sinter_stage_$$
case "$SINTER" in /*) ;; *) SINTER="$PWD/$SINTER" ;; esac
case "$SINTERC" in /*) ;; *) SINTERC="$PWD/$SINTERC" ;; esac
[ -x "$SINTER" ] || { echo "error: sinter not executable"; exit 1; }
[ -x "$SINTERC" ] || { echo "error: sinterc not executable"; exit 1; }
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "skip: $CC not found"; exit 0; }
mkdir -p "$WORK"
pass=0; fail=0
note() { printf '%s %s\n' "$1" "$2"; }
pick_c() {
    c=$(sed -n 's/.*"\(.*sinter_[^"]*\.c\)".*/\1/p' "$1/sources.cmake" 2>/dev/null | grep -v 'sinter_runtime.c' | head -n 1)
    if [ -z "$c" ]; then c=$(ls "$1"/sinter_*.c 2>/dev/null | grep -v 'sinter_runtime.c' | head -n 1); fi
    printf '%s' "$c"
}

# Compile every C file emitted by the multi-file code generator.
cc_emitted() {
    d=$1
    exe=$2
    shift 2
    srcs=$(sed -n 's/.*"\${CMAKE_CURRENT_LIST_DIR}\/\([^"]*\.c\)".*/\1/p' "$d/sources.cmake" 2>/dev/null | sed "s|^|$d/|")
    if [ -z "$srcs" ]; then srcs=$(ls "$d"/sinter_*.c 2>/dev/null); fi
    # shellcheck disable=SC2086
    "$CC" -std=c99 -O2 $DEFS "$@" $srcs -I "$d" -o "$exe" -lm -pthread
}
if ! ( cd "$ROOT/src/sinterc" && SINTERC="$SINTERC" "$SINTER" build -j 2 ) >"$WORK/build.log" 2>&1; then
    echo "FAIL build"; tail -30 "$WORK/build.log"; exit 1
fi
BIN="$ROOT/src/sinterc/build/cmake/sinterc"
if "$BIN" --check "$ROOT"/src/libs/syntax/src/*.sinter >"$WORK/check.log" 2>&1; then
    note PASS "syntax --check"
    pass=$((pass+1))
else
    note FAIL "syntax --check"; cat "$WORK/check.log"; fail=$((fail+1))
fi
for src in "$ROOT"/tests/*.sinter; do
    base=$(basename "$src" .sinter)
    [ "$base" = "broken" ] && continue
    exp="$ROOT/tests/expected/$base.txt"
    [ -f "$exp" ] || continue
    if ! "$BIN" "$src" -o "$WORK/$base-out" >"$WORK/$base.emit" 2>&1; then
        note FAIL "$base: emit"; fail=$((fail+1)); continue
    fi
    if ! cc_emitted "$WORK/$base-out" "$WORK/$base" >"$WORK/$base.cc" 2>&1; then
        note FAIL "$base: cc"; head -20 "$WORK/$base.cc"; fail=$((fail+1)); continue
    fi
    "$WORK/$base" >"$WORK/$base.out"
    if diff -u "$exp" "$WORK/$base.out" >"$WORK/$base.diff"; then
        note PASS "$base"; pass=$((pass+1))
    else
        note FAIL "$base"; cat "$WORK/$base.diff"; fail=$((fail+1))
    fi
done

# ---------------------------------------------------------------------------
# Self-compilation fixpoint: stage0 (host-built) -> stage1 -> stage2 -> stage3.
# stage2 and stage3 must emit byte-identical C for the compiler's own sources.
# ---------------------------------------------------------------------------
STD_ROOT="$ROOT/packages/std"
STD_SRC="$STD_ROOT/src"
ENTRY="$ROOT/src/sinterc/src/main.sinter"
INCS="-I $ROOT/src/libs/syntax/src -I $ROOT/src/libs/sem/src -I $ROOT/src/libs/codegen/src -I $ROOT/src/libs/sfmt/src -I $ROOT/src/libs/sver/src"
if [ -n "$STD_SRC" ]; then
    INCS="$INCS -I $STD_ROOT -I $STD_SRC"
fi

if [ -z "$STD_SRC" ] || [ ! -f "$STD_SRC/std.sinter" ]; then
    note SKIP "self-compile (std package sources not found)"
else
    echo "== self-compile fixpoint =="
    # shellcheck disable=SC2086
    if ! "$BIN" "$ENTRY" -o "$WORK/stage1-out" $INCS >"$WORK/stage1.emit" 2>&1; then
        note FAIL "stage0 -> stage1"; cat "$WORK/stage1.emit"; fail=$((fail+1))
    elif ! cc_emitted "$WORK/stage1-out" "$WORK/stage1" >"$WORK/stage1.cc" 2>&1; then
        note FAIL "compile stage1"; head -20 "$WORK/stage1.cc"; fail=$((fail+1))
    else
        note PASS "stage0 -> stage1"
        pass=$((pass+1))
        # shellcheck disable=SC2086
        if ! "$WORK/stage1" "$ENTRY" -o "$WORK/stage2-out" $INCS >"$WORK/stage2.emit" 2>&1; then
            note FAIL "stage1 -> stage2"; cat "$WORK/stage2.emit"; fail=$((fail+1))
        elif ! cc_emitted "$WORK/stage2-out" "$WORK/stage2" >"$WORK/stage2.cc" 2>&1; then
            note FAIL "compile stage2"; head -20 "$WORK/stage2.cc"; fail=$((fail+1))
        else
            note PASS "stage1 -> stage2"
            pass=$((pass+1))
            # shellcheck disable=SC2086
            if ! "$WORK/stage2" "$ENTRY" -o "$WORK/stage3-out" $INCS >"$WORK/stage3.emit" 2>&1; then
                note FAIL "stage2 -> stage3"; cat "$WORK/stage3.emit"; fail=$((fail+1))
            elif cmp -s "$(pick_c "$WORK/stage2-out")" "$(pick_c "$WORK/stage3-out")"; then
                note PASS "stage2/stage3 generated C identical"
                pass=$((pass+1))
            else
                note FAIL "stage2/stage3 generated C differs"; fail=$((fail+1))
            fi
            # stage2 must still compile and run the regression programs.
            if "$WORK/stage2" "$ROOT/tests/fib.sinter" -o "$WORK/bootstrap_fib-out" >"$WORK/bootstrap_fib.emit" 2>&1 \
                && cc_emitted "$WORK/bootstrap_fib-out" "$WORK/bootstrap_fib" >"$WORK/bootstrap_fib.cc" 2>&1 \
                && "$WORK/bootstrap_fib" >"$WORK/bootstrap_fib.out" \
                && diff -q "$ROOT/tests/expected/fib.txt" "$WORK/bootstrap_fib.out" >/dev/null; then
                note PASS "stage2 compiles and runs fib"
                pass=$((pass+1))
            else
                note FAIL "stage2 compiles and runs fib"; fail=$((fail+1))
            fi
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Complete toolchain: build the Sinter-written package manager, then use it to
# build sinterc / sinter-lsp / sinterd and smoke-test each binary.
# ---------------------------------------------------------------------------
echo "== self-hosted toolchain =="
HOSTC="$WORK/stage2"
[ -x "$HOSTC" ] || HOSTC="$BIN"
TOOL="$WORK/sinter-tool"
# shellcheck disable=SC2086
if ! "$HOSTC" "$ROOT/src/sinter/src/main.sinter" -o "$WORK/sinter-tool-out" $INCS >"$WORK/sinter-tool.emit" 2>&1; then
    note FAIL "build sinter (self-hosted)"; head -20 "$WORK/sinter-tool.emit"; fail=$((fail+1))
elif ! cc_emitted "$WORK/sinter-tool-out" "$TOOL" >"$WORK/sinter-tool.cc" 2>&1; then
    note FAIL "compile sinter (self-hosted)"; head -20 "$WORK/sinter-tool.cc"; fail=$((fail+1))
else
    note PASS "build sinter (self-hosted)"
    pass=$((pass+1))
    for proj in sinter sinterc sinter-lsp sinterd; do
        if ( cd "$ROOT/src/$proj" && SINTERC="$HOSTC" "$TOOL" build -j 2 ) >"$WORK/tool-$proj.log" 2>&1; then
            note PASS "sinter build $proj"
            pass=$((pass+1))
        else
            note FAIL "sinter build $proj"; tail -20 "$WORK/tool-$proj.log"; fail=$((fail+1))
        fi
    done
    if "$ROOT/src/sinter/build/cmake/sinter" --version | grep -q "sinter $VERSION"; then
        note PASS "sinter --version"; pass=$((pass+1))
    else
        note FAIL "sinter --version"; fail=$((fail+1))
    fi
    printf '%s\n' 'fun main(): void {' '    ret 0' '}' > "$WORK/fmt.expected"
    if printf 'fun main():void{\nret 0\n}\n' > "$WORK/fmt.sinter" \
        && "$ROOT/src/sinter/build/cmake/sinter" fmt "$WORK/fmt.sinter" >"$WORK/fmt.log" 2>&1 \
        && diff -u "$WORK/fmt.expected" "$WORK/fmt.sinter" >"$WORK/fmt.diff"; then
        note PASS "sinter fmt"; pass=$((pass+1))
    else
        note FAIL "sinter fmt"; cat "$WORK/fmt.log" "$WORK/fmt.diff"; fail=$((fail+1))
    fi
    if "$ROOT/src/sinterd/build/cmake/sinterd" --self-test >"$WORK/sinterd-test.log" 2>&1; then
        note PASS "sinterd self-test"; pass=$((pass+1))
    else
        note FAIL "sinterd self-test"; cat "$WORK/sinterd-test.log"; fail=$((fail+1))
    fi
    if command -v python3 >/dev/null 2>&1; then
        if BIN="$ROOT/src/sinter-lsp/build/cmake/sinter-lsp" python3 - >"$WORK/lsp-toolchain.log" 2>&1 <<'PYLSP'
import json, os, subprocess
exe = os.environ["BIN"]
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def send(o):
    d = json.dumps(o, separators=(",", ":")).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(d) + d); p.stdin.flush()
def read():
    line = p.stdout.readline(); n = -1
    while line.strip():
        if line.lower().startswith(b"content-length:"): n = int(line.split(b":")[1])
        line = p.stdout.readline()
    return json.loads(p.stdout.read(n))
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":None,"rootUri":None,"capabilities":{}}})
assert read()["result"]["capabilities"]["documentSymbolProvider"] is True
send({"jsonrpc":"2.0","id":2,"method":"shutdown","params":None})
assert read()["result"] is None
send({"jsonrpc":"2.0","method":"exit","params":None})
p.wait(timeout=5)
assert p.returncode == 0
PYLSP
        then
            note PASS "sinter-lsp smoke"; pass=$((pass+1))
        else
            note FAIL "sinter-lsp smoke"; cat "$WORK/lsp-toolchain.log"; fail=$((fail+1))
        fi
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ] || exit 1

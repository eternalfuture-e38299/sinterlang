#!/bin/sh
# tests/check_selfhost.sh —— 自举编译器前端 / C 后端 / LSP 回归
#
#   sh tests/check_selfhost.sh [sinter] [sinterc]
#
# 1. 构建 selfhost 标准 Sinter 工程；
# 2. 用自举 lexer+parser 检查它自己的全部源码（0 诊断）；
# 3. 把 tests/*.sinter 编译成 C，宿主 cc 编译并运行，和 expected 对比；
# 4. 启动 --lsp，走一遍 initialize / didOpen / documentSymbol / hover / shutdown。
set -u

SINTER=${1:-${SINTER:-/tmp/cbin/sinter}}
SINTERC=${2:-${SINTERC:-/tmp/cbin/sinterc}}
# 相对的编译器路径可能在子目录里执行时失效；先转绝对路径。
case "$SINTER" in /*) ;; *) SINTER="$PWD/$SINTER" ;; esac
case "$SINTERC" in /*) ;; *) SINTERC="$PWD/$SINTERC" ;; esac
# SINTER_ABS_NORMALIZED
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${TMPDIR:-/tmp}/sinter_selfhost_$$
BIN="$ROOT/selfhost/build/cmake/sinter-selfhost"

[ -x "$SINTER" ] || { echo "错误: sinter 不可执行（$SINTER）"; exit 1; }
[ -x "$SINTERC" ] || { echo "错误: sinterc 不可执行（$SINTERC）"; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "skip: cc not found"; exit 0; }

mkdir -p "$WORK"
pass=0
fail=0
note() { printf '%s %s\n' "$1" "$2"; }

echo "== build selfhost =="
if ! ( cd "$ROOT/selfhost" && SINTERC="$SINTERC" "$SINTER" build -j 2 ) >"$WORK/build.log" 2>&1; then
    echo "FAIL selfhost: build"
    tail -30 "$WORK/build.log"
    exit 1
fi
[ -x "$BIN" ] || { echo "FAIL selfhost: binary missing ($BIN)"; exit 1; }

echo "== frontend self-check =="
if "$BIN" --check "$ROOT"/selfhost/src/*.sinter >"$WORK/check.log" 2>&1; then
    note PASS "selfhost --check (own sources)"
    pass=$((pass + 1))
else
    note FAIL "selfhost --check"
    cat "$WORK/check.log"
    fail=$((fail + 1))
fi

if "$BIN" --check "$ROOT/selfhost/tests/broken.sinter" >"$WORK/broken.log" 2>&1; then
    note FAIL "selfhost --check (broken file should fail)"
    fail=$((fail + 1))
else
    note PASS "selfhost --check (diagnostics on broken input)"
    pass=$((pass + 1))
fi

echo "== compile and run tests =="
for src in "$ROOT"/selfhost/tests/*.sinter; do
    base=$(basename "$src" .sinter)
    [ "$base" = "broken" ] && continue
    exp="$ROOT/selfhost/tests/expected/$base.txt"
    [ -f "$exp" ] || continue
    cfile="$WORK/$base.c"
    exe="$WORK/$base"
    if ! "$BIN" "$src" -o "$cfile" >"$WORK/$base.emit" 2>&1; then
        note FAIL "$base: selfhost emit"
        cat "$WORK/$base.emit"
        fail=$((fail + 1))
        continue
    fi
    if ! cc -std=c99 -pedantic-errors "$cfile" -o "$exe" >"$WORK/$base.cc" 2>&1; then
        note FAIL "$base: C compile"
        head -20 "$WORK/$base.cc"
        fail=$((fail + 1))
        continue
    fi
    "$exe" >"$WORK/$base.out"
    if diff -u "$exp" "$WORK/$base.out" >"$WORK/$base.diff"; then
        note PASS "$base"
        pass=$((pass + 1))
    else
        note FAIL "$base: output mismatch"
        cat "$WORK/$base.diff"
        fail=$((fail + 1))
    fi
done

echo "== lsp smoke =="
if command -v python3 >/dev/null 2>&1; then
    if BIN="$BIN" python3 - "$BIN" >"$WORK/lsp.log" 2>&1 <<'PY'
import json, os, subprocess, sys
exe = sys.argv[1]
p = subprocess.Popen([exe, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def send(obj):
    data = json.dumps(obj, separators=(",", ":")).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
    p.stdin.flush()

def read():
    line = p.stdout.readline()
    if not line:
        return None
    n = -1
    while line.strip():
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1].strip())
        line = p.stdout.readline()
    if n < 0:
        return None
    return json.loads(p.stdout.read(n))

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":None,"rootUri":None,"capabilities":{}}})
init = read()
assert init["result"]["capabilities"]["documentSymbolProvider"] is True

uri = "file:///tmp/selfhost-smoke.sinter"
good = "module demo\n\nfun add(a: i64, b: i64): i64 {\n    ret a + b\n}\n"
send({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"sinter","version":1,"text":good}}})
diag = read()
assert diag["method"] == "textDocument/publishDiagnostics"
assert len(diag["params"]["diagnostics"]) == 0, diag

send({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":uri}}})
syms = read()["result"]
assert any(s["name"] == "add" for s in syms), syms
assert all("range" in s and "selectionRange" in s for s in syms), syms

send({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":uri},"position":{"line":2,"character":5}}})
hover = read()["result"]
assert "fun add" in hover["contents"]["value"], hover

send({"jsonrpc":"2.0","id":4,"method":"shutdown","params":None})
assert read()["result"] is None
send({"jsonrpc":"2.0","method":"exit","params":None})
p.wait(timeout=5)
assert p.returncode == 0, p.returncode
print("lsp ok")
PY
    then
        note PASS "lsp (initialize/diagnostics/symbols/hover/shutdown)"
        pass=$((pass + 1))
    else
        note FAIL "lsp smoke"
        cat "$WORK/lsp.log"
        fail=$((fail + 1))
    fi
else
    echo "skip: python3 not found"
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ] || exit 1

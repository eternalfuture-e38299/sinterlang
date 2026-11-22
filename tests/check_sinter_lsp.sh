#!/bin/sh
# Build src/sinter-lsp and smoke-test initialize/diagnostics/symbols/hover.
set -u
SINTER=${1:-${SINTER:-/tmp/cbin/sinter}}
SINTERC=${2:-${SINTERC:-/tmp/cbin/sinterc}}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case "$SINTER" in /*) ;; *) SINTER="$PWD/$SINTER" ;; esac
case "$SINTERC" in /*) ;; *) SINTERC="$PWD/$SINTERC" ;; esac
[ -x "$SINTER" ] || { echo "error: sinter not executable"; exit 1; }
[ -x "$SINTERC" ] || { echo "error: sinterc not executable"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "skip: python3 not found"; exit 0; }

if ! ( cd "$ROOT/src/sinter-lsp" && SINTERC="$SINTERC" "$SINTER" build -j 2 ) >/tmp/sinter_lsp_build.log 2>&1; then
    echo "FAIL build"; tail -20 /tmp/sinter_lsp_build.log; exit 1
fi
BIN="$ROOT/src/sinter-lsp/build/cmake/sinter-lsp"
[ -x "$BIN" ] || { echo "FAIL binary missing"; exit 1; }

BIN="$BIN" python3 - "$BIN" <<'PY'
import json, os, subprocess, sys
p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def send(obj):
    data = json.dumps(obj, separators=(",", ":")).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data); p.stdin.flush()
def read():
    line = p.stdout.readline()
    if not line: return None
    n = -1
    while line.strip():
        if line.lower().startswith(b"content-length:"): n = int(line.split(b":")[1].strip())
        line = p.stdout.readline()
    if n < 0: return None
    return json.loads(p.stdout.read(n))
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":None,"rootUri":None,"capabilities":{}}})
assert read()["result"]["capabilities"]["documentSymbolProvider"] is True
uri = "file:///tmp/sinter-lsp-test.sinter"
good = "module demo\n\nfun add(a: i64, b: i64): i64 {\n    ret a + b\n}\n"
send({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"sinter","version":1,"text":good}}})
assert read()["params"]["diagnostics"] == []
send({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":uri}}})
assert any(s["name"] == "add" for s in read()["result"])
send({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":uri},"position":{"line":2,"character":5}}})
assert "fun add" in read()["result"]["contents"]["value"]
send({"jsonrpc":"2.0","id":4,"method":"shutdown","params":None})
assert read()["result"] is None
send({"jsonrpc":"2.0","method":"exit","params":None})
p.wait(timeout=5)
assert p.returncode == 0
print("sinter-lsp ok")
PY

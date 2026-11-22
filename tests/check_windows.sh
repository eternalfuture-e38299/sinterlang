#!/bin/sh
# tests/check_windows.sh —— 用 MinGW 交叉编译 std 测试，再用 Wine 运行。
#
#   sh tests/check_windows.sh [sinterc]
#
# 要求：x86_64-w64-mingw32-gcc、wine 在 PATH 中。
# 只走「Sinter -> C -> MinGW exe -> Wine」，不依赖 CMake；
# 另有一个 CMake 交叉工具链文件：tool/mingw-x86_64.cmake。
set -u

SINTERC=${1:-${SINTERC:-/tmp/cbin/sinterc}}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${TMPDIR:-/tmp}/sinter_win_$$
PATH=/usr/bin:/bin:$PATH
export PATH

for tool in x86_64-w64-mingw32-gcc wine; do
    command -v "$tool" >/dev/null 2>&1 || { echo "skip: $tool not found"; exit 0; }
done
[ -x "$SINTERC" ] || { echo "error: sinterc not executable ($SINTERC)"; exit 1; }

mkdir -p "$WORK"
cp -r "$ROOT/tests/windows/stdtest/." "$WORK/"
rm -rf "$WORK/build" "$WORK/gen"

if ! "$SINTERC" "$WORK/src/main.sinter" -o "$WORK/gen" \
        -I "$ROOT/packages/std/src" -I "$ROOT/packages/std" \
        -I "$ROOT/packages/net/src" -I "$ROOT/packages/net" >"$WORK/sinterc.log" 2>&1; then
    echo "FAIL: sinterc"
    cat "$WORK/sinterc.log"
    exit 1
fi

if ! x86_64-w64-mingw32-gcc -std=gnu99 "$WORK"/gen/*.c -I "$WORK/gen" \
        -o "$WORK/stdtest.exe" -lm -lws2_32 >"$WORK/gcc.log" 2>&1; then
    echo "FAIL: mingw compile"
    head -40 "$WORK/gcc.log"
    exit 1
fi

# Wine prints fixme/err lines on stderr; keep the program's own output.
( cd "$WORK" && WINEDEBUG=-all wine ./stdtest.exe 2>wine.err ) |
    tr -d '\r' >"$WORK/got.txt"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL: wine exited $rc"
    tail -10 "$WORK/wine.err"
    exit 1
fi

if ! diff -u "$ROOT/tests/windows/stdtest/expect.txt" "$WORK/got.txt"; then
    echo "FAIL: output mismatch (work dir: $WORK)"
    exit 1
fi

echo "PASS windows stdtest ($WORK/stdtest.exe)"

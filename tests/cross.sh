#!/bin/sh
# Cross-build the self-hosted Sinter toolchain with Zig.
#
#   sh tests/cross.sh [target ...]
#
# TARGETS default: x86_64-linux-gnu aarch64-linux-gnu x86_64-macos-none
#                  aarch64-macos-none x86_64-windows-gnu aarch64-windows-gnu
#
# SINTER        self-hosted sinter frontend used to drive the build
# SINTERC       host sinterc used to emit C (it is not the cross binary)
# SINTER_ZIG    zig executable (default: zig)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")
ZIG=${SINTER_ZIG:-zig}
TOOL=${SINTER:-$ROOT/src/sinter/build/cmake/sinter}
HOSTC=${SINTERC:-$ROOT/src/sinterc/build/cmake/sinterc}
TARGETS=${SINTER_TARGETS:-"x86_64-linux-gnu aarch64-linux-gnu x86_64-macos-none aarch64-macos-none x86_64-windows-gnu aarch64-windows-gnu"}
if [ "$#" -gt 0 ]; then TARGETS="$*"; fi

command -v "$ZIG" >/dev/null 2>&1 || { echo "error: zig not found: $ZIG"; exit 1; }
[ -x "$TOOL" ] || { echo "error: self-hosted sinter not executable: $TOOL"; exit 1; }
[ -x "$HOSTC" ] || { echo "error: host sinterc not executable: $HOSTC"; exit 1; }

WORK=${TMPDIR:-/tmp}/sinter_cross_$$
mkdir -p "$WORK"
printf 'Sinter %s cross build with %s %s\n' "$VERSION" "$ZIG" "$("$ZIG" version)"

for t in $TARGETS; do
    echo "== $t =="
    for p in sinter sinterc sinterd sinter-lsp; do
        log="$WORK/$t-$p.log"
        if (cd "$ROOT/src/$p" \
            && SINTERC="$HOSTC" SINTER_TARGET="$t" SINTER_ZIG="$ZIG" \
               SINTER_BUILD_DIR="build/cross/$t" "$TOOL" build -j 4) >"$log" 2>&1; then
            printf '  [ OK ] %s\n' "$p"
        else
            printf '  [FAIL] %s\n' "$p"
            tail -30 "$log"
            exit 1
        fi
    done
done

echo "cross build complete:"
for t in $TARGETS; do
    echo "  $t:"
    for p in sinter sinterc sinterd sinter-lsp; do
        d="$ROOT/src/$p/build/cross/$t"
        f=$(ls "$d/$p" "$d/$p.exe" 2>/dev/null | head -n 1 || true)
        [ -n "$f" ] && echo "    $f"
    done
done

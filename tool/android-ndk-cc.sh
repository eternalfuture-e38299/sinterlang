#!/bin/sh
# Android NDK clang wrapper for `sinter build`.
#
# `sinter build` sets CMAKE_C_COMPILER_ARG1=cc whenever SINTER_TARGET is set,
# because the default cross compiler is `zig cc`. NDK clang does not accept
# that extra `cc` argument, so this wrapper drops it and forwards everything
# else to the NDK clang binary.
set -eu

NDK=${ANDROID_NDK_HOME:-/home/eternalfuture/Android/Sdk/ndk/28.2.13676358}
HOST=${ANDROID_NDK_HOST:-linux-x86_64}
CLANG="$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang"

if [ ! -x "$CLANG" ]; then
    echo "error: NDK clang not found: $CLANG" >&2
    exit 1
fi

if [ "${1:-}" = "cc" ]; then
    shift
fi

exec "$CLANG" "$@"

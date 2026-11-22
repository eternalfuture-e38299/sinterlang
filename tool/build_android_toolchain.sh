#!/bin/sh
# 构建 Android (arm64-v8a / bionic) 版 sinter 工具链，并放进 Flutter 工程的 jniLibs。
#
# 背景（为什么不是一句 cmake 就行）：
#   1. NDK 只发 linux-x86_64 / darwin-x86_64 宿主的 prebuilt 工具链，
#      而这个仓库的构建环境是手机上的 Alpine aarch64——NDK 自带的 clang
#      在这里根本执行不了。所以用「宿主 clang + NDK sysroot」：
#      --target=aarch64-linux-android24 + NDK 的头文件/库。
#   2. 发行版 clang 会硬塞一个 -lssp_nonshared（musl 补丁），Android 上没有，
#      这里放一个空归档把它喂饱。
#   3. Android 10+ 禁止在 home 目录 execve，只有 APK 解出来的 `lib/` 例外。
#      所以四个可执行文件必须改名成 lib*.so 才会被解到 nativeLibraryDir。
#
# 用法：
#   tool/build_android_toolchain.sh            # 增量构建并同步
#   tool/build_android_toolchain.sh --clean    # 删掉 build-ndk 重新来
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-/opt/android-ndk-r28c}
API=${ANDROID_API:-24}
BUILD="$ROOT/build-ndk"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$PREBUILT/sysroot"
RT="$PREBUILT/lib/clang/19/lib/linux"
JNI="$ROOT/sinter-studio/android/app/src/main/jniLibs/arm64-v8a"

if [ ! -d "$NDK" ]; then
    echo "找不到 NDK：$NDK（用 ANDROID_NDK_HOME 指定）" >&2
    exit 1
fi
if ! command -v clang++ >/dev/null 2>&1; then
    echo "找不到 clang++：apk add clang lld" >&2
    exit 1
fi

# ---- 1. 宿主 clang 的资源目录（补上 Android 目标缺的 builtins / libunwind）----
CLANG_VER=$(clang++ -dumpversion | cut -d. -f1)
RT_DIR=/opt/ndk-rt/lib/clang/$CLANG_VER
mkdir -p "$RT_DIR/lib/aarch64-unknown-linux-android$API" /opt/ndk-rt/lib
ln -sfn "$(clang++ -print-resource-dir)/include" "$RT_DIR/include"
ln -sfn "$RT/libclang_rt.builtins-aarch64-android.a" \
    "$RT_DIR/lib/aarch64-unknown-linux-android$API/libclang_rt.builtins.a"
ln -sfn "$RT/aarch64/libunwind.a" \
    "$RT_DIR/lib/aarch64-unknown-linux-android$API/libunwind.a"
# 发行版 clang 的 musl 补丁会无条件加 -lssp_nonshared；给个空归档
[ -f /opt/ndk-rt/lib/libssp_nonshared.a ] || (cd /opt/ndk-rt/lib && ar rcs libssp_nonshared.a)

# ---- 2. 构建 ----
if [ "${1:-}" = "--clean" ]; then
    rm -rf "$BUILD"
fi
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tool/android-arm64.cmake" \
    -DSINTER_ANDROID_RESOURCE_DIR="$RT_DIR" \
    -DANDROID_API="$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSINTER_BUILD_TESTS=OFF
cmake --build "$BUILD" --target sinterc sinter sinter-lsp sinterd -j"$(nproc 2>/dev/null || echo 4)"

# ---- 3. 同步到 jniLibs（改名的原因见文件头第 3 条）----
#
# 先 strip：带符号的 sinterc 是 8MB，strip 后不到 1MB。四个加起来从 27MB
# 降到 4MB 左右——APK 体积几乎全部由它们决定。
mkdir -p "$JNI"
strip_one() {
    cp "$1" "$2"
    strip --strip-all "$2"
    chmod 755 "$2"
}
strip_one "$BUILD/bin/sinterc"    "$JNI/libsinter_tool_sinterc.so"
strip_one "$BUILD/bin/sinter"       "$JNI/libsinter_tool_sinter.so"
strip_one "$BUILD/bin/sinter-lsp" "$JNI/libsinter_tool_lsp.so"
strip_one "$BUILD/bin/sinterd"    "$JNI/libsinter_tool_sinterd.so"

# ---- 4. 打包独立库：std / net / zip 各一个 zip ----
python3 - "$ROOT" <<'PY'
import pathlib, sys, zipfile
base = pathlib.Path(sys.argv[1])
outdir = base / 'sinter-studio/assets/sinter'
outdir.mkdir(parents=True, exist_ok=True)
for name in ('std', 'net', 'zip', 'algo', 'json', 'toml'):
    root = base / 'packages' / name
    out = outdir / (name + '.zip')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(root.rglob('*')):
            if f.is_file():
                z.write(f, f.relative_to(root).as_posix())
    print(name + '.zip', out.stat().st_size, 'bytes')
PY

# ---- 5. 验收：架构、解释器、动态依赖 ----
echo
echo "== 产物校验 =="
for f in "$JNI"/libsinter_tool_*.so; do
    arch=$(readelf -h "$f" | awk -F: '/Machine/ {gsub(/ /,"",$2); print $2}')
    interp=$(readelf -p .interp "$f" 2>/dev/null | awk 'NR==3 {print $2}')
    needed=$(readelf -d "$f" | awk -F'[][]' '/NEEDED/ {print $2}' | tr '\n' ' ')
    size=$(wc -c < "$f")
    printf '  %-42s %-8s %-18s %6s B  NEEDED: %s\n' \
        "$(basename "$f")" "$arch" "${interp:-<none>}" "$size" "$needed"
    case "$needed" in
        *libc++*|*libunwind*) echo "    ✗ 仍依赖外部 C++ 运行时" >&2; exit 1 ;;
    esac
done
echo
echo "完成。APK 里会出现在 lib/arm64-v8a/ 下，运行时从 nativeLibraryDir 执行。"
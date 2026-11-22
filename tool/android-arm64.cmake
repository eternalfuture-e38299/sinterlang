# Android (arm64-v8a / bionic) 交叉编译工具链
#
# 为什么不用 NDK 自带的 build/cmake/android.toolchain.cmake：
# 那个文件会把编译器指向 `$NDK/toolchains/llvm/prebuilt/<host>/bin/clang++`，
# 而 NDK 官方只发布 linux-x86_64 / darwin-x86_64 宿主包——在 arm64 的机器
# （本项目的构建环境就是手机上的 Alpine aarch64）上那个二进制根本执行不了。
#
# 所以这里改用「本机 clang + NDK sysroot」：
#   * clang 本身就是交叉编译器，`--target=aarch64-linux-android24` 就够了；
#   * 头文件与库全部来自 NDK sysroot（bionic + libc++），所以产物仍然是
#     货真价实的 NDK 目标（AArch64 + /system/bin/linker64 + libc.so）；
#   * `-nostdlib++ -lc++_static` 静态链接 libc++，避免 APK 里再多带一个 .so。
#
# 用法：
#   cmake -S . -B build-ndk -G Ninja -DCMAKE_TOOLCHAIN_FILE=tool/android-arm64.cmake
#
# 依赖（由 tool/build_android_toolchain.sh 准备）：
#   /opt/ndk-rt/lib/clang/<ver>          clang 资源目录（include 指向宿主 clang，
#                                        lib 指向 NDK 的 builtins / libunwind）
#   /opt/ndk-rt/lib/libssp_nonshared.a   空归档，抵消发行版 clang 的 musl 补丁

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}" CACHE PATH "Android NDK 根目录")
if(NOT ANDROID_NDK)
    set(ANDROID_NDK "/opt/android-ndk-r28c")
endif()
set(ANDROID_API 24 CACHE STRING "minSdk / bionic API level")

set(ANDROID_PREBUILT "${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64")
set(ANDROID_SYSROOT "${ANDROID_PREBUILT}/sysroot")
set(ANDROID_TRIPLE "aarch64-linux-android${ANDROID_API}")

set(CMAKE_C_COMPILER "${SINTER_ANDROID_CC}" CACHE FILEPATH "")
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "clang")
endif()
set(CMAKE_CXX_COMPILER "${SINTER_ANDROID_CXX}" CACHE FILEPATH "")
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

# clang 资源目录：宿主 clang 的版本号不适合写死，构建脚本会传
# -DSINTER_ANDROID_RESOURCE_DIR=...；缺省时用宿主 clang 自己的资源目录。
if(SINTER_ANDROID_RESOURCE_DIR)
    set(ANDROID_RESOURCE_FLAG "-resource-dir=${SINTER_ANDROID_RESOURCE_DIR}")
endif()

string(JOIN " " ANDROID_COMMON_FLAGS
        "--target=${ANDROID_TRIPLE}"
        "--sysroot=${ANDROID_SYSROOT}"
        "-fuse-ld=lld"
        "-fno-stack-protector"
        "-fPIE")

set(CMAKE_C_FLAGS "${ANDROID_COMMON_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${ANDROID_COMMON_FLAGS} ${ANDROID_RESOURCE_FLAG} -nostdlib++" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "-pie" CACHE STRING "" FORCE)

# 静态 libc++：NDK 的 libc++_static.a 已含 libc++abi，但仍显式带上，
# 避免宿主 clang 版本差异导致的符号缺口。
string(JOIN " " ANDROID_LIB_DIRS
        "-L${ANDROID_SYSROOT}/usr/lib/aarch64-linux-android/${ANDROID_API}"
        "-L${ANDROID_SYSROOT}/usr/lib/aarch64-linux-android"
        "-L/opt/ndk-rt/lib")
set(CMAKE_CXX_STANDARD_LIBRARIES
        "${ANDROID_LIB_DIRS} -lc++_static -lc++abi -lm -ldl -lc"
        CACHE STRING "" FORCE)
set(CMAKE_C_STANDARD_LIBRARIES "${ANDROID_LIB_DIRS} -lm -ldl -lc" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${ANDROID_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# 自举包管理器与构建前端

> 中文在前，英文在后。本文描述 [`../src/sinter/`](../src/sinter/) —— 用 Sinter 自己实现的
> 包管理器与构建前端。原初 C++ 实现只存在于历史分支：
> <https://github.com/eternalfuture-e38299/sinterlang/tree/bootstrap-compiler>。

## 中文

### 1. 准备

本机已验证的二进制位于 `/home/eternalfuture/bin/sinter/`：

```sh
BIN=/home/eternalfuture/bin/sinter
export SINTERC="$BIN/sinterc"
export CC=clang

"$BIN/sinter" --version     # sinter 1.0.0
"$BIN/sinter" --help
```

环境变量：

| 变量 | 用途 |
| --- | --- |
| `SINTERC` | 生成 C 的编译器；未设置时使用 PATH 中的 `sinterc` |
| `CC` | 宿主 C 编译器，默认 `cc`；本机验证用 `clang` |
| `SINTER_CMAKE` | 显式指定可用 cmake；未设置时自动探测 |
| `SINTER_TARGET` | 交叉目标三元组，使用 `zig cc -target` |
| `SINTER_ZIG` | 指定 Zig 可执行文件 |
| `SINTER_BUILD_DIR` | 覆盖构建目录，默认 `build/cmake` |
| `SINTER_HOME` | 已安装包根目录，默认 `~/.sinter/packages` |
| `SINTER_CSTD` / `SINTER_OPT` / `SINTER_JOBS` | 覆盖清单中的 C 标准、优化级别与并行数 |

CMake 探测顺序：`SINTER_CMAKE` → `~/.local/bin/cmake` → CLion 自带 cmake →
`/usr/local/bin/cmake` → PATH。候选必须输出 `cmake version ...` 且不能包含 `CMAKE_ROOT`
错误，因此本机坏掉的 `/usr/bin/cmake` 会被自动跳过。

### 2. 快速开始

```sh
BIN=/home/eternalfuture/bin/sinter
REPO=/home/eternalfuture/CLionProjects/cinder/Sinter

cd "$REPO/examples/hello"
SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build --run -j8
```

新建工程：

```sh
"$BIN/sinter" init myapp
cd myapp
SINTERC="$BIN/sinterc" "$BIN/sinter" build --run -j8
```

`init` 生成 `sinter.toml`、`src/main.sinter` 和 `version.txt`。模板中的 `std = "*"` 需要
`SINTER_HOME` 中已安装 std；本仓库自己的工程全部使用相对 `path:` 依赖，不依赖全局安装。

### 3. 命令

| 命令 | 说明 |
| --- | --- |
| `init [名字]` | 创建最小工程，默认名 `myapp` |
| `build [--run] [--debug\|--release] [--opt 0..3\|s\|z\|g] [-jN] [--cstd <std>]` | 生成 C 与 CMake 并构建；`--run` 随后运行；`-j8`、`-j 8`、`--jobs 8` 等价 |
| `emit-c [目录]` | 只生成 runtime、每模块 `.c/.h` 与源文件清单 |
| `emit-cmake [目录]` | 生成 `CMakeLists.txt` 与全部 C 源文件，供导出 |
| `tree` | 打印递归依赖树 |
| `install` | 解析版本并写 `sinter.lock`（不下载） |
| `add <包>[@版本]` / `remove <包>` | 在清单中增删依赖 |
| `list` / `search <关键词>` | 列出/搜索 `SINTER_HOME` 中的本地包 |
| `info` | 打印解析后的清单与 include 路径 |
| `clean` | 删除 `build/`，保留 `sinter.lock` |
| `fmt <文件>...` | 原地格式化 Sinter 源码 |
| `--version` / `-V` / `--help` / `-h` / `help` | 版本与帮助 |

`build` 流程：读清单 → 解析 `path:` / 已安装依赖 → 调用 `sinterc` 生成 C → 生成可移植
CMake → 选择可用 cmake 并并行构建。

### 4. `sinter.toml`

#### 4.1 示例

```toml
[package]
name = "demo"
version = "file:../version.txt"
entry = "src/main.sinter"
artifact = "exe"
description = "示例工程"

[dependencies]
std = { version = "1.5.2", source = "path:../../packages/std" }
syntax = { version = "1.0.0", source = "path:../libs/syntax" }

[build]
opt = "2"
cstd = "c99"

[link]
subdirs = ["libs/greet"]
libs = ["greet"]
includes = ["${CMAKE_CURRENT_SOURCE_DIR}/libs/greet/include"]
defs = ["DEMO_LINUX=1"]
options = ["-static-libgcc"]
```

- `artifact = "exe"` 产出可执行文件；`static` 产出 `lib<name>.a`；`header` 不产二进制。
- `source = "path:..."` 相对清单所在目录解析，绝对路径也支持。本仓库实际相对路径：
  - `src/<tool>/sinter.toml` → `../../packages/<pkg>` 与 `../libs/<lib>`
  - `src/libs/<lib>/sinter.toml` → `../../../packages/std` 与 `../<lib>`
  - `examples/<name>/sinter.toml` → `../../packages/std`
  - `tests/cases/<name>/sinter.toml` → `../../../packages/<pkg>`
- `target` 只在 `path:` 依赖本身带 CMake 工程时用于 `target_link_libraries`；省略则用依赖名。
- `[link]` 的 `subdirs` / `libs` / `includes` / `defs` / `options` 会写入生成的 CMake；
  `defs` 按名字去重，`options` 支持逗号参数如 `"-Wl,--gc-sections"`。
- `[link.android]`、`[link.mingw]`、`[[link.condition]]` 等条件块也支持。

#### 4.2 版本与文件引用

`version` 支持 `file:`，路径相对清单目录解析。自举工具统一指向 `src/version.txt`，例如
`src/sinterc/sinter.toml` 写 `file:../version.txt`，`src/libs/syntax/sinter.toml` 写
`file:../../version.txt`。仓库根 `VERSION` 是脚本读取的副本；两者内容均为 `1.0.0`。
若文件缺失，会回退到根 `VERSION`，再回退到 `SINTER_VERSION`。`build` 会把解析结果以
`-DSINTER_VERSION_STRING="..."` 传给生成的 C。

### 5. 依赖与包目录

```text
.
├── packages/             可分发包：std、algo、json、net、toml、zip
│   └── std/src/std/      std.string / std.list / std.hashmap / std.arrays / …
├── src/libs/             编译器自身库：syntax、sem、codegen、cfront、sfmt、sver
└── src/<tool>/           sinter、sinterc、sinterd、sinter-lsp
```

`collectIncludes` 会同时把每个 `path:` 依赖的根目录与 `src/` 加入 include 路径，所以
`import json` 与 `import algo.aes` 这类不同布局都能解析。`std` 只是 prelude，
`std.string`、`std.list` 等非 prelude 模块必须显式导入。

### 6. 可移植 CMake 与交叉构建

生成物包括 `sinter_runtime.c/.h`、每模块一个 `sinter_<module>.c/.h`、`sources.cmake` 与
`CMakeLists.txt`。所有路径基于 `${CMAKE_CURRENT_LIST_DIR}`；子构建放在
`${CMAKE_CURRENT_BINARY_DIR}/deps/...`。输出中**没有** `SINTER_ROOT` / `CMAKE_ROOT`
自定义根变量，也没有本机绝对路径，可以把生成目录拷到另一台机器再 configure。

含 `CMakeLists.txt` 的 `path:` 依赖通过 `add_subdirectory` 引入并自动链接。Release 构建在
Linux/Android 使用 `--strip-all`，macOS 使用 `-S`；Debug 保留符号。

```sh
SINTER_TARGET=x86_64-windows-gnu "$BIN/sinter" build -j8
SINTER_TARGET=aarch64-macos-none  "$BIN/sinter" build -j8
```

最终 C 编译/链接由 `zig cc -target` 完成；`tests/cross.sh` 默认覆盖 6 个目标 × 4 个工具。

### 7. 全平台发布包

`tool/build_release.sh` 会一次性构建 13 个目标并打包到仓库根的 `build/`：

```sh
sh tool/build_release.sh                  # 全部平台
sh tool/build_release.sh linux.x64 android.arm64   # 只构建指定目标
```

输出文件名为 `<platform>.<arch>.zip`，每个压缩包包含 `sinter`、`sinterc`、`sinterd`、
`sinter-lsp`、`LICENSE`、`VERSION` 和 `README.txt`。

| 压缩包 | Zig 目标 |
| --- | --- |
| `linux.x64.zip` | `x86_64-linux-gnu` |
| `linux.x86.zip` | `x86-linux-gnu` |
| `linux.arm64.zip` | `aarch64-linux-gnu` |
| `linux.arm.zip` | `arm-linux-gnueabihf` |
| `windows.x64.zip` | `x86_64-windows-gnu` |
| `windows.x86.zip` | `x86-windows-gnu` |
| `windows.arm64.zip` | `aarch64-windows-gnu` |
| `macos.x86.zip` | `x86_64-macos-none` |
| `macos.arm64.zip` | `aarch64-macos-none` |

Android 目标使用 NDK 28.2.13676358 的 clang，通过 `tool/android-ndk-cc.sh` 转发；
默认 NDK 路径为 `/home/eternalfuture/Android/Sdk/ndk/28.2.13676358`，可用
`ANDROID_NDK_HOME` 覆盖。

| 压缩包 | NDK 目标 |
| --- | --- |
| `android.arm64.zip` | `aarch64-linux-android24` |
| `android.arm.zip` | `armv7a-linux-androideabi24` |
| `android.x64.zip` | `x86_64-linux-android24` |
| `android.x86.zip` | `i686-linux-android24` |

Android 目标不链接独立的 `pthread`（bionic 已提供），生成的 CMake 会改用 `m dl`。

### 8. 排错

- `import not found: std` —— 清单 `path:` 相对层级不对，或工程被单独复制到临时目录时丢了
  `packages/`。`tests/check.sh` 会保留相对层级并链接 `packages/`。
- `Could not find CMAKE_ROOT` —— 系统 cmake 坏了；设置 `SINTER_CMAKE` 指向可用版本，或依靠
  自动探测（本机为 `~/.local/bin/cmake`）。
- `sinterc failed` —— 查看上方 `line:col` 诊断；当前后端不支持的语法见
  [`LANGUAGE.md`](LANGUAGE.md)。
- `version is empty` —— `file:` 指向的版本文件不存在，且没有根 `VERSION` / `SINTER_VERSION`
  兜底。

---

## English

### 1. Setup

Validated binaries are in `/home/eternalfuture/bin/sinter/`:

```sh
BIN=/home/eternalfuture/bin/sinter
export SINTERC="$BIN/sinterc"
export CC=clang

"$BIN/sinter" --version     # sinter 1.0.0
"$BIN/sinter" --help
```

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SINTERC` | compiler used to emit C; defaults to `sinterc` on PATH |
| `CC` | host C compiler (default `cc`; `clang` used for validation here) |
| `SINTER_CMAKE` | explicit working cmake; otherwise auto-detected |
| `SINTER_TARGET` | cross triple, uses `zig cc -target` |
| `SINTER_ZIG` | explicit Zig executable |
| `SINTER_BUILD_DIR` | build directory override (default `build/cmake`) |
| `SINTER_HOME` | installed package root, default `~/.sinter/packages` |
| `SINTER_CSTD` / `SINTER_OPT` / `SINTER_JOBS` | override manifest C standard, opt level and parallelism |

CMake detection order: `SINTER_CMAKE` → `~/.local/bin/cmake` → CLion-bundled cmake →
`/usr/local/bin/cmake` → PATH. A candidate must print `cmake version ...` and must not contain a
`CMAKE_ROOT` error, so the broken `/usr/bin/cmake` on this machine is skipped.

### 2. Quick start

```sh
BIN=/home/eternalfuture/bin/sinter
REPO=/home/eternalfuture/CLionProjects/cinder/Sinter

cd "$REPO/examples/hello"
SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build --run -j8
```

Create a new project:

```sh
"$BIN/sinter" init myapp
cd myapp
SINTERC="$BIN/sinterc" "$BIN/sinter" build --run -j8
```

`init` writes `sinter.toml`, `src/main.sinter` and `version.txt`. Its `std = "*"` requires an
installed std under `SINTER_HOME`; projects in this repo use relative `path:` dependencies and need
no global install.

### 3. Commands

| Command | Description |
| --- | --- |
| `init [name]` | create a minimal project; default name `myapp` |
| `build [--run] [--debug\|--release] [--opt 0..3\|s\|z\|g] [-jN] [--cstd <std>]` | emit C/CMake and build; `--run` then executes; `-j8`, `-j 8`, `--jobs 8` are equivalent |
| `emit-c [dir]` | emit runtime, per-module `.c/.h` and source list only |
| `emit-cmake [dir]` | emit `CMakeLists.txt` plus all C sources for export |
| `tree` | print the recursive dependency tree |
| `install` | resolve versions and write `sinter.lock` (no download) |
| `add <pkg>[@version]` / `remove <pkg>` | add/remove a dependency |
| `list` / `search <keyword>` | list/search local packages in `SINTER_HOME` |
| `info` | print the resolved manifest and include paths |
| `clean` | remove `build/`, keep `sinter.lock` |
| `fmt <file>...` | format Sinter sources in place |
| `--version` / `-V` / `--help` / `-h` / `help` | version and help |

`build` reads the manifest → resolves `path:`/installed dependencies → invokes `sinterc` to emit C
→ generates portable CMake → picks a working cmake and builds in parallel.

### 4. `sinter.toml`

#### 4.1 Example

```toml
[package]
name = "demo"
version = "file:../version.txt"
entry = "src/main.sinter"
artifact = "exe"
description = "demo project"

[dependencies]
std = { version = "1.5.2", source = "path:../../packages/std" }
syntax = { version = "1.0.0", source = "path:../libs/syntax" }

[build]
opt = "2"
cstd = "c99"

[link]
subdirs = ["libs/greet"]
libs = ["greet"]
includes = ["${CMAKE_CURRENT_SOURCE_DIR}/libs/greet/include"]
defs = ["DEMO_LINUX=1"]
options = ["-static-libgcc"]
```

- `artifact = "exe"` produces an executable; `static` produces `lib<name>.a`; `header` produces no binary.
- `source = "path:..."` is relative to the manifest directory; absolute paths also work. Actual
  relative paths in this repo:
  - `src/<tool>/sinter.toml` → `../../packages/<pkg>` and `../libs/<lib>`
  - `src/libs/<lib>/sinter.toml` → `../../../packages/std` and `../<lib>`
  - `examples/<name>/sinter.toml` → `../../packages/std`
  - `tests/cases/<name>/sinter.toml` → `../../../packages/<pkg>`
- `target` is only used for `target_link_libraries` when a `path:` dependency contains a CMake
  project; it defaults to the dependency name.
- `[link]` arrays `subdirs` / `libs` / `includes` / `defs` / `options` are written into generated
  CMake; `defs` are deduplicated by name and `options` preserve commas such as `"-Wl,--gc-sections"`.
- Conditional blocks such as `[link.android]`, `[link.mingw]` and `[[link.condition]]` are supported.

#### 4.2 Versions and file references

`version` supports `file:`, resolved relative to the manifest directory. Self-hosted tools point at
`src/version.txt`: `src/sinterc/sinter.toml` uses `file:../version.txt`, while
`src/libs/syntax/sinter.toml` uses `file:../../version.txt`. Repository-root `VERSION` is a copy
used by scripts; both contain `1.0.0`. If the file is missing, resolution falls back to root
`VERSION`, then `SINTER_VERSION`. The result is passed to generated C as
`-DSINTER_VERSION_STRING="..."`.

### 5. Dependencies and package directories

```text
.
├── packages/             distributable packages: std, algo, json, net, toml, zip
│   └── std/src/std/      std.string / std.list / std.hashmap / std.arrays / …
├── src/libs/             compiler libraries: syntax, sem, codegen, cfront, sfmt, sver
└── src/<tool>/           sinter, sinterc, sinterd, sinter-lsp
```

`collectIncludes` adds both the root and `src/` directory of each `path:` dependency to the include
path, so layouts such as `import json` and `import algo.aes` both resolve. `std` is only the
prelude; non-prelude modules such as `std.string` and `std.list` need explicit imports.

### 6. Portable CMake and cross builds

Generated output includes `sinter_runtime.c/.h`, one `sinter_<module>.c/.h` per module,
`sources.cmake` and `CMakeLists.txt`. All paths are based on `${CMAKE_CURRENT_LIST_DIR}`;
sub-builds use `${CMAKE_CURRENT_BINARY_DIR}/deps/...`. No `SINTER_ROOT` / `CMAKE_ROOT` custom root
variable or machine-local absolute path is emitted, so the generated directory can be copied before
configuring.

A `path:` dependency containing `CMakeLists.txt` is imported via `add_subdirectory` and linked
automatically. Release builds strip with `--strip-all` on Linux/Android and `-S` on macOS;
Debug keeps symbols.

```sh
SINTER_TARGET=x86_64-windows-gnu "$BIN/sinter" build -j8
SINTER_TARGET=aarch64-macos-none  "$BIN/sinter" build -j8
```

The final C compile/link is done by `zig cc -target`; `tests/cross.sh` covers six targets times
four tools by default.

### 7. Release bundles for all platforms

`tool/build_release.sh` builds all 13 targets and packages them under the repository-root `build/`
directory:

```sh
sh tool/build_release.sh                           # all platforms
sh tool/build_release.sh linux.x64 android.arm64   # selected targets
```

Files are named `<platform>.<arch>.zip`. Each archive contains `sinter`, `sinterc`, `sinterd`,
`sinter-lsp`, `LICENSE`, `VERSION` and `README.txt`.

| Archive | Zig target |
| --- | --- |
| `linux.x64.zip` | `x86_64-linux-gnu` |
| `linux.x86.zip` | `x86-linux-gnu` |
| `linux.arm64.zip` | `aarch64-linux-gnu` |
| `linux.arm.zip` | `arm-linux-gnueabihf` |
| `windows.x64.zip` | `x86_64-windows-gnu` |
| `windows.x86.zip` | `x86-windows-gnu` |
| `windows.arm64.zip` | `aarch64-windows-gnu` |
| `macos.x86.zip` | `x86_64-macos-none` |
| `macos.arm64.zip` | `aarch64-macos-none` |

Android targets use NDK 28.2.13676358 clang through `tool/android-ndk-cc.sh`. The default NDK path
is `/home/eternalfuture/Android/Sdk/ndk/28.2.13676358` and can be overridden with
`ANDROID_NDK_HOME`.

| Archive | NDK target |
| --- | --- |
| `android.arm64.zip` | `aarch64-linux-android24` |
| `android.arm.zip` | `armv7a-linux-androideabi24` |
| `android.x64.zip` | `x86_64-linux-android24` |
| `android.x86.zip` | `i686-linux-android24` |

Android targets do not link a separate `pthread` (bionic provides it); generated CMake uses
`m dl` instead.

### 8. Troubleshooting

- `import not found: std` — wrong relative `path:` level, or the project was copied to a temp
  directory without `packages/`. `tests/check.sh` preserves the relative level and links `packages/`.
- `Could not find CMAKE_ROOT` — the system cmake is broken; set `SINTER_CMAKE` to a working one or
  rely on auto-detection (`~/.local/bin/cmake` here).
- `sinterc failed` — inspect the `line:col` diagnostic above it; current backend boundaries are in
  [`LANGUAGE.md`](LANGUAGE.md).
- `version is empty` — the `file:` target is missing and there is no root `VERSION` /
  `SINTER_VERSION` fallback.

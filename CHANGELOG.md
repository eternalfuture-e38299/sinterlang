# 变更记录

> 中文在前，英文在后。格式参考 [Keep a Changelog](https://keepachangelog.com/)，
> 版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## 中文

### [Unreleased]

#### Added

- **仓库独立化**：本目录现在作为独立仓库使用。所有文档、命令与相对路径都以本目录为根。
- **完整文档重写**：`README.md` 与 `docs/` 下的语言、自举、包管理文档均以中文在前、英文在后重写；
  语言描述改为“具备多种特性的现代语言”，不再使用其他语言作类比。
- **编辑器检查可选化**：CLion 插件源码属于独立维护的插件工程；缺失时相关检查明确跳过，
  不影响编译器回归。
- **全平台发布包**：新增 `tool/build_release.sh` 与 `tool/android-ndk-cc.sh`，一次构建
  Linux（x64/x86/arm64/arm）、Windows（x64/x86/arm64）、macOS（x64/arm64）和
  Android（arm64/arm/x64/x86）共 13 个目标的四个工具，并打包为
  `build/<platform>.<arch>.zip`。Android 使用 NDK 28.2.13676358。

#### Changed

- **包目录重命名**：`sinterpackages/` → `packages/`。所有 `sinter.toml` 的 `path:` 依赖、
  测试脚本、文档与版权年份同步更新；版权年份统一为 **2026**。
- **原初编译器关系澄清**：最初的 C++ 原型仅作为本仓库的历史分支保留：
  <https://github.com/eternalfuture-e38299/sinterlang/tree/bootstrap-compiler>。
  当前开发、包与测试全部以本仓库为准。

### [1.0.0] - 2026-11-22

#### Added

- **完整自举工具链**：`sinterc`（编译器）、`sinter`（包管理器/构建前端）、`sinterd`（守护进程）、
  `sinter-lsp`（语言服务器）全部用 Sinter 实现，并由自己构建。
- **现代语言特性**：模块与导入、静态类型、类/数据类/结构体/枚举/接口、单态化泛型、
  扩展函数、lambda、默认/命名/可变参数、重载链接名、闭区间、`when` 分支、异常、RAII、
  `del`、`@no_raii` 与 C 互操作。
- **按模块拆分代码生成**：生成 `sinter_runtime.c/.h` 与每模块一个 `sinter_<module>.c/.h`，
  并写出 `sources.cmake`；`build` / `emit-c` / `emit-cmake` 不再依赖单个大 C 文件。
- **可移植 CMake**：生成的 `CMakeLists.txt` 只使用 `${CMAKE_CURRENT_LIST_DIR}` 与
  `${CMAKE_CURRENT_BINARY_DIR}`，不写入 `SINTER_ROOT`、`CMAKE_ROOT` 或本机绝对路径。
- **包管理器**：`sinter.toml` 支持相对 `path:` 依赖与 `file:` 版本引用；提供
  `build` / `init` / `emit-c` / `emit-cmake` / `tree` / `install` / `add` / `remove` /
  `list` / `search` / `info` / `clean` / `fmt`。
- **CMake 自动探测与并行构建**：跳过损坏的 cmake 安装，支持 `-j8`、`-j 8`、`--jobs 8`
  与 `SINTER_JOBS`。
- **Zig 交叉编译**：`SINTER_TARGET=<triple>` 覆盖 x86_64/aarch64 的 Linux、macOS、Windows
  共 6 个目标，四个工具均可构建。
- **源码格式化与 C 前端**：`sfmt` 支撑 `sinter fmt` 与 `sinterd format`；`cfront` 提供
  C99 词法/解析与 C 重排，服务于 `sinterd` 的 C 相关操作。
- **编辑器支持**：`sinter-lsp` 提供诊断、文档符号、hover 与 initialize/shutdown；
  `--self-test` 与真实 stdio 协议测试共同覆盖。

#### Fixed

- 普通代码生成阶段也会报告并拒绝解析/语义诊断，不再静默生成非法 C。
- 生成的 C 在包含 libc 头文件之前定义 `_POSIX_C_SOURCE`，修复 `struct addrinfo` 在严格 C 标准下不完整的问题。
- 没有显式 `module` 的源文件归入合成模块 `program`，旧式入口重新生成可用 `main`。
- 临时字符串提升避免跨迭代复用已释放缓冲区；空参数函数统一写成 `(void)`。
- Release 构建自动剥离符号；Debug 保留符号。
- Android 交叉构建不再链接独立的 `pthread`，生成的 CMake 改用 `m dl`。

### [历史阶段]

- 语言与工具从旧的机器名迁移到 Sinter：CLI、扩展名、清单、锁文件、环境变量、C 运行时前缀、
  CMake 目标与插件标识统一更新。
- 标准库拆成按模块显式导入的形式；独立包（std、algo、json、net、toml、zip）逐步迁到 `packages/`。
- 守护进程、LSP、格式化器与 C 前端从实验脚本演进为可自举的 Sinter 工程。

---

## English

### [Unreleased]

#### Added

- **Independent repository**: this directory is now used as a standalone repository. All docs,
  commands and relative paths are rooted here.
- **Complete documentation rewrite**: the README and the language, self-host and package docs in
  `docs/` are rewritten Chinese-first, English-second. The language is described as a modern
  language with a broad feature set, without analogies to other languages.
- **Optional editor checks**: CLion plugin sources belong to a separately maintained plugin
  project; when absent those checks are skipped explicitly and do not affect compiler regressions.
- **All-platform release bundles**: added `tool/build_release.sh` and `tool/android-ndk-cc.sh` to
  build all four tools for 13 targets — Linux (x64/x86/arm64/arm), Windows (x64/x86/arm64),
  macOS (x64/arm64) and Android (arm64/arm/x64/x86) — and package them as
  `build/<platform>.<arch>.zip`. Android uses NDK 28.2.13676358.

#### Changed

- **Package directory renamed**: `sinterpackages/` → `packages/`. All `path:` dependencies in
  `sinter.toml`, test scripts, docs and copyright years were updated; copyright year is now **2026**.
- **Original compiler relationship clarified**: the initial C++ prototype is kept only as a
  historical branch of this repository:
  <https://github.com/eternalfuture-e38299/sinterlang/tree/bootstrap-compiler>.
  Current development, packages and tests all live here.

### [1.0.0] - 2026-11-22

#### Added

- **Complete self-hosted toolchain**: `sinterc` (compiler), `sinter` (package manager/build frontend),
  `sinterd` (daemon) and `sinter-lsp` (language server) are all implemented in Sinter and built by it.
- **Modern language features**: modules and imports, static types, classes/data classes/structs/
  enums/interfaces, monomorphized generics, extension functions, lambdas, default/named/variadic
  parameters, overload link names, inclusive ranges, `when` branches, exceptions, RAII, `del`,
  `@no_raii` and C interop.
- **Per-module code generation**: emits `sinter_runtime.c/.h` plus one `sinter_<module>.c/.h` per
  module and writes `sources.cmake`; `build` / `emit-c` / `emit-cmake` no longer depend on one large C file.
- **Portable CMake**: generated `CMakeLists.txt` only uses `${CMAKE_CURRENT_LIST_DIR}` and
  `${CMAKE_CURRENT_BINARY_DIR}`; no `SINTER_ROOT`, `CMAKE_ROOT` or machine-local absolute paths.
- **Package manager**: `sinter.toml` supports relative `path:` dependencies and `file:` version
  references, with `build` / `init` / `emit-c` / `emit-cmake` / `tree` / `install` / `add` /
  `remove` / `list` / `search` / `info` / `clean` / `fmt`.
- **CMake detection and parallel builds**: skips broken cmake installations and supports `-j8`,
  `-j 8`, `--jobs 8` and `SINTER_JOBS`.
- **Zig cross builds**: `SINTER_TARGET=<triple>` covers six targets (x86_64/aarch64 Linux, macOS
  and Windows) for all four tools.
- **Formatting and C frontend**: `sfmt` backs `sinter fmt` and `sinterd format`; `cfront` provides
  C99 lexing/parsing and C reindentation for `sinterd` C operations.
- **Editor support**: `sinter-lsp` provides diagnostics, document symbols, hover and
  initialize/shutdown, covered by both self-tests and real stdio protocol tests.

#### Fixed

- Normal code generation now reports and rejects parse/semantic diagnostics instead of silently
  emitting invalid C.
- Generated C defines `_POSIX_C_SOURCE` before libc headers, fixing incomplete `struct addrinfo`
  under strict C standards.
- Files without an explicit `module` go into the synthetic `program` module, restoring usable
  `main` generation for legacy entry points.
- Temporary string promotion avoids reusing freed buffers across loop iterations; empty-parameter
  functions are emitted as `(void)`.
- Release builds strip symbols automatically; Debug keeps them.
- Android cross builds no longer link a separate `pthread`; generated CMake uses `m dl`.

### [Historical]

- The language and tools were renamed from the old machine name to Sinter: CLI, file extension,
  manifest, lockfile, environment variables, C runtime prefix, CMake targets and plugin identities.
- The standard library was split into explicitly imported modules; standalone packages
  (std, algo, json, net, toml, zip) were gradually moved under `packages/`.
- The daemon, LSP, formatter and C frontend evolved from experimental scripts into self-hosted
  Sinter projects.

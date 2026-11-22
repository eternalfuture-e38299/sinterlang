# Sinter

<p align="right">
  <img src="assets/icon.svg" alt="Sinter" width="88">
</p>

## 中文

### 定位

Sinter 是一门**现代静态类型编译语言**，也是一套完整、可自举的语言工具链。它把 `.sinter`
源码翻译成可读的 C（`sinter_<module>.c/.h` 与一个很小的 `sinter_runtime.c`），再交给
gcc、clang、CMake 或 Zig 完成最终构建。

设计目标不是“再发明一个运行时”，而是把现代语言的表达力放在源码层，把部署和互操作留在 C 层：

- **现代语言特性**：模块、静态类型、类与数据类、结构体、枚举、接口、泛型单态化、扩展函数、
  lambda、默认/命名/可变参数、区间、模式式分支、异常、RAII 与 C 互操作。
- **C 作为稳定 ABI**：生成的是普通 C 源码，不依赖专有虚拟机或 GC；可以接入现有 CMake 工程，
  也可以交叉编译到 Linux、macOS、Windows 等目标。
- **工具链自举**：编译器、包管理器、格式化器、守护进程和 LSP 全部用 Sinter 编写，并由自己构建。

### 解决了什么问题

1. **表达力与可部署性的矛盾**：源码层提供高级抽象，产物层只有 C；泛型在编译期单态化，
   类、异常和 RAII 被 lowering 成普通 C 结构与控制流。
2. **构建可移植性**：生成的 `CMakeLists.txt` 只使用 `${CMAKE_CURRENT_LIST_DIR}`，不写入
   `SINTER_ROOT`、`CMAKE_ROOT` 或本机绝对路径；生成目录可以整体拷走再 configure。
3. **环境鲁棒性**：`sinter build` 会自动探测可用 CMake，跳过报 `CMAKE_ROOT` 错误的坏安装，
   并把 `-j8`、`-j 8`、`--jobs 8` 直接传给 `cmake --build --parallel`。
4. **零全局依赖开发**：仓库内 `packages/` 提供标准库和独立包，所有 `sinter.toml` 使用相对
   `path:` 依赖；克隆后可立即构建工具和测试，不依赖 `SINTER_HOME` 预装包。
5. **可验证性**：生成 C 是人类可读的；回归测试同时检查运行输出、生成结构和自编译固定点。

### 适用场景与优势

| 场景 | 做法 |
| --- | --- |
| 独立命令行程序 | `sinter build --run -j8`，默认输出到 `build/cmake/` |
| 接入现有 C/C++ 工程 | `sinter emit-cmake`，把生成的 C 与 CMake 纳入外部工程 |
| 交叉编译 | `SINTER_TARGET=x86_64-windows-gnu sinter build -j8`，由 `zig cc -target` 完成最终构建 |
| 库与复用 | `packages/` 放标准库与独立包；`src/libs/` 放编译器自身库 |
| 编辑器支持 | `sinter-lsp` 提供诊断、符号、hover；`sinter fmt` 统一格式 |

优势可以概括为：**源码现代，产物朴素**。类型检查、泛型展开、可见性与资源生命周期尽量在编译期
解决；运行时只保留字符串、数组、分配和少量 ABI 胶水。

### 语言特性一览

Sinter 是具备多种特性的现代语言，当前 C 后端已支持：

- **模块系统**：`module` 声明与 `import`；`std` 是 prelude，其他模块需显式导入。
- **类型与变量**：`val` / `var`；`i8..u64`、`f32/f64`、`bool`、`char`、`str`、`ptr`、`cstr`；
  定长数组 `i64[3]`、指针 `*T`、引用 `&T`、可空 `T?`、`null` 与 `as` 转换。
- **表达式**：算术、比较、逻辑、位运算、复合赋值、三元 `?:`、字符串插值 `"${expr}"`、
  十进制及 `0x`/`0b`/`0o` 字面量、闭区间 `..`。
- **控制流**：`if`/`else`、`while`、`for (x in a..b)`、`break`、`continue`、`when` 语句。
- **函数式能力**：表达式体函数、`=>`、`ret`/`return`、默认参数、命名参数、可变参数、
  重载链接名、扩展函数、lambda 与函数类型参数。
- **面向对象与抽象**：`class`、`struct`、`data class`、`enum`、`interface`、`annotation class`、
  构造与析构、`self`/`this`、继承与虚表、`pub`/`priv` 可见性。
- **泛型**：泛型函数与泛型类在语义分析前单态化，无装箱与运行时类型擦除开销。
- **错误处理与资源**：`try`/`catch`/`finally`/`throw`、`~Name()` 析构、`del`、
  `@no_raii`，并带有基础重复删除与局部指针逃逸检查。
- **C 互操作**：`embed C { ... }`、`embed C header { ... }`、`bind C fun ...`。

当前边界见 [`docs/LANGUAGE.md`](docs/LANGUAGE.md)；可运行示例在 [`examples/`](examples/)。

### 仓库结构

```text
.
├── README.md              本文件（中英双语）
├── CHANGELOG.md           变更记录
├── LICENSE / assets/      MIT 许可证与图标
├── VERSION                脚本读取的版本（与 src/version.txt 一致）
├── docs/                  语言、自举、包管理与构建文档
├── examples/              hello、collections、broken 与 01_values…08_stdlib
├── packages/              std、algo、json、net、toml、zip 等可分发包
├── src/
│   ├── version.txt        自举工具与编译器库的统一版本源
│   ├── libs/              syntax / sem / codegen / cfront / sfmt / sver
│   ├── sinterc/           自举办编译器
│   ├── sinter/            自举包管理器与构建前端
│   ├── sinterd/           JSON-line 守护进程
│   └── sinter-lsp/        stdio 语言服务器
├── tests/                 check.sh、check_sinter.sh、cross.sh、cases/、windows/
└── tool/                  辅助脚本与 CMake toolchain
```

### 快速开始

已验证的二进制位于 `/home/eternalfuture/bin/sinter/`；从仓库根目录运行：

```sh
BIN=/home/eternalfuture/bin/sinter
REPO=$(pwd)

cd "$REPO/examples/hello"
SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build --run -j8
```

从源码重建四个工具：

```sh
BIN=/home/eternalfuture/bin/sinter
for p in sinterc sinter sinterd sinter-lsp; do
  (cd "src/$p" && SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build -j8)
  cp "src/$p/build/cmake/$p" "$BIN/$p"
done
```

验证：

```sh
BIN=/home/eternalfuture/bin/sinter

CC=clang sh tests/check.sh "$BIN/sinter" "$BIN/sinterc"
CC=clang sh tests/check_sinter.sh "$BIN/sinter" "$BIN/sinterc"
SINTER="$BIN/sinter" SINTERC="$BIN/sinterc" CC=clang sh tests/cross.sh
```

全平台发布包（13 个目标，输出到 `build/<platform>.<arch>.zip`）：

```sh
sh tool/build_release.sh
```

详见 [`docs/SELFHOST_PACKAGES.md`](docs/SELFHOST_PACKAGES.md) 的“全平台发布包”。

### 自举状态

| 组件 | 状态 |
| --- | --- |
| `src/libs/syntax` | 当前源码的词法器与递归下降解析器 |
| `src/libs/sem` | 多模块限定、声明与引用重写 |
| `src/libs/codegen` | C 后端：字符串 ABI、类/枚举/全局、单态化泛型、数组、RAII、异常、扩展函数、`embed C` |
| `src/libs/cfront` | C99 词法/解析与 C 格式化 |
| `src/libs/sfmt` / `sver` | Sinter 源码格式化 / 构建期版本 |
| `sinterc` | 自编译固定点：stage2 与 stage3 生成 C 逐字节一致 |
| `sinter` | 自举前端：`build` / `emit-c` / `emit-cmake` / `fmt` / `tree` / `install` / `add` / `remove` / `info` / `clean` |
| `sinter-lsp` | 诊断、文档符号、hover、initialize/shutdown |
| `sinterd` | JSON-line Unix socket 守护进程，`--self-test` 21 项通过 |

### 文档

| 文档 | 内容 |
| --- | --- |
| [`docs/LANGUAGE.md`](docs/LANGUAGE.md) | 语言特性、语法边界与可运行示例 |
| [`docs/SELFHOST.md`](docs/SELFHOST.md) | 自编译固定点、编译器分层与工具服务 |
| [`docs/SELFHOST_PACKAGES.md`](docs/SELFHOST_PACKAGES.md) | `sinter.toml`、相对依赖、可移植 CMake、交叉构建与排错 |

### 与原初编译器的关系

本仓库是 Sinter 语言的主仓库。最初的 C++ 原型编译器仅作为一个历史分支保留：

<https://github.com/eternalfuture-e38299/sinterlang/tree/bootstrap-compiler>

当前开发、文档、包与测试都以本仓库为准。

### 许可证

MIT，见 [`LICENSE`](LICENSE)。Copyright © 2026 Sinter contributors.

---

## English

Sinter is a **modern, statically typed compiled language** and a complete self-hosted toolchain.
It lowers `.sinter` sources into readable C (`sinter_<module>.c/.h` plus a small
`sinter_runtime.c`) and lets gcc, clang, CMake or Zig perform the final build.

The goal is not to invent another runtime. Modern-language expressiveness lives in the source;
deployment and interoperability stay in C.

- **Modern features**: modules, static types, classes and data classes, structs, enums,
  interfaces, monomorphized generics, extension functions, lambdas, default/named/variadic
  parameters, ranges, branch statements, exceptions, RAII and C interop.
- **C as a stable ABI**: output is ordinary C with no proprietary VM or GC. It fits existing CMake
  projects and cross-builds to Linux, macOS, Windows and more.
- **Self-hosted toolchain**: the compiler, package manager, formatter, daemon and LSP are all
  written in Sinter and built by Sinter.

### Problems it solves

1. **Expressiveness versus deployability**: high-level source abstractions become plain C.
   Generics are monomorphized; classes, exceptions and RAII are lowered to C structures and control flow.
2. **Portable builds**: generated `CMakeLists.txt` only uses `${CMAKE_CURRENT_LIST_DIR}`. No
   `SINTER_ROOT`, `CMAKE_ROOT` or machine-local absolute paths are emitted.
3. **Robust environments**: `sinter build` detects a working CMake, skips broken installations
   reporting `CMAKE_ROOT` errors and passes `-j8`, `-j 8` or `--jobs 8` to `cmake --build --parallel`.
4. **No global package prerequisite**: `packages/` contains the standard library and standalone
   packages; every `sinter.toml` uses relative `path:` dependencies. A fresh clone builds the tools
   and tests without preinstalled `SINTER_HOME` packages.
5. **Verifiability**: generated C is readable; regressions check runtime output, generated structure
   and the self-compilation fixpoint.

### Use cases and advantages

| Use case | Command |
| --- | --- |
| Standalone program | `sinter build --run -j8` (outputs under `build/cmake/`) |
| Existing C/C++ project | `sinter emit-cmake` and vendor the generated C/CMake |
| Cross build | `SINTER_TARGET=x86_64-windows-gnu sinter build -j8` via `zig cc -target` |
| Libraries and reuse | `packages/` for std and standalone packages; `src/libs/` for compiler libraries |
| Editor support | `sinter-lsp` for diagnostics/symbols/hover; `sinter fmt` for formatting |

In short: **modern at the source layer, plain at the artifact layer**. Type checking, generic
expansion, visibility and resource lifetimes are resolved at compile time; the runtime keeps only
strings, arrays, allocation and a small ABI shim.

### Language highlights

Sinter is a modern language with a broad feature set. The current C backend supports:

- **Modules**: `module` declarations and `import`; `std` is the prelude, other modules are explicit.
- **Types and variables**: `val` / `var`; `i8..u64`, `f32/f64`, `bool`, `char`, `str`, `ptr`, `cstr`;
  fixed arrays `i64[3]`, pointers `*T`, references `&T`, nullable `T?`, `null` and `as` casts.
- **Expressions**: arithmetic, comparison, logical and bitwise operators, compound assignment,
  ternary `?:`, string interpolation `"${expr}"`, decimal and `0x`/`0b`/`0o` literals, inclusive `..`.
- **Control flow**: `if`/`else`, `while`, `for (x in a..b)`, `break`, `continue`, `when` statements.
- **Functional style**: expression-bodied functions, `=>`, `ret`/`return`, default/named/variadic
  parameters, overload link names, extension functions, lambdas and function-typed parameters.
- **Object-oriented and abstract types**: `class`, `struct`, `data class`, `enum`, `interface`,
  `annotation class`, constructors/destructors, `self`/`this`, inheritance and vtables, `pub`/`priv`.
- **Generics**: generic functions and classes are monomorphized before semantic analysis, with no
  boxing or runtime type-erasure cost.
- **Errors and resources**: `try`/`catch`/`finally`/`throw`, `~Name()` destructors, `del`,
  `@no_raii`, plus basic double-delete and local-pointer-escape checks.
- **C interop**: `embed C { ... }`, `embed C header { ... }`, `bind C fun ...`.

See [`docs/LANGUAGE.md`](docs/LANGUAGE.md) for current boundaries and runnable examples in
[`examples/`](examples/).

### Repository layout

```text
.
├── README.md              this file (Chinese then English)
├── CHANGELOG.md           change history
├── LICENSE / assets/      MIT license and icons
├── VERSION                version used by scripts (matches src/version.txt)
├── docs/                  language, self-host, package and build docs
├── examples/              hello, collections, broken and 01_values…08_stdlib
├── packages/              std, algo, json, net, toml, zip and other packages
├── src/
│   ├── version.txt        shared version source for tools and libraries
│   ├── libs/              syntax / sem / codegen / cfront / sfmt / sver
│   ├── sinterc/           self-hosting compiler
│   ├── sinter/            self-hosted package manager and build frontend
│   ├── sinterd/           JSON-line daemon
│   └── sinter-lsp/        stdio language server
├── tests/                 check.sh, check_sinter.sh, cross.sh, cases/, windows/
└── tool/                  helper scripts and CMake toolchains
```

### Quick start

Validated binaries live in `/home/eternalfuture/bin/sinter/`. From the repository root:

```sh
BIN=/home/eternalfuture/bin/sinter
REPO=$(pwd)

cd "$REPO/examples/hello"
SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build --run -j8
```

Rebuild all four tools from source:

```sh
BIN=/home/eternalfuture/bin/sinter
for p in sinterc sinter sinterd sinter-lsp; do
  (cd "src/$p" && SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build -j8)
  cp "src/$p/build/cmake/$p" "$BIN/$p"
done
```

Validate:

```sh
BIN=/home/eternalfuture/bin/sinter

CC=clang sh tests/check.sh "$BIN/sinter" "$BIN/sinterc"
CC=clang sh tests/check_sinter.sh "$BIN/sinter" "$BIN/sinterc"
SINTER="$BIN/sinter" SINTERC="$BIN/sinterc" CC=clang sh tests/cross.sh
```

Release bundles for all 13 targets are written to `build/<platform>.<arch>.zip`:

```sh
sh tool/build_release.sh
```

See the “Release bundles for all platforms” section in
[`docs/SELFHOST_PACKAGES.md`](docs/SELFHOST_PACKAGES.md).

### Self-host status

| Component | State |
| --- | --- |
| `src/libs/syntax` | lexer and recursive-descent parser for the current sources |
| `src/libs/sem` | module qualification and declaration/reference rewriting |
| `src/libs/codegen` | C backend: string ABI, classes/enums/globals, monomorphized generics, arrays, RAII, exceptions, extensions, `embed C` |
| `src/libs/cfront` | C99 lexer/parser and C formatter |
| `src/libs/sfmt` / `sver` | Sinter source formatter / build-time version |
| `sinterc` | self-compilation fixpoint: stage2 and stage3 C output is byte-identical |
| `sinter` | self-hosted frontend: `build` / `emit-c` / `emit-cmake` / `fmt` / `tree` / `install` / `add` / `remove` / `info` / `clean` |
| `sinter-lsp` | diagnostics, document symbols, hover, initialize/shutdown |
| `sinterd` | JSON-line Unix-socket daemon; `--self-test` passes 21 checks |

### Documentation

| Document | Contents |
| --- | --- |
| [`docs/LANGUAGE.md`](docs/LANGUAGE.md) | language features, syntax boundaries and examples |
| [`docs/SELFHOST.md`](docs/SELFHOST.md) | self-compilation fixpoint, compiler layers and tool services |
| [`docs/SELFHOST_PACKAGES.md`](docs/SELFHOST_PACKAGES.md) | `sinter.toml`, relative deps, portable CMake, cross builds, troubleshooting |

### Relationship to the original compiler

This repository is the main repository for Sinter. The original C++ prototype is kept only as a
historical branch:

<https://github.com/eternalfuture-e38299/sinterlang/tree/bootstrap-compiler>

Current development, documentation, packages and tests all live here.

### License

MIT, see [`LICENSE`](LICENSE). Copyright © 2026 Sinter contributors.

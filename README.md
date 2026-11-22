# Sinter bootstrap compiler — original C++ prototype / 最初的 C++ 原型编译器

> **Status / 状态**
>
> **EN:** This directory is an archived snapshot of the **original C++ implementation** of the Sinter compiler. It is kept as a reference and bootstrap artifact only. New development happens in the self-hosted Sinter toolchain under [`Sinter`](https://github.com/eternalfuture-e38299/sinterlang). Do not expect new features here.
>
> **中文：** 本目录保存的是 Sinter 编译器**最初的 C++ 实现**的归档快照，仅作为参考与自举历史产物保留。当前开发已转移到 [`Sinter`](https://github.com/eternalfuture-e38299/sinterlang) 下用 Sinter 自身编写的自举工具链；这里不再接受新功能开发。

---

## English

### What this is

This is the first working compiler for the Sinter language, written in C++20. It established the language semantics, diagnostics, C code generation, package manager, formatter/daemon, and LSP protocol that the later self-hosted toolchain re-implements in Sinter itself.

The directory contains only the C++ project and its documentation:

```
bootstrap-compiler/
├── CMakeLists.txt          # C++20 CMake build
├── VERSION                 # version source for the C++ build
├── LICENSE                 # license snapshot
├── CHANGELOG.md            # historical changelog snapshot
├── README.md               # this document (English + 中文)
├── include/sinter/         # public C++ headers
├── src/                    # compiler libraries
│   ├── support/            # diagnostics, source positions, shared utilities
│   ├── front/              # lexer, parser, generics, compile-time evaluation
│   ├── cfront/             # tolerant C / `embed C` analysis
│   ├── cfront2/            # independent C99 parser used by the daemon/LSP
│   ├── sem/                # semantic analysis and type checking
│   ├── backend/            # C code generation
│   └── driver/             # `sinterc` entry point
├── tools/                  # C++ tools
│   ├── sinter/             # package manager / build frontend
│   ├── sinterd/            # formatting + C analysis daemon
│   └── sinter-lsp/         # JSON-RPC LSP server
└── docs/                   # language, CMake, package and LSP references
```

### What it produced

| Tool | Role |
|---|---|
| `sinterc` | Sinter → C compiler |
| `sinter`  | package manager and build frontend |
| `sinterd` | formatting, C parsing and symbol daemon |
| `sinter-lsp` | LSP server over stdio |

### Build

Use a working CMake 3.20+ and a C++20 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

The binaries are written to `build/bin/`. If the system `cmake` is broken, point `cmake` at a working installation explicitly (for example `~/.local/bin/cmake`).

### Relationship to the current toolchain

- This C++ code is **the prototype/reference implementation**.
- The current, maintained implementation is the self-hosted Sinter project in [`Sinter`](https://github.com/eternalfuture-e38299/sinterlang).

### Scope of this archive

Do not put Sinter package sources, generated C, self-hosted projects, IDE plugins, or build outputs here. This directory intentionally contains **only the C++ project and its documents**.

---

## 中文

### 这是什么

这里是 Sinter 语言第一个可用的编译器实现，使用 C++20 编写。它确立了语言的语义、诊断、C 代码生成、包管理、格式化/守护进程以及 LSP 协议；后来用 Sinter 自身编写的自举工具链重新实现了这些能力。

目录中只保留 C++ 工程及其文档：

```
bootstrap-compiler/
├── CMakeLists.txt          # C++20 CMake 构建
├── VERSION                 # C++ 构建的版本来源
├── LICENSE                 # 许可证快照
├── CHANGELOG.md            # 历史变更记录快照
├── README.md               # 本文档（中英双语）
├── include/sinter/         # 公共 C++ 头文件
├── src/                    # 编译器库
│   ├── support/            # 诊断、位置、通用工具
│   ├── front/              # 词法、语法、泛型、编译期求值
│   ├── cfront/             # 容错 C / `embed C` 分析
│   ├── cfront2/            # 守护进程/LSP 使用的独立 C99 解析器
│   ├── sem/                # 语义分析与类型检查
│   ├── backend/            # C 代码生成
│   └── driver/             # `sinterc` 入口
├── tools/                  # C++ 工具
│   ├── sinter/             # 包管理器 / 构建前端
│   ├── sinterd/            # 格式化 + C 分析守护进程
│   └── sinter-lsp/         # stdio JSON-RPC LSP 服务
└── docs/                   # 语言、CMake、包管理与 LSP 参考文档
```

### 产物

| 工具 | 职责 |
|---|---|
| `sinterc` | Sinter → C 编译器 |
| `sinter`  | 包管理器与构建前端 |
| `sinterd` | 格式化、C 解析与符号守护进程 |
| `sinter-lsp` | stdio 上的 LSP 服务 |

### 构建

需要可用的 CMake 3.20+ 与 C++20 编译器：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

二进制输出到 `build/bin/`。如果系统 `cmake` 不可用，请显式指向可用的 cmake（例如 `~/.local/bin/cmake`）。

### 与当前工具链的关系

- 这份 C++ 代码是**最初的原型/参考实现**。
- 当前持续维护的实现是 [`Sinter`](https://github.com/eternalfuture-e38299/sinterlang) 中的自举 Sinter 工具链。

### 归档边界

不要把 Sinter 包源码、生成的 C、自举工程、IDE 插件或构建产物放进这里。本目录刻意只保留 **C++ 工程与文档**。

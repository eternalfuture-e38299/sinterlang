# 自举工具链

> 中文在前，英文在后。本文描述本仓库内的多项目自举工具链。原初 C++ 编译器只是本仓库的
> 历史分支：<https://github.com/eternalfuture-e38299/sinterlang/tree/bootstrap-compiler>。

## 中文

### 总体结构

自举工具链由一组可独立版本化的 Sinter 工程组成：

```text
src/
├── version.txt        所有 tools/libs 的版本源
├── libs/
│   ├── syntax/        TokenKind / NodeKind、AST 池、词法器、递归下降解析器
│   ├── sem/           多模块限定、声明与引用重写
│   ├── codegen/       C 后端与 runtime ABI
│   ├── cfront/        C99 词法/解析与 C 格式化
│   ├── sfmt/          Sinter 源码格式化
│   └── sver/          构建期版本字符串
├── sinterc/           编译器 CLI
├── sinter/            包管理器与构建前端
├── sinterd/           JSON-line Unix socket 守护进程
└── sinter-lsp/        stdio 语言服务器
```

所有 `sinter.toml` 使用相对依赖：

- `src/<tool>/sinter.toml` → `../../packages/std`、`../../packages/json`、`../libs/<lib>`
- `src/libs/<lib>/sinter.toml` → `../../../packages/std`、`../<lib>`
- `examples/<name>/sinter.toml` → `../../packages/std`
- `tests/cases/<name>/sinter.toml` → `../../../packages/<pkg>`
- 版本统一用 `file:../version.txt` 或 `file:../../version.txt` 指向 `src/version.txt`

`tests/check.sh` 复制测试工程到临时目录时保留相对层级，并链接 `packages/`，因此单独把
`tests/cases/<name>` 拷走也能解析相对依赖。

### 编译器分层

1. **`syntax`**：词法与解析。产生带行列信息的 AST，供后续所有工具复用。
2. **`sem`**：语义整理。解析模块归属，限定非 prelude 模块引用，重写声明与引用。
3. **`codegen`**：C 后端。生成 `sinter_runtime.c/.h` 与每模块一个 `sinter_<module>.c/.h`；
   负责字符串 ABI、类/枚举/全局、单态化泛型、数组、RAII、异常、扩展函数与 `embed C`。
4. **`cfront`**：C99 词法/解析与保守的 C 缩进格式化，服务于 `sinterd` 的 C 相关操作。
5. **`sfmt` / `sver`**：Sinter 源码格式化与构建期版本注入。

### 自编译固定点

验证入口是 `tests/check_sinter.sh`：

1. **stage0**：已安装的 `sinterc` 编译 `src/sinterc/src/main.sinter`，生成 stage1 C。
2. **stage1**：宿主 C 编译器把 stage1 C 编成可执行编译器。
3. **stage2**：stage1 编译自己的源码；stage2 与 stage3 生成的 C 必须逐字节一致。
4. **stage3**：stage2 编译并运行 `tests/fib.sinter`，输出与 `tests/expected/fib.txt` 一致。
5. 之后用 stage2 编译 Sinter 版 `sinter`，再由它构建 `sinter`、`sinterc`、`sinterd`、
   `sinter-lsp`，并验证版本输出、格式化、`sinterd --self-test` 与 LSP initialize/shutdown。

当前机器上的结果：**15 PASS，0 FAIL**；`sinterd --self-test` 为 **21 passed**。

### 工具服务

- `sinter fmt` 使用 `src/libs/sfmt`，与 `sinterd` 的 `format` / `reindent` 同源。
- `sinter-lsp` 通过 `Content-Length` stdio 协议提供诊断、文档符号、hover 与
  initialize/shutdown；`tests/check_sinter_lsp.sh` 做协议冒烟测试。
- `sinterd` 暴露 ping / format / reindent / tokens / diagnose / cparse / cparse2 / cheader /
  symbols / shutdown 等 JSON-line 操作。
- `tests/check_editor.sh` 还包含编辑器侧检查；其中 CLion 插件源文件属于独立维护的插件工程，
  缺失时脚本会明确跳过相关项，不影响编译器回归。

### 构建与验证

```sh
BIN=/home/eternalfuture/bin/sinter

for p in sinterc sinter sinterd sinter-lsp; do
  (cd "src/$p" && SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build -j8)
  cp "src/$p/build/cmake/$p" "$BIN/$p"
done

CC=clang sh tests/check.sh "$BIN/sinter" "$BIN/sinterc"
CC=clang sh tests/check_sinter.sh "$BIN/sinter" "$BIN/sinterc"
SINTER="$BIN/sinter" SINTERC="$BIN/sinterc" CC=clang sh tests/cross.sh
```

`tests/check.sh` 当前为 **35 PASS，0 FAIL**；`tests/cross.sh` 覆盖 x86_64/aarch64 的 Linux、
macOS、Windows 共 6 个目标 × 4 个工具。生成 CMake 中 `SINTER_ROOT` / `CMAKE_ROOT` 出现次数为 0。

---

## English

### Layout

The self-hosted toolchain is a set of independently versioned Sinter projects:

```text
src/
├── version.txt        version source shared by tools and libraries
├── libs/
│   ├── syntax/        TokenKind / NodeKind, AST pool, lexer, recursive-descent parser
│   ├── sem/           module qualification, declaration and reference rewriting
│   ├── codegen/       C backend and runtime ABI
│   ├── cfront/        C99 lexer/parser plus C formatter
│   ├── sfmt/          Sinter source formatter
│   └── sver/          build-time version string
├── sinterc/           compiler CLI
├── sinter/            package manager and build frontend
├── sinterd/           JSON-line Unix-socket daemon
└── sinter-lsp/        stdio language server
```

Every `sinter.toml` uses relative dependencies:

- `src/<tool>/sinter.toml` → `../../packages/std`, `../../packages/json`, `../libs/<lib>`
- `src/libs/<lib>/sinter.toml` → `../../../packages/std`, `../<lib>`
- `examples/<name>/sinter.toml` → `../../packages/std`
- `tests/cases/<name>/sinter.toml` → `../../../packages/<pkg>`
- versions come from `file:../version.txt` or `file:../../version.txt`, both pointing at `src/version.txt`

`tests/check.sh` preserves relative hierarchy when copying cases to a temporary directory and links
`packages/`, so a single `tests/cases/<name>` case still resolves its dependencies.

### Compiler layers

1. **`syntax`** — lexing and parsing into an AST with source positions, shared by all tools.
2. **`sem`** — module qualification and declaration/reference rewriting.
3. **`codegen`** — emits `sinter_runtime.c/.h` and one `sinter_<module>.c/.h` per module; handles
   string ABI, classes/enums/globals, monomorphized generics, arrays, RAII, exceptions, extensions
   and `embed C`.
4. **`cfront`** — C99 lexer/parser and conservative C reindentation for `sinterd` C operations.
5. **`sfmt` / `sver`** — Sinter source formatting and build-time version injection.

### Fixpoint

The entry point is `tests/check_sinter.sh`:

1. **stage0** — the installed `sinterc` compiles `src/sinterc/src/main.sinter` to stage1 C.
2. **stage1** — a host C compiler turns that C into an executable compiler.
3. **stage2** — stage1 compiles its own sources; stage2 and stage3 C output must be byte-identical.
4. **stage3** — stage2 compiles and runs `tests/fib.sinter`, compared with `tests/expected/fib.txt`.
5. The script then compiles the Sinter-written `sinter` with stage2, rebuilds all four tools and
   smoke-tests version output, formatting, `sinterd --self-test` and the LSP handshake.

On this machine: **15 PASS, 0 FAIL**; `sinterd --self-test` reports **21 passed**.

### Tool services

- `sinter fmt` uses `src/libs/sfmt`, shared with `sinterd` `format` / `reindent`.
- `sinter-lsp` speaks `Content-Length` stdio LSP and provides diagnostics, document symbols, hover
  and initialize/shutdown; `tests/check_sinter_lsp.sh` performs a protocol smoke test.
- `sinterd` exposes JSON-line operations such as ping / format / reindent / tokens / diagnose /
  cparse / cparse2 / cheader / symbols / shutdown.
- `tests/check_editor.sh` includes editor-side checks. CLion plugin sources belong to a separately
  maintained plugin project; when absent, those checks are explicitly skipped without affecting
  compiler regressions.

### Build and validation

```sh
BIN=/home/eternalfuture/bin/sinter

for p in sinterc sinter sinterd sinter-lsp; do
  (cd "src/$p" && SINTERC="$BIN/sinterc" CC=clang "$BIN/sinter" build -j8)
  cp "src/$p/build/cmake/$p" "$BIN/$p"
done

CC=clang sh tests/check.sh "$BIN/sinter" "$BIN/sinterc"
CC=clang sh tests/check_sinter.sh "$BIN/sinter" "$BIN/sinterc"
SINTER="$BIN/sinter" SINTERC="$BIN/sinterc" CC=clang sh tests/cross.sh
```

`tests/check.sh` currently reports **35 PASS, 0 FAIL**; `tests/cross.sh` covers six targets
(x86_64/aarch64 Linux, macOS and Windows) times four tools. Generated CMake contains zero
`SINTER_ROOT` / `CMAKE_ROOT` occurrences.

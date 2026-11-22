# sinter-lsp 与 sinterd

两个独立的可执行工具，服务于「在编辑器里写 Sinter」的体验。
`sinter-lsp` 不参与编译流程，也不链接语义层与代码生成后端——
它需要的只是词法/语法层面的信息，因此启动快、内存小、能容忍不完整的代码。

C 前端（`src/cfront/`）现在有**两个**消费者：`sinterd`（编辑器侧）和
`sinterc`（编译期）。后者用 `bind C header "x.h"` 把真实 C 头文件里的函数原型
与不透明结构体翻成 Sinter AST（`src/driver/c_bridge.cpp`），并与 `extern fun`
做 ABI 比对。它仍是独立静态库 `sinter_cfront`：语义层与代码生成后端都不依赖它，
所以「启动快、内存小」这个性质对 LSP 侧没有变化。

| 工具 | 角色 | 传输 | 依赖 |
|------|------|------|------|
| `sinter-lsp` | 语言服务器（补全 / 格式化 / 模板 / 跳转 / 悬停 / 大纲 / 诊断） | stdout/stdin，LSP `Content-Length` 帧 | Sinter 词法器 |
| `sinterd` | 后端守护进程（Sinter 格式化 / C 语法分析 / token） | Unix domain socket，一行一条 JSON | `sinter_format` + `src/cfront/` + `src/cfront2/` |

两者的分工来自语言结构：Sinter 源码归 `sinter-lsp`（用 Sinter 词法器），
`embed C { }` 块里的原生 C 归 `sinterd`（用 C 前端做完整 C11 解析）。
`sinter-lsp` 已经能定位 `embed C` 正文的字节范围（`Index::embed_c`），
但默认不主动调用 `sinterd`；需要跨进程信息时可开 `--backend <socket>`。

---

## sinter-lsp

### 用法

```sh
sinter-lsp --stdio          # 正常模式：编辑器通过 stdio 与它通信
sinter-lsp --self-test      # 内置自测（75 项）
sinter-lsp --verbose        # 把收发报文打到 stderr
sinter-lsp --backend /tmp/sinterd.sock   # 指定 sinterd 套接字（预留）
```

> `/sdcard`、`/storage` 是 `noexec` 挂载点，二进制必须复制到 `/tmp` 并 `chmod 755` 才能执行。

### 支持的能力

| LSP 方法 | 行为 | 实现要点 |
|----------|------|----------|
| `initialize` | 声明七项能力，`textDocumentSync.change = 1`（全量） | 索引是全量重建，增量同步反而更贵 |
| `textDocument/didOpen/didChange/didSave` | 更新文档并推送诊断 | 每次改动重建索引（几十毫秒级） |
| `textDocument/didClose` | 丢弃文档并推送空诊断 | 清掉编辑器里的残留标记 |
| `textDocument/completion` | 符号 / 关键字 / 内置类型 / **25 条模板**；`x.` 时只给该类型成员 | 见下「成员补全」与「模板补全」 |
| `textDocument/definition` | 跳到同名声明，取 `scope_depth` 最大者 | 就近绑定 |
| `textDocument/hover` | 返回 markdown 代码块（含类型），带 range | 声明处优先 |
| `textDocument/documentSymbol` | 按 `scope_depth` 还原父子层级 | 先算 parent/kids 再递归序列化 |
| `textDocument/formatting` | 整文件替换，只重排空白 | 见下「格式化」 |
| `textDocument/rangeFormatting` | **等价于整文件**格式化 | 缩进从文件开头累积，单独格式化一段会压平嵌套 |

触发器字符：`.`、`>`、`:`。

### 模板补全（snippet）

`completionProvider.completionItem.snippetSupport = true`，模板项带：

- `insertTextFormat = 2`（snippet）：`${1:name}` / `${2}` 是 tabstop，`$0` 是最终光标位；
- `documentation`：markdown 代码块，展开后长什么样一眼可见；
- `sortText = "0_" + label`：排在同名关键字**之前**——敲 `fun` 时先给「函数骨架」，
  而不是先给裸关键字 `fun`。

25 条覆盖：结构（`module` `import` `fun` `pub fun` `fun =>` `class` `struct`
`interface` `enum` `ext`）、控制流（`if` `ifelse` `while` `for` `forin` `when`）、
常用（`main` `println` `embed C` `bind C` `val` `var` `comptime`）、注释（`todo` `doc`）。

加一条模板只需在 `features.cpp` 的 `kSnippets` 里加一行；
要同步给 CLion 的 `Ctrl+J`，再改一份
`tools/clion-plugin/src/main/resources/liveTemplates/Sinter.xml`。
两份的一致性由 `scripts/check_snippets.py` 校验，`sh scripts/check_editor.sh` 会跑。

### 格式化

`src/format/sinter_format.cpp`（sinterd 与 sinter-lsp 共用同一份），行为契约
（改规则前先改 `tools/sinter-lsp/self_test.cpp` / `tools/sinterd/main.cpp` 的断言）：

1. 缩进 4 空格，不用 tab；
2. `{` 留行尾；跨行的 `}` 单独成行并回上一层缩进，**原本写成一行**的
   `{ ... }` 保持一行；
3. 括号内不断行，`,` 后补一个空格；
4. 二元运算符两侧各一空格（`= + - * / < > == != <= >= && || => ->` 等，多字符优先）；
   一元 `- + ! ~` 右侧紧贴（`ret -1` 不会变成 `ret - 1`）；
5. `;` 后一个空格（`for` 头里）；
6. 连续空行压成最多一行；
7. 文件末尾恰好一个换行；
8. 字符串 / 字符字面量 / 注释内部**一个字节都不动**（`///`、`/** */` 文档注释同样）；
9. `embed C { ... }` 正文原样保留（独立的花括号配对扫描，带字符串/字符/两种注释状态）；
10. 数字字面量整体输出，`2.2e-16` 不会被拆成 `2.2e - 16`。

**只动空白，不动任何非空白字符。** 这条是为了编辑器：正在敲的文件经常语法不完整，
只有纯空白重排才能保证「按了格式化不会毁掉代码」。

返回**单个 TextEdit 覆盖全文**，不做增量 range：行数一变增量 range 容易错位，
而订阅全量同步的客户端（CLion 插件就是）本来就接受整文件替换。

`rangeFormatting` 也走整文件：Sinter 的缩进是从文件开头累积出来的，
单独格式化一段需要知道该段原本的缩进层级，否则会把嵌套块压平。

### 为什么不做 AST

编辑器里的代码经常是**语法不完整的**（刚敲了 `fun f(` 还没闭合）。
这种情况下 AST 往往根本建不出来，而补全/跳转/悬停/大纲只需要
「哪些名字在哪里被声明/引用」，因此这里只用词法器扫一遍 token：

- 遇到 `fun/class/struct/interface/enum/module/var/val/const/alias`
  就把下一个标识符登记为声明；
- 其余标识符登记为引用；
- 用 `{`/`}` 维护 `brace_depth`，据此算出每个符号的 `scope_depth` 与所属 `container`。

`embed C` 正文用**原文里的花括号配对**定位（`find_embed_c_body`），
而不是复用 `EmbedCBody` token 的位置——Sinter 词法器 `add_token` 记录的是
「token 结束时」的行列，反推正文起点会差出二十几个字节。

### 成员补全怎么知道 `r.` 的类型

词法级索引没有类型推断，唯一可靠的线索是**显式写出的类型**：

```
fun use(r: Res) { r. }     → Symbol{r}.type_name == "Res" → 只列 Res 的成员
var total: i64 = ...       → Symbol{total}.type_name == "i64"
var x = Res()              → 没有 type_name → 退化为全局补全（宁多不漏）
self.                      → 用当前所在类的成员
```

一旦确定接收者类型，就**只返回该类型的成员**，即使为空也不掺关键字：
在 `r.` 后面弹出 `fun/class/if` 只会干扰。

### 已知限制

- 只支持 stdio 传输（所有主流编辑器的 LSP 客户端都走 stdio）。
- 不支持跨文件跳转/补全（索引是单文件的）。
- 不做类型检查，诊断只有词法错误与「花括号未闭合」提示。
- `embed C` 块内部目前不做深度的 C 语义诊断；需要时应对接 `sinterd`。
- 位置换算按 UTF-16 码元（LSP 规范），BMP 外字符（emoji）算 2 个码元。

### 编辑器接入示例（Neovim）

```lua
vim.lsp.start({
  name = "sinter",
  cmd = { "/tmp/sinter-lsp", "--stdio" },
  root_dir = vim.fn.getcwd(),
  filetypes = { "sinter" },
})
```

VS Code 需要在扩展里注册 `LanguageClient`，`serverOptions` 用
`{ command: "/tmp/sinter-lsp", args: { "--stdio" } }`。

### 编辑器接入示例（CLion）

CLion 插件在 `tools/clion-plugin/`，用 IntelliJ Platform 官方 LSP API
（`com.intellij.platform.lsp`，2023.2 起稳定）接入，没有自己写 JSON-RPC 客户端。
平台负责进程生命周期与能力协商，`sinter-lsp` 声明了什么能力，IDE 里就能用什么。

插件在 LSP 之外还补了两件 LSP 给不了的事：

- **语法高亮**：`SinterLexer.kt` + `SinterSyntaxHighlighter.kt`。高亮必须跟编辑器的
  增量重扫配合，只能活在 IDE 进程里。配色全部复用 `DefaultLanguageHighlighterColors`，
  自动跟随 Darcula / 亮色 / 自定义主题，不维护单独配色表。
- **Live Template**（`Ctrl+J`）：能在任意位置主动插入、可被用户改或禁用，
  这是 snippet 做不到的。正文与 `kSnippets` 保持一致，由脚本校验。

`ParserDefinition` 是**最简实现**（把文件切成一串叶子 token，不做语法分析）：
语法能力全在 LSP 侧，写个真 Parser 等于把 Sinter 语法维护两份。

构建、安装与取舍说明见 `tools/clion-plugin/README.md`。

---

## sinterd

### 用法

```sh
sinterd                              # 前台运行，默认套接字 /tmp/sinterd.sock
sinterd --socket /tmp/x.sock         # 指定套接字
sinterd --verbose                    # 打印请求到 stderr
sinterd --idle-timeout 300           # 空闲超时（秒）
sinterd --self-test                  # 内置自测（38 项）
```

默认**前台**运行：守护进程化会丢掉 bind 失败之类的错误输出，
在受限环境里排查问题会非常困难。

### 协议

一行一条 JSON 请求，一行一条 JSON 响应（换行分隔，便于 `socat`/`nc` 手工调试）。

请求：

```json
{"id": 1, "op": "format", "text": "fun main(): void{\nret 0\n}"}
```

- `id` 可以是字符串或数字；响应里**原样回显**（数字不加引号）。
- `op` 必填。
- 源码字段同时接受 `text` 与 `source`（文档用 `text`，手工调试常用 `source`）。

响应：

```json
{"id":1,"ok":true,"text":"fun main(): void {\n    ret 0\n}\n"}
{"id":2,"ok":false,"error":"未知操作: nope"}
```

### 操作一览

| op | 输入 | 输出 |
|----|------|------|
| `ping` | — | `pong`、`version` |
| `format` | `text`，可选 `indent`(4) | `text`（格式化后的 **Sinter 源码**） |
| `reindent` | `text`，可选 `base_indent`(0)、`indent`、`use_tabs` | `text`（只重排 C / `embed C` 正文的缩进） |
| `cparse` | `text`、`path`、`typedefs` | `functions`/`tags`/`typedefs`/`includes`/`items`/`diagnostics`（cfront） |
| `cparse2` | `text` | 同上结构，走独立实现的第二个 C 前端 `cfront2`（`frontend` 字段为 `cfront2`） |
| `symbols` | 同上 | 同上（语义上用于取符号表） |
| `symbols2` | `text` | 同 `cparse2`，供第二前端取符号 |
| `diagnose` | `text` | `diagnostics` |
| `tokens` | `text` | `tokens[]{kind,type,text,offset,length,line,column,directive?}`、`errors[]` |
| `shutdown` | — | `bye`，并退出 |

`tokens` 的 `kind` 是给语义着色用的粗分类（`keyword`/`identifier`/`number`/
`string`/`char`/`punctuator`/`operator`/`preproc`），`type` 是精确的 token 名。

### 手工调试

```sh
/tmp/sinterd --socket /tmp/sinterd.sock &
printf '%s\n' '{"id":1,"op":"ping"}' | socat - UNIX-CONNECT:/tmp/sinterd.sock
printf '%s\n' '{"id":2,"op":"format","source":"fun main(): void{ret 0}"}' \
  | socat - UNIX-CONNECT:/tmp/sinterd.sock
```

### 已知限制

- 单线程顺序处理（无锁，问题好定位）；一个连接可连续处理多条请求，
  且**一次 recv 带回多条也不会丢**（`LineReader` 按连接维护接收缓冲）。
- 不做宏展开、不做完整类型检查。
- `format` 处理 Sinter 源码；C / `embed C` 正文的缩进走 `reindent`
  （`src/cfront/c_format.cpp`）。

---

## 构建与测试

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DCMAKE_MAKE_PROGRAM=/usr/bin/make
cmake --build build-cmake -j6
```

两个工具都已注册进 ctest，共 7 个用例：

| 用例 | 内容 |
|------|------|
| `sinterd_selftest` | 36 项：协议、Sinter 格式化规则、cparse2（第二 C 前端）、token 分类、错误路径 |
| `sinterd_e2e` | 21 项：真实 socket，含「一次投递多条」与 id 类型回显 |
| `sinter_lsp_selftest` | 82 项：JSON、位置换算、索引、补全、跳转、悬停、大纲、格式化、模板、错误处理 |
| `cfront2_selftest` | 8 项：第二 C 前端的 typedef/struct/enum/函数/语句/表达式与 include 记录 |
| `sinter_lsp_integration` | 22 项：真实 stdio 会话，按 LSP 帧走完整编辑器流程 |
| `sinter.language` | 17 个语言用例 |
| `sinter.toolchain` | 14 个工具链用例 |
| `sinter.version` | 版本输出 |

`ctest` 里执行两个工具的自测时会把二进制先复制到 `/tmp`，
因为项目所在的挂载点是 `noexec`。

> 校验产物可执行性：`readelf -l build-cmake/bin/sinter-lsp | grep interpreter`
> 应显示 `/lib/ld-musl-aarch64.so.1`；显示 `/system/bin/linker64` 说明
> CMake 复用了失效的 Termux 路径，产物在 proot 内无法执行。
---

## 自举 LSP（`selfhost --lsp`）

`selfhost/` 里的标准 Sinter 工程自带一个最小 LSP 实现，用于验证自举前端：

```sh
cd selfhost
sinter build -j 4
./build/cmake/sinter-selfhost --lsp
```

能力：

- `textDocumentSync = 1`（全量同步）
- `textDocument/didOpen` / `didChange` / `didSave` → `publishDiagnostics`
- `textDocument/documentSymbol` → 顶层函数 / 类 / 枚举
- `textDocument/hover` → 当前行的声明签名
- `shutdown` / `exit`

它直接复用 `selfhost/src/lexer.sinter` 与 `parser.sinter`，语法诊断与自举编译器
一致。完整范围与路线图见 [`docs/SELFHOST.md`](SELFHOST.md)。

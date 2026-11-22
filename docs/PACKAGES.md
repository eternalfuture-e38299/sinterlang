# Sinter 包管理器（sinter）

`sinter` 负责依赖管理、项目构建，以及把 Sinter 项目导出为 CMake 工程。

> 本文描述的是 C++ 宿主版 `sinter`。由 Sinter 自己实现的自举版包管理器见
> [`docs/SELFHOST_PACKAGES.md`](SELFHOST_PACKAGES.md)：它的清单字段、生成的
> CMake 与环境变量略有不同。

---

## 快速开始

```sh
sinter init myapp          # 创建 sinter.toml + src/main.sinter
cd myapp
sinter build --run         # 编译并立即运行
```

---

## 命令一览

### 项目命令

| 命令 | 说明 |
| --- | --- |
| `sinter init [名字]` | 在当前目录创建 `sinter.toml` 与 `src/main.sinter`（已存在则报错） |
| `sinter add <包>[@版本]` | 添加依赖并写入 `sinter.toml`；参数是本地目录时登记为路径依赖 |
| `sinter remove <包>` | 移除依赖 |
| `sinter install` | 解析全部依赖、安装到本地仓库、生成 `sinter.lock` |
| `sinter build [--run] [--debug\|--release] [--opt <0-3\|s\|z>] [-j N] [--toolchain <文件>] [--cmake-arg <参数> ...]` | 生成 CMake 工程并构建（产物在 `build/cmake/`）；`--run` 立即执行（仅 exe 包），`--debug` 用 Debug 配置，`--release` / `--opt` 设置 Release 优化级别（默认 `-O2`，也可用 `SINTER_OPT`）；`--toolchain` 传交叉工具链，`--cmake-arg` 原样转给 CMake（如 `-DANDROID_ABI=arm64-v8a`） |
| `sinter emit-cmake [目录]` | 只生成 CMake 工程不构建（默认 `build/`，见 `docs/CMAKE.md`） |
| `sinter tree` | 打印依赖树 |
| `sinter info` | 打印清单摘要（kind / artifact / license / 依赖 / lock 状态） |
| `sinter clean` | 删除 `build/`（保留 `sinter.lock`） |

### 仓库命令

| 命令 | 说明 |
| --- | --- |
| `sinter publish [--path 目录]` | 把本地包复制进仓库并登记到索引 |
| `sinter list` | 列出仓库中所有包（每包显示最高版本） |
| `sinter search <关键词>` | 按名称或描述搜索 |

---

## 标准库是独立包

`std` 与 `net` 都有自己的版本号，不随编译器版本更新：

- `packages/std` → `std 1.5.2`（核心库）
- `packages/net` → `net 1.2.3`（`std.net`，依赖 `std`）
- `packages/zip` → `zip 1.3.1`（独立 `zip` 库，依赖 `std`；不随 std 发布）
- `packages/algo` → `algo 1.0.0`（独立算法库：AES 块/CBC/CTR）
- `packages/json` → `json 1.0.1`（独立 JSON 库：解析 + 序列化，依赖 `std`）
- `packages/toml` → `toml 1.1.0`（独立 TOML 库：解析 + 序列化，依赖 `std`）

内置包只是**种子**：已安装同版本时 `sinter` 不会覆盖；要发布新 std/net，
改包自己的 `version`。本地调试可设 `SINTER_REFRESH_BUNDLED=1` 强制刷新。

```toml
[dependencies]
std = "1.5.2"
net = "1.2.3"      # 只有 import std.net 时才需要
zip = "1.3.1"      # 只有 import zip 时才需要
algo = "1.0.0"     # 只有 import algo.aes 时才需要
json = "1.0.1"     # 只有 import json 时才需要
toml = "1.1.0"     # 只有 import toml 时才需要
```

## `sinter.toml` 格式

```toml
[package]
name = "myapp"
version = "0.1.0"
entry = "src/main.sinter"          # 编译入口，默认 src/main.sinter
description = "示例应用"
authors = ["张三 <z@example.com>"]
license = "MIT"                  # SPDX 标识，默认 MIT

[dependencies]
"std" = "0.3.0"                     # 精确版本
"mathx" = "^1.0"                    # 兼容 1.x
"util" = "~0.3.1"                   # 兼容 0.3.x
"legacy" = "*"                      # 任意版本
"local_lib" = { version = "0.0.1", source = "path:../local_lib" }
```

### 版本约束语法

| 写法 | 含义 |
| --- | --- |
| `1.2.3` | 精确匹配 |
| `>=1.0` / `>1.0` / `<=2.0` / `<2.0` | 比较 |
| `^1.2` | 主版本相同且不低于 1.2 |
| `~1.2.3` | 次版本相同且不低于 1.2.3 |
| `*` | 任意版本 |

版本比较按数字段逐段进行（`1.10.0 > 1.9.9`）。

---

## 产物形态（`artifact`）

一个包除了「源码从哪来」（`kind`），还要说明「产出什么」（`artifact`）。两者正交：

| `artifact` | 产物 | Sinter 包默认 | CMake 包默认 | 可被依赖 |
| --- | --- | --- | --- | --- |
| `"exe"` | 可执行文件 | ✅ | — | ❌ |
| `"static"` | 静态库 `.a` | — | ✅ | ✅ |
| `"shared"` | 动态库 `.so` / `.dll` / `.dylib` | — | — | ✅ |
| `"header"` | 纯头文件，不产出二进制 | — | — | ✅（只透传 include） |

不写 `artifact` 时按 `kind` 取默认值：Sinter 包是可执行文件，CMake 包是静态库。
这样旧清单不用改就能继续用。

### 写成可执行文件

```toml
[package]
name = "mytool"
version = "1.0.0"
entry = "src/main.sinter"
# artifact = "exe" 是默认值，可以不写
```

`sinter build` 产出 `build/cmake/mytool`，`sinter build --run` 直接运行它。
**可执行包不能被别的包依赖** —— `sinter add` 和依赖解析都会明确拒绝，并提示改成 `static` 或 `shared`。

### 写成静态库

```toml
[package]
name = "clib"
version = "1.0.0"
entry = "src/clib.sinter"
artifact = "static"
target = "clib"                  # 必填：导出给上层的 CMake 目标名
```

单独构建得到 `build/cmake/libclib.a`。被别的项目依赖时，会链接进那个项目。

### 写成动态库

```toml
[package]
name = "sharedlib"
version = "2.0.0"
kind = "cmake"
artifact = "shared"
target = "sharedlib"
```

生成的工程会：

- 用 `add_library(... SHARED ...)`（Sinter 源码包）或直接采用包内 `add_library(x SHARED ...)`（CMake 包）
- 为 Sinter 源码动态库打开 `POSITION_INDEPENDENT_CODE`
- 设置 `VERSION` / `SOVERSION`（来自包版本），保证运行期能找到正确的 `.so`
- 可执行文件带 `BUILD_RPATH` 指向构建目录，构建树里直接就能跑，不用设 `LD_LIBRARY_PATH`

### 写成头文件库

```toml
[package]
name = "hdr"
version = "0.1.0"
kind = "cmake"
artifact = "header"
target = "hdr"
include_dir = "include"          # 默认就是 include
```

不产出任何二进制，生成的工程里是一个 `INTERFACE` 库，只把头文件目录透传给使用者。
参考方在 `embed C` 里 `#include` 即可。

### 校验规则

`publish` 与依赖解析都会检查：

- 库包头（`static` / `shared` / `header`）必须有 `target`，且不能含空格或斜杠。
- 可执行包不能写 `target`（写了会误导依赖方）。
- 头文件包的 `include_dir` 不能为空，且发布时该目录必须存在。
- Sinter 源码库包的 `entry` 必须指向真实存在的文件。
- CMake 包（非头文件）必须有 `CMakeLists.txt`。

---

## 依赖来源

| `source` | 说明 |
| --- | --- |
| `registry`（默认） | 从本地仓库安装 |
| `path:<路径>` | 直接使用本地目录（相对路径相对于当前 `sinter.toml`） |

---

## 引用 CMake 项目（`kind = "cmake"`）

一个包可以不是 Sinter 包，而是一个**普通的 CMake 工程**。这样已有的 C/C++ 库不用先用
Sinter 重写一遍，就能被 Sinter 项目直接链接。

> 产出的是静态库、动态库还是纯头文件，由上面的 `artifact` 字段决定；
> 本节只讲「源码是 CMake 工程」这件事。

### 被引用方：CMake 包怎么声明

```toml
[package]
name = "fastmath"
version = "1.2.0"

kind = "cmake"                  # 默认是 "sinter"，这里改成 cmake
target = "fastmath"             # 必填：要导出的 CMake 目标名
cmake_dir = "."                 # 可选：CMakeLists.txt 所在子目录
find_package = "FastMath"       # 可选：先尝试 find_package 找系统安装的版本
```

配套的 `CMakeLists.txt` 必须真的定义同名目标：

```cmake
cmake_minimum_required(VERSION 3.20)
project(fastmath VERSION 1.2.0 LANGUAGES C)
add_library(fastmath STATIC fastmath.c)
target_include_directories(fastmath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

校验规则（`publish` 时就会挡住）：

- `target` 不能为空，不能含空格或斜杠（否则生成的 `target_link_libraries` 会崩）。
- `cmake_dir` 不能为空。
- 目录里必须存在 `CMakeLists.txt`。

### 引用方：怎么用

```toml
[dependencies]
"std" = "*"
"fastmath" = { version = "^1.0", source = "registry" }
```

依赖侧还能覆盖目标名，用于同一个库带多套编译变体的情况：

```toml
"fastmath" = { version = "^1.0", target = "fastmath_simd" }
```

`sinter add <本地CMake包目录>` 会自动识别 `kind = "cmake"` 并把 `target` 一起写进依赖。

### 生成出来的工程做了什么

对每个 CMake 依赖，生成器会插入：

```cmake
find_package(FastMath QUIET)          # 只有在清单里写了 find_package 才有
if(NOT TARGET fastmath)
    add_subdirectory(<包目录> ${CMAKE_CURRENT_BINARY_DIR}/deps/fastmath)
endif()
if(NOT TARGET fastmath)
    message(FATAL_ERROR "CMake 包 fastmath 未定义目标 fastmath（检查其 CMakeLists.txt）")
endif()
```

最后统一 `target_link_libraries(<项目>_sinter PUBLIC fastmath ...)`。
每个依赖单独用一个子构建目录，互不干扰。

### 在 Sinter 代码里调用该库

Sinter 目前没有独立的 FFI 语法，用 `embed C` 直接声明外部符号即可：

```sinter
module app

import std

embed C {
    int64_t fastmath_magic(void);
}

fun main(): void {
    printlnInt(fastmath_magic())
}
```

**注意类型宽度**：`embed C` 里的声明必须与库的真实 ABI 一致。C 的 `int` 是 32 位，
如果你在 Sinter 侧声明成 `int64_t`，读到的就是错的。要么让库导出 `int64_t`，
要么在 `embed C` 里写一个转换函数。

### 限制

- CMake 包**不参与** `sinterc` 的模块解析，所以不能 `import fastmath`；
  它只提供链接期符号。
- CMake 包自身没有 `entry`，`sinter build` 在它自己的目录里会拒绝执行
  （提示改用 `cmake -S . -B build`）。
- CMake 包目前只支持 `add_subdirectory` 引入，不支持从远程仓库拉取
  （先用 `path:` 依赖或手动放置）。

---

## 链接库与平台条件（`[link]`）

CMake 包解决的是「库本身怎么构建」；`[link]` 解决的是「当前项目要链接什么、
包含哪些目录、加哪些链接选项」。它可以直接表达原来写在 `CMakeLists.txt` 里的
`add_subdirectory` / `target_link_libraries` / `target_include_directories` /
`target_link_options`，并且支持按平台条件启用。

### 无条件链接

```toml
[link]
subdirs  = ["libs/greet"]
includes = ["libs/greet/include"]
libs     = ["greet"]
dirs     = ["libs/greet/lib"]          # 可选：target_link_directories
defs     = ["GREET_STATIC=1"]          # 可选：target_compile_definitions
options  = ["-Wl,--gc-sections"]       # 可选：target_link_options
```

生成到 `build/cmake/CMakeLists.txt` 的内容（顺序固定）：

```cmake
add_subdirectory("/项目/libs/greet" "${CMAKE_CURRENT_BINARY_DIR}/deps/link_0")
target_include_directories(<项目>_sinter PUBLIC "/项目/libs/greet/include")
target_compile_definitions(<项目>_sinter PUBLIC "GREET_STATIC=1")
target_link_libraries(<项目>_sinter PUBLIC "greet")
target_link_options(<可执行文件> PRIVATE "-Wl,--gc-sections")
```

字段对照：

| 字段 | 生成的 CMake | 作用域 |
| --- | --- | --- |
| `libs` | `target_link_libraries` | `PUBLIC`（头文件包为 `INTERFACE`） |
| `subdirs` | `add_subdirectory` | 每个子目录独立 `deps/link_<n>` 构建目录 |
| `includes` | `target_include_directories` | `PUBLIC` / `INTERFACE` |
| `dirs` | `target_link_directories` | `PUBLIC` / `INTERFACE` |
| `defs` | `target_compile_definitions` | `PUBLIC` / `INTERFACE` |
| `options` | `target_link_options` | exe → `PRIVATE`；shared → `PRIVATE`；static/header → `INTERFACE` |

相对路径按**项目根目录**解析。为了兼容普通 CMake 的写法，`subdirs` 里
普通相对路径会镜像到构建目录（`libs/libffi` → `${CMAKE_CURRENT_BINARY_DIR}/libs/libffi`），
`${CMAKE_CURRENT_SOURCE_DIR}` / `${CMAKE_SOURCE_DIR}` 会被重写成项目根目录；
其余 `${...}` / `$ENV{...}` 原样透传，所以
`${CMAKE_CURRENT_BINARY_DIR}/libs/libffi/include` 也能直接用。
只要出现 `subdirs`，生成的工程会启用 `LANGUAGES C CXX`，方便 shadowhook
这类 C++ 子项目。

### 平台条件

三种写法等价，任选：

```toml
[link.android]
libs     = ["log"]
subdirs  = ["libs/libffi", "libs/shadowhook"]
includes = ["libs/libffi/include",
            "${CMAKE_CURRENT_BINARY_DIR}/libs/libffi/include"]

[link.mingw]
options = ["-static-libgcc", "-static-libstdc++",
           "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic"]
```

```toml
[[link.condition]]
if = "ANDROID"
libs = ["log"]

[[link.condition]]
if = "MINGW OR (WIN32 AND CMAKE_C_COMPILER_ID STREQUAL \"GNU\")"
options = ["-static-libgcc", "-static-libstdc++",
           "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic"]
```

`if` 是**原样**写进 CMake `if(...)` 的表达式，可以使用 `AND` / `OR` / `NOT`、
`STREQUAL` 等 CMake 语法。`[link.<名字>]` 是常用平台的快捷写法：

| 段名 | 生成的 `if(...)` |
| --- | --- |
| `[link.android]` | `if(ANDROID)` |
| `[link.windows]` / `[link.win32]` | `if(WIN32)` |
| `[link.mingw]` | `if(MINGW)` |
| `[link.linux]` | `if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)` |
| `[link.macos]` | `if(APPLE)` |
| `[link.unix]` | `if(UNIX)` |
| `[link.msvc]` | `if(MSVC)` |
| `[link.gcc]` | `if(CMAKE_C_COMPILER_ID STREQUAL "GNU")` |
| `[link.clang]` | `if(CMAKE_C_COMPILER_ID MATCHES "Clang")` |

一个 `[[link.condition]]` 只写 `if` 也可以，多个条件块按出现顺序生成多个
`if(...)` 段。

### 依赖也带条件

CMake 包依赖可以在内联表里加 `when`，只在指定平台参与 `find_package` /
`add_subdirectory` / 最终链接（包仍会正常解析、安装）：

```toml
[dependencies]
"std" = "*"
"androidlog" = { version = "1.0.0", when = "ANDROID" }
"winhook" = { version = "1.0.0", target = "winhook_static", when = "WIN32" }
```

### Android / NDK 构建

`--toolchain` 已支持交叉工具链；配合 `--cmake-arg` 传 NDK 的 ABI/API：

```sh
NDK=$HOME/Android/Sdk/ndk/28.2.13676358
sinter build \
  --toolchain "$NDK/build/cmake/android.toolchain.cmake" \
  --cmake-arg -DANDROID_ABI=arm64-v8a \
  --cmake-arg -DANDROID_PLATFORM=android-24
```

首次构建时 `sinterc` 在 CMake 配置之后才生成 C 文件，`sinter build` 会自动
重新配置并重试一次；也可以手动再执行一次 `cmake --build`。NDK r28c 已验证
`[link.android]` 的 `libs = ["log"]` 与本地 `add_subdirectory` 库能一起链接。

限制：`[link]` 目前只读取**当前项目**的 `sinter.toml`；依赖包自己的 `[link]`
不会传播到上层（CMake 依赖包应由它自己的 `CMakeLists.txt` 处理链接），
`subdirs` 指向的目录必须包含 `CMakeLists.txt`。


## 仓库布局

仓库根目录由环境变量 `SINTER_HOME` 指定，默认 `~/.sinter`：

```
$SINTER_HOME/
├── registry/index.toml          # 包索引
└── packages/
    ├── std/0.3.0/...
    └── mathx/1.2.0/...
```

### 索引格式

索引由 `sinter publish` 自动维护，格式为：

```toml
[[package]]
name = "mathx"
version = "1.2.0"
description = "示例数学扩展包"
path = "/home/user/.sinter/packages/mathx/1.2.0"
```

### 内置包

`packages/` 目录下的包随发行版附带。`sinter` 首次运行 `build` / `install` / `list` 时会自动把它们登记进本地仓库，因此 `import std` 开箱可用。

设置 `SINTER_NO_BUNDLED=1` 可关闭该自动行为。

---

## 锁文件 `sinter.lock`

`sinter install` 会把解析结果写入锁文件，保证团队内构建结果一致：

```toml
# 由 sinter install 生成，勿手动编辑
version = 3

[[package]]
name = "mathx"
version = "1.2.0"
kind = "sinter"
artifact = "static"
target = "mathx"
path = "/home/user/.sinter/packages/mathx/1.2.0"

[[package]]
name = "sharedlib"
version = "2.0.0"
kind = "cmake"
artifact = "shared"
target = "sharedlib"
path = "/home/user/.sinter/packages/sharedlib/2.0.0"
```

- `version = 2` 增加了 `kind` 与 `target`。
- `version = 3` 增加了 `artifact`（`exe` / `static` / `shared` / `header`）。

**v1、v2 的锁文件仍然能读**，但下次 `install` 会写回 v3。

---

## 环境变量

| 变量 | 说明 |
| --- | --- |
| `SINTER_HOME` | 仓库根目录，默认 `~/.sinter` |
| `SINTERC` | `sinterc` 的可执行文件路径，默认从 `PATH` 查找 |
| `SINTER_NO_BUNDLED` | 设为任意值则跳过内置包自动登记 |

---

## 发布一个包

```sh
mkdir mylib && cd mylib
mkdir src
cat > sinter.toml <<'EOF'
[package]
name = "mylib"
version = "0.1.0"
entry = "src/mylib.sinter"
description = "我的库"
EOF
# 写 src/mylib.sinter ...
sinter publish
sinter list | grep mylib
```

发布后，其它项目即可：

```sh
sinter add mylib@^0.1
sinter build
```

---

## 典型工作流

```sh
# 1. 新建应用
sinter init app && cd app

# 2. 引用依赖
sinter add mathx@^1.0

# 3. 安装并锁定
sinter install

# 4. 开发循环
sinter build --run

# 5. 交给 CMake / IDE
sinter emit-cmake
cmake -S build -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j
```

---

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 参数错误、清单读取失败、依赖解析失败、编译或链接失败 |
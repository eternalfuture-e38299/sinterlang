# Sinter 构建系统（CMake）

Sinter 编译器本身是一个模块化 CMake 工程；同时 `sinter` 可以为任意 Sinter 项目**导出**一份可直接构建的 CMake 工程。

---

## 1. 编译编译器本体

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j6
```

产物：

| 路径 | 说明 |
| --- | --- |
| `build-cmake/bin/sinterc` | Sinter 编译器（Sinter → C） |
| `build-cmake/bin/sinter` | 包管理器 / 构建前端 |

### 目录结构

```
.
├── CMakeLists.txt              # 顶层工程：选项、PCH、子目录
├── src/
│   ├── support/                # sinter_support：token、诊断、PCH
│   │   ├── token.hpp
│   │   ├── diagnostics.{hpp,cpp}
│   │   └── pch.hpp
│   ├── front/                  # sinter_front：词法 + 语法 + AST
│   │   ├── ast.hpp
│   │   ├── lexer.{hpp,cpp}
│   │   └── parser.{hpp,cpp}
│   ├── sem/                    # sinter_sem：名称解析、类型检查、模块
│   │   └── semantic.{hpp,cpp}
│   ├── backend/                # sinter_backend：C 代码生成（按关注点拆 TU）
│   │   ├── codegen.hpp
│   │   ├── codegen.cpp         # 顶层驱动
│   │   ├── codegen_types.cpp   # 类型推断 / C 类型映射 / 类布局
│   │   ├── codegen_lifecycle.cpp # 构造/析构/拷贝/移动/函数定义
│   │   ├── codegen_stmts.cpp   # 语句
│   │   └── codegen_exprs.cpp   # 表达式
│   └── driver/                 # sinterc 可执行文件
│       └── main.cpp
├── tools/sinter/                 # sinter：包管理器 + 构建前端
│   ├── manifest.{hpp,cpp}      # sinter.toml 解析与序列化
│   ├── registry.{hpp,cpp}      # 本地仓库索引、依赖解析、锁文件
│   ├── bundled.{hpp,cpp}       # 随发行版附带的内置包
│   └── main.cpp
├── packages/                   # 内置包（std 等）
├── projects/                   # 项目化示例：sinter.toml + src/main.sinter + expect.txt
├── tests/                      # 三套端到端回归（语言 / 工具链 / 项目）
│   └── cases/                  # 语言用例：每个用例一个 sinter 工程（sinter.toml + src/ + expect.txt）
└── docs/
```

### 模块依赖关系

```
sinterc ──> sinter_backend ──> sinter_sem ──> sinter_front ──> sinter_support
sinter    ──> (独立，仅用标准库)
```

每个模块是静态库，可以单独编译、重复使用。构建时会并行编译 8 个翻译单元。

### CMake 选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | 构建类型 |
| `SINTER_BUILD_TESTS` | `ON` | 注册语言与工具链回归测试 |
| `SINTER_USE_PCH` | `ON` | 预编译头（`src/support/pch.hpp`），显著加快全量构建 |
| `SINTER_WARNINGS_AS_ERRORS` | `OFF` | 把警告当错误 |

示例：

```sh
# 最快的一次干净构建
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release -DSINTER_USE_PCH=ON
cmake --build build-cmake -j6

# 只要编译器，跳过测试
cmake -S . -B build-cmake -DSINTER_BUILD_TESTS=OFF

# 严格模式（CI）
cmake -S . -B build-cmake -DSINTER_WARNINGS_AS_ERRORS=ON
```

### 增量构建

- 每个模块一个静态库，只改动 `codegen_exprs.cpp` 时其它模块不会重编。
- PCH 命中后，单 TU 解析标准库的开销几乎消失。

### 运行测试

```sh
cd build-cmake
ctest --output-on-failure
```

| 测试 | 内容 |
| --- | --- |
| `sinter.language` | 10 个语言特性用例（多态 / vtable / RAII / 空安全 / RTTI / 别名冲突…） |
| `sinter.toolchain` | 28 个工具链用例（init / publish / add / install / build / emit-cmake / CMake 包 / 三种产物形态） |
| `sinter.version` | 包管理器自检 |

### 安装

```sh
cmake --install build-cmake --prefix /usr/local
```

安装内容：`bin/sinterc`、`bin/sinter`、`include/sinter/`、文档。

---

## 2. 为 Sinter 项目导出 CMake 工程

`sinter emit-cmake` 把工程写进 `build/`（也可传自定义目录）：

```
你的项目/
├── sinter.toml
├── src/main.sinter
└── build/
    └── CMakeLists.txt     # 由 sinter 生成
```

构建（注意 `-S` 是工程目录，`-B` 是构建目录，两者必须不同）：

```sh
sinter emit-cmake
cmake -S build -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j
./build/cmake/<你的项目名>
```

或者直接用 `sinter build`，它会把上面三步一次做完，产物在 `build/cmake/<你的项目名>`。

### 交叉编译 / 自定义 C 工具链

`sinter build --toolchain <file.cmake>` 会把 `-DCMAKE_TOOLCHAIN_FILE=<file>` 传给 cmake，
并强制重新配置（不会复用宿主机的 CMakeCache）。Android / 嵌入式 / 其他架构的
交叉编译都走这一条；仓库里的 `tool/android-arm64.cmake` 就是一个例子。

```sh
sinter build --toolchain tool/android-arm64.cmake
```

生成的工程默认目标 C99（`CMAKE_C_STANDARD 99` + `CMAKE_C_EXTENSIONS OFF`），
并给 GNU/Clang 加上 `-ffunction-sections -fdata-sections` 与链接期
`-Wl,--gc-sections`（macOS 用 `-Wl,-dead_strip`），减小最终产物。

### 生成的 `CMakeLists.txt` 做了什么

1. `set(SINTERC_EXE ...)` / `find_program` 定位 `sinterc`（可用环境变量 `SINTERC` 指定）。
2. 写死目标 C 标准：`set(CMAKE_C_STANDARD <n>)` + `CMAKE_C_STANDARD_REQUIRED ON` +
   `set(CMAKE_C_EXTENSIONS <ON|OFF>)`。取值来自 `sinter.toml` 的 `[build] cstd`
   （默认 `c99` → `99` + `OFF`；`c17` → `17` + `OFF`），可被 `--cstd` / `SINTER_CSTD` 覆盖。
3. `add_custom_command` 把入口 `.sinter` 编译为 C（同时产出全部依赖模块的 `.c/.h`，
   并写出 `gen/sources.cmake` 列出这些文件）。
4. 配置期先跑一次 `sinterc` 刷新源文件清单（`gen/sources.cmake`）。
5. 按产物形态建目标：
   - `artifact = "exe"` → `add_library(<name>_sinter STATIC ...)` + `add_executable(<name> ...)`
   - `artifact = "static"` → `add_library(<name>_sinter STATIC ...)`，产出 `lib<target>.a`
   - `artifact = "shared"` → `add_library(<name>_sinter SHARED ...)` + `POSITION_INDEPENDENT_CODE`
     + `VERSION`/`SOVERSION`，产出 `lib<target>.so`
   - `artifact = "header"` → `add_library(<name>_sinter INTERFACE)`，只有 include 目录
6. CMake 依赖包逐个 `add_subdirectory` + `target_link_libraries`；
   头文件包只 `target_include_directories`，源码包才 `add_subdirectory`。
7. 可执行产物额外设置 `BUILD_RPATH` 指向构建目录，这样依赖动态库时在构建树里直接可运行。

> 为什么需要「产物清单」这一步？`sinterc` 一次调用会产出多个 `.c`（入口模块 +
> 全部被 `import` 的模块 + runtime），具体有哪几个只有跑完才知道，而 CMake 在
> **配置阶段**就必须知道要编译哪些源文件。所以让 `sinterc` 自己写一份清单
> （`gen/sources.cmake`），CMake 直接 `include` 它。
>
> 清单是**每次配置都刷新**的，不是「不存在才生成」。后者会漏编：新增依赖或新增
> 模块后，旧清单被 include 进来，新模块根本不在源文件列表里，链接时报
> `undefined reference` 且完全看不出原因。

### Sinter 模块搜索路径

导出的工程会把以下目录都加入 `-I`：

1. 项目自己的 `src/`
2. `sinter.toml` 中声明的全部依赖（含传递依赖）的 `src/` 与根目录
3. 内置包（`std`）的源码目录

因此 `import std` / `import mathx` 在 CMake 工程里同样可用。

---

## 3. 手工集成（不用 sinter）

如果你的项目不想引入 `sinter`，可以手写：

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp LANGUAGES C)

set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY ${GEN})

find_program(SINTERC_EXE sinterc REQUIRED)

add_custom_command(
        OUTPUT ${GEN}/sinter_myapp.c
        COMMAND ${SINTERC_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/src/main.sinter
                -o ${GEN} -I ${CMAKE_CURRENT_SOURCE_DIR}/src
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/main.sinter
        VERBATIM)

add_custom_target(myapp_gen DEPENDS ${GEN}/sinter_myapp.c)

add_custom_target(myapp ALL
        COMMAND ${CMAKE_C_COMPILER} ${GEN}/sinter_std.c ${GEN}/sinter_myapp.c
                -o ${CMAKE_CURRENT_BINARY_DIR}/myapp -I ${GEN} -lm
        DEPENDS myapp_gen VERBATIM)
```

---

## 4. 平台说明

- 需要 CMake ≥ 3.20、支持 C++20 的编译器（GCC 11+ / Clang 13+）。
- 生成代码目标是 C11，用任意 C 编译器链接即可。
- 在 `/sdcard`、`/storage` 等 `noexec` 挂载点上，构建出的可执行文件需要先复制到可执行文件系统（例如 `/tmp`）再运行。

---

## 5. 没有 cmake 的环境：`sinter emit-c`

`sinter build` 的最后一步是 `cmake --build`，而 CMake 官方只发布 x86_64 / darwin 宿主包，
**arm64 Android 设备上没有可用的 cmake**。为此 `sinter` 提供一条只做前半段的命令：

```sh
sinter emit-c [目录] [--debug]      # 默认目录是 build/
```

它做的事和 `sinter build` 完全相同，直到「C 源已经生成」为止：

1. 解析并发布依赖（`std` 等内置包从 `SINTER_BUNDLED_DIR` 登记，`SINTER_HOME` 下建仓）；
2. 生成 `CMakeLists.txt`（**是的，emit-c 也会写**，否则「手机生成、电脑编译」缺一半）；
3. 调用 `sinterc <entry> -o <dst>/gen --emit-sources-list <dst>/gen/sources.cmake -I …`
   产出 `sinter_runtime.c` + 各模块 `.c/.h` + 入口 `main()`。

产物：

```
build/gen/*.c            C 源码（含 runtime）
build/gen/sources.cmake  源文件清单（CMake include 用）
build/CMakeLists.txt     可直接交给桌面 cmake
```

在桌面接着编：

```sh
cmake -S build -B build/cmake -DSINTERC_EXE=/path/to/sinterc
cmake --build build/cmake -j
```

或者在任意有 cc 的机器上一步到位（参数顺序与 CMakeLists 里那条 `add_custom_command` 一致）：

```sh
cc -std=c99 build/gen/*.c -I build/gen -o build/app -lm
```

这条命令由 `sinterc` 打印，`-std=` 的取值跟工程实际配置一致：`sinter emit-c --cstd c17`
时它打出的是 `-std=c17`。纯手工集成时换成你要的标准即可，下限是 `c99`。

`sinter emit-c` 与 `sinter build` 共用同一份依赖解析与 CMake 生成代码，所以两者不会漂移；
区别只在于「要不要继续调 cmake」。
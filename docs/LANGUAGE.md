# Sinter 语言与编译器（本轮实现说明）

Sinter 由 C++ 实现，把 `.sinter` 编译成可直接用 C 编译器构建的 C 代码
（默认按 `c99` 构建，可配置为 `c99` / `c11` / `c17` / `c23`，见下）。
编译器源码在 `src/`，构建产物 `build/sinterc`。

## 构建

```sh
g++ -std=c++2b -O2 -Wall -Wextra -Isrc \
    src/main.cpp src/lexer.cpp src/parser.cpp src/codegen.cpp src/semantic.cpp \
    -o build/sinterc
```

注意：`build/` 位于 `/sdcard`（无执行权限），本地运行请先
`cp build/sinterc /usr/local/bin/sinterc`。

## 用法

```sh
sinterc <entry.sinter> -o <输出目录> -I <搜索目录>... \
        [-D key=value ...] [--target-os=<os>] [--target-arch=<arch>] \
        [--cstd <std>] [--release|--debug]
```

`--cstd` 只影响 sinterc 打印的那条「手动 gcc 链路命令」，真正的编译标准由
`sinter` 写进生成的 CMake 工程。

输出文件名带 `sinter_` 前缀（`sinter_<模块>.c/.h`），避免与系统头
（`math.h`、`string.h` 等）冲突。入口模块会自动生成 C 的 `main()`。

### 目标 C 标准

实测记录见「回归测试」里的 `scripts/cases/cstd` 用例：同一个工程用 `c17` 与 `gnu17`
各构建一次，断言 CMake 里 `CMAKE_C_STANDARD` / `CMAKE_C_EXTENSIONS` 与预期一致。

默认 `c99`。三种写法等价，优先级从高到低：

1. 命令行：`sinter build --cstd c17`（`emit-c` / `emit-cmake` 同样支持）
2. 环境变量：`SINTER_CSTD=c17`
3. `sinter.toml`：

```toml
[build]
cstd = "c17"
```

允许 `c99` / `c11` / `c17` / `c23`（严格 ISO C，`sinter` 会写
`set(CMAKE_C_EXTENSIONS OFF)`）与 `gnu99` / `gnu11` / `gnu17` / `gnu23`
（带 GNU 扩展，`ON`）。大小写不敏感，`c9x` / `c1x` / `c2x` 也认。

**C99 是下限**：生成代码用了 `for (long long i = 0; …)` 与块中间的声明，
所以 `c89` / `c90` 会被 `sinter` 直接拒绝并说明原因，而不是留给 gcc 报一句
跟 C 标准无关的错。写错值也一样在加载 `sinter.toml` 时报错并列出可选值。

手写编译命令时对应加 `-std=<值>`：

```sh
gcc -std=c17 -pedantic-errors -w sinter_*.c -o program -I <输出目录> -lm
```

生成代码在 `-std=c99 -pedantic-errors` 与 `-std=c17 -pedantic-errors` 下都零报错。
唯一涉及 `__attribute__` 的地方是 `sinter_runtime.h` 里的 `sinter_maybe_unused`，
它有三重保护（`#ifndef` + `#if defined(__GNUC__)||defined(__clang__)` + `#else` 空宏），
非 GNU 编译器下退化为空定义，因此在严格 `c99` / `c17` 下也不报警。

## 回归测试

回归分成「三个工程 + 专题用例」：工程在 `projects/` 下，专题在 `scripts/cases/` 下。

```sh
sh scripts/check.sh /tmp/cbin/sinter /tmp/cbin/sinterc
```

| 用例 | 角色 | 判定 |
|---|---|---|
| `projects/hello` | 正向：语言基础面（类、RAII、字符串模板、管道、`when`） | `expect.txt` 逐行比对程序输出 |
| `projects/collections` | 综合：泛型容器 + 多模块 + 标准库 + FFI | 同上 |
| `projects/broken` | 反向：同名函数未加别名，必须编译失败 | `expect_error.txt` 每行都要出现在诊断里 |
| `scripts/cases/strtmp` | 专题：调用实参里的字符串临时值（A.1） | `expect.txt` + `assert_gen.txt` |
| `scripts/cases/cstd` | 专题：`[build] cstd` 是否真的落到 CMake | `expect.txt` + `assert_gen.txt` |
| `scripts/cases/langfix` | 专题：`void` 表达式体 / 泛型模板原型 / 转义字面量长度 | `expect.txt` + `assert_gen.txt` |
| `scripts/cases/badmember` | 反向：调用不存在的成员必须报 `E0306` | `expect_error.txt` |

判定方式由目录里的文件决定，脚本里没有用例清单：`expect.txt` 表示「必须构建成功且输出一致」，
`expect_error.txt` 表示「必须构建失败且诊断命中」，`assert_gen.txt` 表示「生成的 C 必须长成这样」——
每行 `<运算符>|<子串>`，运算符为 `<=N` / `>=N` / `==N`，计数对象是该工程
`build/**/gen/*.c` 里这个**定长子串**的出现次数。

只有正向用例时，「全部通过」只说明正确程序跑得对，不能说明错误程序挡得住 ——
`projects/broken` 就是为了后者：它验证 E0204（同名函数必须起别名）。
`assert_gen.txt` 补的是另一个盲区：程序输出正确、但生成代码把同一个内层调用求值两次
（双份副作用 + 临时值泄漏）时，只看 stdout 是看不出来的。

## 诊断与类型检查

编译器的报错统一走 `support/diagnostics.hpp`，**全部为英文**，输出带「文件:行:列」、
稳定错误码、**模块 › 类/结构体/接口 › 函数** 上下文、源码行与插入符下划线，并尽可能给出一条可执行的修复建议：

```
main.sinter:4:18: error[E0301]: variable x initializer type mismatch: expected i64, got str
  = scope: module hello › class Bad › fun check
  |
4 |     var x: i64 = "str"
  |                  ^^^^^
  = hint: check the declared type and the type of the value on the right
```

错误码段位（新增诊断请沿用同一段位，便于脚本化匹配）：

| 段位 | 含义 |
|---|---|
| `E01xx` | 词法 / 语法（未闭合字面量、期望 token 与实际 token 不符） |
| `E02xx` | 声明、名字与可见性（构造参数不足、`when` 非穷尽） |
| `E03xx` | 类型检查（初始化 / 赋值 / 返回值不匹配，`str` 参与算术或与数字比较，成员调用不存在） |
| `E04xx` | 泛型与单态化（缺少类型实参、`must` 约束不满足） |
| `E05xx` | 编译期求值（comptime 条件不可求值、静态断言失败） |
| `E06xx` | 代码生成内部错误（例如编译期关键字漏折） |
| `E07xx` | `extern C` ABI 约束 |
| `E08xx` | C 互操作（头文件找不到、`extern fun` 与 C 原型不匹配、类型无法映射），见「C 互操作」 |

- 位置来自 `Token` 的行列（lexer 记录 **token 起始列**，不是扫描结束列），
  由 parser 写入 `Expression::line/col` 与 `Statement::line/col`，`clone_expr`
  复制节点时一并保留。字符串插值里的 token 会被平移回外层坐标。
- **语法错误也走同一套渲染**：`Parser` 内部的 `consume` 只负责说「期望什么」，
  统一的 `error()` 会在末尾补上「实际看到什么」（`, found 'x'` / `, found end of line`），
  再由 driver 渲染源码行与 `^^^`。设置 `source_path` 之前退化为单行文本，
  只供内部自测使用。
- 一次编译会尽量报全同一次分析里的多个错误，不是遇到第一个就停。
- 任何诊断产出后，`sinterc` 返回非零退出码并停止生成代码，不会把非法 C 交给 gcc。

类型检查（`src/sem/typeck.cpp`）在语义分析之后、代码生成之前运行，当前覆盖：

| 检查 | 例子 |
|---|---|
| 变量初始值类型 | `var x: i64 = "str"` |
| 赋值类型 | `var n: i64 = 1; n = "hello"` |
| str 参与算术运算 | `"a" - 1` |
| str 与数字比较 | `"a" == 1` |
| 返回值类型 | `fun f(): i64 => "s"` |
| 构造函数参数不足 | `Vec(1)` 而 `Vec(x: i64, y: i64)` |

**设计原则是保守**：只要有一侧类型推不出来（`CType::Unknown`）就一律放行，
宁可漏报也不判死合法程序。子类可赋给父类（沿 `base_class` 链判断）。

明确**不做**的检查（会大面积误报，或需要所有权分析）：

- **不检查「函数/变量是否已声明」**。Sinter 大量使用 `bind C`、`embed C`
  与跨模块隐式依赖，`Semantic::functions()` 不是名字全集，据此报错会把
  合法的 FFI 代码全部判死（实测 std 的 `str`/`arr` 模块会立刻翻车）。
- 不检查数组元素类型、泛型实例化内部的类型一致性。
- 不做空指针与跨函数生命周期检查；只做**函数内**的保守本地生命周期检查（见 `docs/SPEC.md` §8）。

## 已实现的特性

| 特性 | 语法示例 | 实现位置 |
| --- | --- | --- |
| 块式模块 + `pub {}` | `module math { pub { fun f() {...} } }` | parser / semantic / codegen |
| `number` 整数统称 | `fun m<T>(a: number, b: number): T must number` | `Semantic::check_constraint` |
| 泛型 + `must` 约束 | `<T must number>`、尾置 `: T must number` | parser / 单态化 |
| 泛型类 / 泛型结构体 | `class Box<T>(val v: T)`、`struct Pair<A, B must number>(...)`，使用处写 `Box<i64>(7)` | `src/front/generic_mono.cpp` |
| 重载 + 仅返回类型分支 | `fun square(x: i32): i32` / `fun square : i64` | `register_function` |
| 函数冲突检测 | 同签名同返回 → `函数冲突…请使用别名` | `register_function` |
| 函数别名 | `fun second = first` | 语义转发 + codegen 转发函数 |
| 泛型函数别名 | `fun arrayOf<T>[anyArrayOf<T>](e: T...)`：别名可重复声明类型参数；显式/推断类型实参都能通过别名调用 | parser / `generic_fn_c_name` |
| 别名 `[]`（Sinter 侧） | `fun square[sq](x: i64): i64` —— `[]` 内是**额外可调用的 Sinter 名**（`sq(2)`）；C 符号名由编译器生成（模块 + 签名 + 别名片段） | `FunctionDecl::alias_name` / `register_function` |
| `inline` / `static` | `inline fun`、`static fun`、`static var` | 全链路 |
| struct / class 括号构造参数 | `struct my_struct(val name: str, var address: str, date: i64)`；构造函数里写 `val`/`var` 的成员即公开字段；带构造参数的类/结构体可省略空类体 `struct Point(val x: i64, val y: i64)` | `parse_class_like` |
| data class / data struct | `data class Point(val x: i64, val y: i64)` 自动生成 `equals` / `hashCode` / `toString` / `copy(...)`（字段默认值为 `self.field`，支持 `copy()` / `copy(x=2)` 部分复制）/ `copyWith(...)` / `componentN()`；可省略类体 | `parse_class_like` / `add_data_methods` |
| 尾随 lambda（DSL） | `twice { println("hi") }`、`Button().apply { text = "OK" }`；块内 `this` / `self` 指向接收者，裸字段名直接解析到接收者 | `parse_block_lambda` / `expr_scope_call` |
| 接收者函数类型 | `fun build(block: Button.() -> void): Button { val b = Button(); b.block(); ret b }`；调用 `build { text = "OK" }`，`b.block()` 把接收者作为第一个实参；接收者类型也可以用于 `str.()` 等 | `TypeAnnotation::receiver_type` / `expr_lambda` |
| `= default` 构造/析构 | `my_struct() = default`、`~my_struct() = default` | parser / codegen |
| 继承 + 多态（vtable） | `class Circle(...) : Shape(name)` | `build_vtables` / `emit_vtables` |
| `is` / `in` 模式 | `when (x) { in 0..5 => … }`、`is` | `expr_when` |
| `when` 表达式 | `val g = when (n) { … }` | `expr_when` |
| 管道 `\|>` | `n \|> sum3(1, 2)` | `expr_pipeline` |
| 解构赋值 | `val (x, y) = Pair(7, 9)` | `stmt_destructure` |
| 区间字面量 | `0..5`、`0..<5` | lexer / parser |
| 函数类型 / 高阶函数 | `fun apply(f: (i64) -> i64, x: i64): i64 => f(x)`；传顶层函数或 lambda 都行 | `TypeAnnotation::FunctionType` / `expr_ident` / 泛型替换 |
| Lambda | `fun (x: i64): i64 => x * x` | `expr_lambda`（提升到文件作用域） |
| 字符串模板 | `"result: ${r}"` | `expr_template` |
| 注解系统 | `@deprecated("use g", "1.1", "2.0")` 调用点报 `W0302` 并在提示里带弃用/移除版本；`@experimental` 报 `W0304`；`@frozen("1.0")` / `@since("1.0")` 记录 API 生命周期；`@must_use` 报 `W0303`；`@no_raii` 关闭自动析构 | `parse_annotations` / `typeck.cpp` |
| 扩展函数（Kotlin 风格） | `ext fun i64.twice(): i64 => self * 2` | parser（隐式 `self` 形参） |
| 命名参数 + 默认值 | `add(2, b = 5)`、`fun add(a: i64, b: i64 = 10)`；可以跳过中间参数，只给 `f(a = 1, c = 3)`，被跳过的参数必须有默认值（函数、方法、构造函数一致） | parser / `expr_call` / `typeck` |
| 注解类型 `annotation class` | `annotation class Route(val path: str, val code: i64 = 200)`：声明编译期注解规格；`@Route("/x")` 在编译期校验参数个数、必填项与字面量类型 | parser / `Semantic::validate_annotations` |
| 成员可见性 | 类里的 `fun` 默认只在本模块可见（跨模块调用报 `E0202`）；`pub fun` / `pub { ... }` 导出，`priv fun` / `priv { ... }` 显式收回 | parser / `typeck` / 模块头文件 |
| 复制 / 移动构造 | `fun T(const T&)`、`fun T(T&&)`、`_copy` / `_move` | `emit_class_ctor_def` |
| 作用域函数 / DSL | `v.let(fun(t) => t * 3)`、`.let { }` / `run` / `apply` / `also` / `with`；块内 `this`/`self` 与裸字段名都指向接收者 | `expr_scope_call` |
| RAII 与关闭开关 | 自动析构；`@no_raii` 注解、`no_raii { }` 块 | `FunctionDecl::no_raii` / `BlockStmt::no_raii` |
| RAII 退出路径 | 作用域结束、`return`、`break`、`continue` 四条路径都触发析构，且每条恰好一次 | `emit_scope_exits` / `stmt_return` |
| 类型推断声明的析构 | `var r = Res(1)` 与 `var r: Res = Res(1)` 行为一致（按最终 C 类型登记） | `register_raii` |
| 显式析构 | `del x` | `stmt_del` |
| 三元 / Elvis | `c ? a : b`、`a ?: b` | `expr_ternary` |
| 表达式函数 | `fun f(x: i64): i64 => x + 1` | `parse_function` |
| 可空类型 | `T?` | `TypeAnnotation::Optional` |
| 异常 | `try { ... } catch (e: str) { ... } finally { ... }`、`throw "msg"`；未捕获异常打印源码位置、backtrace 后 `abort()` | parser / `stmt_try` / `sinter_throw` |
| 本地生命周期检查 | 拒绝返回 `&局部`、`del` 后使用、重复 `del`、`&局部` 逃逸到字段/全局（`E0901`–`E0905`） | `src/sem/ownership.cpp` |
| 非空断言 `!!` | `val b = maybe(true)!!`：空指针在运行期 `abort()`（Kotlin 语义），结果类型去掉可空 | `NonNullExpr` / `codegen_exprs.cpp` |
| 多行字符串 | `"""` 包裹，原样保留换行与缩进；内部 `"` 不需要转义，`${...}` 模板照常生效 | `Lexer::tokenize` |
| 入口参数 | `fun main()`、`fun main(argc: i32)`、`fun main(argc: i32, argv: **u8): i32`、`fun main(argv: i32, argc: *cstr): i32`；带返回值的入口直接作为进程退出码 | `src/driver/main.cpp` |
| `if` / `while` 条件括号可选 + `when` | `if (cond) { … }` 与 `if cond { … }` 都接受 | `parse_if_stmt` / `parse_while` |
| 泛型函数的用户类实参 | `fun id<T>(v: T): T` + `id(point)` 按实参类型推断（不再只认内建类型） | `generic_subst_for_call` / `collect_generic_instantiations` |
| 跨模块/隐式模块依赖 | `math.square(5)` 自动加载 `math.sinter` | `ModuleLoader::load_entry` |
| 类型层级 `i8…i128` / `u8…u128` | 字面量后缀 `5i32` | lexer / `resolve_type_name` |
| 模块默认私有 | 只有 `pub fun` / `pub class` / `pub { … }` 内的声明进入头文件 | `DeclChunk::is_pub` / `generate_h` |
| 内部字符串 `str` | 编译期带长度的 `sinter_string`：拼接免 `strlen`、按需一次分配 | `sinter_runtime.h` / `str_cast` |
| C 字符串 `cstr` | `const char*`，与外部 C API 互操作；`std.string.toCStr` / `std.string.fromCStr` 双向转换 | `aliases_["cstr"]` / `upcast_if_needed` |
| `bool` 映射 `uint8_t` | 字面量生成 `1u` / `0u`，不再使用 `long long` | `CType::make_bool` |
| 运算符重载 | `fun operator+(other: Vec): Vec`、`operator==` / `-` / `*` / `/` / `%` / `<` / `<=` / `>` / `>=` | `parse_operator` / `find_binary_operator` |
| 可变参数 | `fun sum(xs: i64...)`，元素个数由 `xs__n` 给出 | `parse_parameters` / `expr_call` |
| 变长泛型类型参数包 | `fun show<Args...>(xs: Args...)`、`Args[i]`；异构实参逐参数展开为 `show_i64_str(int64_t xs0, sinter_string xs1)` | `parse_type` / `generic_pack_suffix` / `emit_function_def_generic` |
| 编译期目标常量 | `Target.os` / `arch` / `pointerBits` / `longBits` / `endian` / `isDebug`，在语义分析前折成字面量 | `src/front/comptime.cpp` |
| 编译期位置对象 `Site` | `Site.file` / `.line` / `.column` / `.module` / `.class` / `.struct` / `.function`：单态化之后折成字面量，可拼接、可传参、可放进 `comptime` | `src/front/comptime.cpp` |
| 编译期位置关键字（兼容） | `__FILE__` / `__MODULE__` / `__LINE__` / `__COLUMN__` / `__FUNCTION__` / `__CLASS__` / `__STRUCT__` 仍可用，语义等同 `Site.*` | 同上 |
| `comptime if` 条件裁剪 | `comptime if (Target.os == "linux") { A } else { B }`，未命中分支完全不进入语义分析与代码生成 | `src/front/comptime.cpp` |
| `comptime { val ... }` 常量块 | `comptime { val N = 1 + 2 }`，逐条编译期求值后**展开到外层作用域**，值落成字面量（未注类型时默认 `i64`） | `parse_comptime` / `ComptimeStmt::expand_const_block` |
| `comptime assert` 静态断言 | `comptime assert(Target.pointerBits == 64, "需要 64 位")`：失败在编译期报错并带用户提示文本，成功则整条删除、不生成 C | `parse_comptime` / `lower_comptime` |
| `when` 枚举穷尽检查 | subject 是 enum 且没有 `else` 时，漏掉任何变体都是编译错误（诊断点出缺哪个） | `Semantic::check_when_exhaustive` |
| `extern fun` 实现 C 函数 | `extern fun c_double(x: i32): i32 { … }`：符号名不带模块前缀、不导出头文件、参数必须是 C ABI 类型 | `parse_declaration_impl` / `FunctionDecl::is_extern_c` |
| 目标命令行覆盖 | `--target-os=` / `--target-arch=` / `--target-endian=` / `--release` / `--debug` / `-D os=freebsd` | `src/driver/main.cpp` / `parse_target_flag` |
| `bind C header` | `bind C header "native.h"`：编译期解析 C 头，里面的函数原型直接可调用，生成的 C 自动 `#include` 该头 | `src/driver/c_bridge.cpp` |
| `bind C struct` + `ptr` | `bind C struct FILE`、`fun f(p: ptr)`：不透明 C 类型只发前置声明、不生成定义，按值使用报错 | `ClassInfo::opaque` / `type_to_c` |
| `extern fun` ABI 校验 | 与 `bind C header` 登记的原型逐参数比对，不符报 `E0803`；一致时把实现挂到已有声明上 | `register_function` |
| 复制/移动赋值重载 | `fun operator=(other: T)` 与 `fun operator=(other: T&&)`：类里定义了就调用它（`T&&` 对类不加星，与 `T` 同构），否则默认逐字节赋值；一个类只写一个赋值重载 | `codegen_exprs.cpp` / `expr_assign` |
 `fun operator=(other: T): void` / `(other: T&&)`：类里定义了就调用它，否则默认逐字节赋值 | `codegen_exprs.cpp` / `expr_assign` |
| 反射式字符串化 | 每个 struct/class 自动生成 `<T>_to_str`；`${obj}`、`"x=" + obj`、`println(obj)` 自动调用，输出 `Type(field=value, ...)` | `codegen_lifecycle.cpp` / `str_cast` |
| 引用 `T&` | `fun inc(x: i64&) { x = x + 1 }`、`var r: i64& = &x`：非空、不可重绑定，读取/赋值自动解引用；`*r` 也接受；类引用 `Box&` 直接别名对象。未初始化或赋 `null` 报错 | parser / `typeck` / `codegen` |
| 类型转换 `as` | `p as *i64`、`p as i64`、`p as ptr`；生成 C 强制转换，配合指针/整数互转 | parser / `CastExpr` / `expr` |
| 指针运算 | `&x` 取地址、`*p` 解引用（可作左值）、`p ± n`、`p == q`；类型写 `*T`，与 C 的 `T*` 一一对应 | `parse_unary` / `typeck` / `expr_unary` |
| 尾随逗号 | `[1, 2, 3,]`、`fun f<T,>`、`class P<T,>`、`fun f(a: i64,)` | `parse_primary` / `parse_parameters` / `parse_class_like` |

## 指针

Sinter 的指针就是 C 的指针，没有额外的运行时语义：

```sinter
var n: i64 = 42
val p: *i64 = &n        // & 取地址，得到 i64*
println(*p)             // 42    —— * 解引用（可读）
*p = 99                 // 也可以写：*p 是左值
println(n)              // 99

val q = p + 1           // 指针算术按元素大小缩放（C 语义，i64* + 1 前进 8 字节）
val same = (p == &n)    // 指针比较
val raw: ptr = &n       // 任意指针都能赋给 ptr（void*），反向也成立
```

- 类型写法是 `*T`（与 C 的 `T*` 对应）；`ptr` 仍是 `void*` 的别名，用于不需要元素类型的场合。
- `*p` 的类型是「去掉一层 `*` 的 `T`」：`*i64` 得到 `i64`，`*sinter_string` 得到 `str`。
- 指针算术/比较直接落到 C，因此语义与 C 完全一致（`void*` 上的算术仍然非法）。
- 对非指针解引用报 `E0305`。
- 对**不存在**的成员调用报 `E0306`（`type 'X' has no member named 'y'`）。查的是
  「类 + 继承链 + 该类型的 `ext fun`」，三处都没有才报错；以前这种调用会生成一个
  丢了接收者的裸调用（`push(9)`），错误一直到 gcc 才变成
  `implicit declaration of function 'push'`。

## 反射：自动转字符串

编译器为**每个** struct / class 生成一个 `<模块>_<类>_to_str`，把实例渲染成
`Type(field=value, ...)`。字符串插值、`+` 拼接、`println` 都自动走它：

```sinter
struct Point(val x: i64, val y: i64) { }

class Box(val v: i64, val tag: str) { }

fun main() {
    val p = Point(3, 4)
    println("point=${p}")     // point=Point(x=3, y=4)
    println("box=" + Box(7, "hi"))   // box=Box(v=7, tag=hi)
}
```

字段按类型选转换：整数 `from_int`、浮点 `from_num`、`bool` `from_bool`、
`str` 原样、`cstr` `from_cstr`、指针打印地址、类/结构体字段递归调用它自己的 `to_str`。
静态字段不参与。

## 命名约定

- **C 名字不带参数类型**：函数是 `<模块>_<函数>`，方法是 `<模块>_<类>_<方法>`。
  唯一例外是构造函数（`String(str)` / `String(cstr)` 都得叫 `String`）与「函数名撞类名」
  的合成入口，它们保留 `__<签名>` 后缀，否则 C 的普通标识符命名空间会重复。
- 因为不再用类型区分符号，**同一个模块（或同一个类）里同名函数必须各自带别名**：
  写重载时给每个变体加 `[]`，如 `fun twice[twice_str](x: str)`。编译器按实参类型
  与接收者推导调用哪个，C 符号由编译器生成（带签名/别名片段）。
  **同名同签名**、或**某个同名函数漏了 `[]`** 都报 `E0204`，而不是留到链接期变成
  `undefined reference`。
- 生成 C 的模块前缀、函数、类、方法、枚举与静态字段统一使用
  `sinter_out_int` 这种 **snake_case**。Sinter 标识符（例如 `sortTape`）
  在生成 C 时会自动转换（`sort_tape`），不会把 camelCase 带进 C 符号。
- **形参与局部变量同样归一化**：`fun totalOf(aValue: i64)`、
  `class FooBar(val someValue: i64)`、lambda 形参 `fun(itemValue)` 以及
  `var myTotal = 0` 在 C 里都变成 `a_value` / `some_value` / `item_value` /
  `my_total`。Sinter 源码里仍可按自己的风格书写，归一化只影响生成 C。
- 编译器内部符号（`__ret`、`__tmpstr_*`、`self`、`this`）保持原样；
  形参归一化后的重名（同一形参表内）按位次加后缀去重。
- 标准库对外 API 与 Kotlin 对齐：**类型 PascalCase**（`Any` / `Int` / `Float` /
  `Bool` / `String` / `IntArray` / `FloatArray` / `List` / `Map`），
  **函数与方法 camelCase**（`printlnInt` / `isEmpty` / `indexOf` / `uppercase` /
  `compareTo` / `plus` / `joinToString`）。
- 模块内部 C 辅助函数仍是 `sinter_<模块>_<动作>`（C 侧的 snake_case 约定不变，
  生成符号不会被 camelCase 污染，见下节）。
- 运行时类型统一为小写 `sinter_string`，宏 `sinter_string_static` 也保持小写。
- `sinter_runtime.h` 提供可移植的 `sinter_maybe_unused` 宏（GCC/Clang 展开为
  `__attribute__((unused))`，其他编译器为空）。类头文件里的类型标识
  `<类>_type_name` / `<类>_type_chain` 都带这个标注并向内互相引用，
  用户开启 `-Wunused-const-variable`（含 `=2`）也不会看到提示。

## 对象模型

`std.core` 里有一个所有对象类型的根 `Any`（对应 Kotlin 的 `Any`）：

```sinter
pub class Any {
    fun toString(): str => "Any"
    fun typeLabel(): str => "Any"   // RTTI 类型名
}
```

`Int` / `Float` / `Bool` / `String` / `IntArray` / `FloatArray` 都写成
`class X : Any`，`Box` / `Pair` / `List` / `Map` / `HashMap` 也一样；用
`override` 覆盖 `toString()` / `typeLabel()`，通过 `Any` 形参调用时走虚表，
拿到的始终是真实类型的实现：

```sinter
fun describe(a: Any): str => a.typeLabel() + ":" + a.toString()
println(describe(Int(42)))        // Int:42
println(describe(String("hi")))   // String:hi

val xs = List<str>()
xs.add("ada")
println(describe(xs))             // List:[ada]
```

容器的 `toString()` 是 Kotlin 风格：`List` 是 `[a, b]`，`Map` / `HashMap`
是 `{k=v, ...}`，`Box` 是盒子里的值，`Pair` 是 `a=b` —— 都按类型实参分派到
对应的文本化函数，因此 `List<i64>` 出数字、`List<str>` 出字符串本身。

低层原语 `i64` / `f64` / `bool` / `str` 仍然是标量（零开销、无堆分配），
对象层是它们之上的包装：`Int(42)` / `Float(2.5)` / `Bool(true)` / `String("x")`。
`i64` 与 `Int` 之间的转换用 `Int(...)` 构造与 `.v` 取值。

## 标准库模块

`packages/std` 不再是一个大而全的模块，而是按职责拆分：

| 模块 | 内容 |
| --- | --- |
| `std` | **预置（prelude）**：输出（重载族 `println` / `print` / `eprintln`，按实参类型选 i64 / f64 / bool / str / cstr / Any；旧名 `printlnInt` / `printlnFloat` / `printlnBool` / `printlnAny` 保留为兼容包装）、数学（`sqrt` / `pow` / `floor` / `ceil` / `round` / `PI`）、`minOf` / `maxOf` / `abs` / `coerceIn`。只 `import std.core`，**不**带其余子模块 |
| `std.core` | `Any` 与基本类型的对象形式 `Int` / `Float` / `Bool`；整数上下界盒子 `I64` / `I32` / `U32` / `U64`（`static fun BITS()` / `MAX_VALUE()` / `MIN_VALUE()`，值来自 `comptime` 常量，调用点直接折成字面量） |
| `std.string` | `String`：`length` / `isEmpty` / `get` / `substring` / `indexOf` / `startsWith` / `endsWith` / `uppercase` / `lowercase` / `trim` / `repeat` / `replace` / `compareTo` / `plus` / `toInt` / `toFloat` / `toCStr`，以及同名的 `str` 扩展函数 `pub ext fun str.xxx()` |
| `std.arrays` | **泛型定长数组** `Array<T>`（`data` / `size` / `owned` 字段；`isEmpty` / `get` / `set` / `fill` / `sum` / `indexOf` / `contains` / `sort` / `reverse` / `copyOf` / `blank` / `joinToString`）与 `ByteArray`（u8 版，布局相同）；Kotlin 风格工厂 `arrayOf(...)` / `intArrayOf(...)` / `intArray(n, init)` / `floatArrayOf(...)` / `floatArray(n, init)` / `byteArrayOf(...)` / `byteArray(n, init)`。元素动作（分配 / 填充 / 排序 / 反转 / 拷贝 / 求和 / 下标 / 文本）按 **T 分派**：`Array<str>` 的排序走字典序、`get` 返回克隆、析构逐元素释放。**算法**：`sortInt` / `reverseInt` / `sumInt` / `minInt` / `maxInt` / `binarySearchInt` / `lowerBoundInt` / `isSortedInt` / `sortStr` / `binarySearchStr` / `sliceInt` / `shuffleInt`，以及接收函数参数的 `forEachInt` / `mapInt` / `filterInt` / `reduceInt` / `countIfInt` / `sortByInt` / `forEachStr` |
| `std.generic` | 泛型容器的**元素动作分派表**（`sinter_gen_clone` / `free` / `drop` / `grow` / `hash` / `cmp` / `blank` / `text`）与文本构建器 `sinter_gen_sb_*`。不给用户直接调用，容器模块 import 它 |
| `std.box` | `Box<T>`：`get` / `set` / `blank` / `describe` / `toString` |
| `std.pair` | `Pair<A, B>`：`component1` / `component2` / `replaceSecond` / `swap` / `toText` / `toString` |
| `std.list` | `List<T>`（`T` 可以是内建类型、`str` 或用户类；类元素按**借用指针**存储，clone/drop 不转移所有权）：`size` / `capacity` / `add` / `get` / `set` / `at` / `last` / `addAt` / `removeAt` / `removeLast` / `indexOf` / `contains` / `clear` / `isEmpty` / `toString` |
| `std.map` | `Map<K, V>`：**有序**（键按 `sinter_gen_cmp` 排，二分查找）——`put` / `get` / `getOr` / `remove` / `containsKey` / `keyAt` / `valueAt` / `size` / `toString` |
| `std.hashmap` | `HashMap<K, V>`：**哈希**（开放寻址，装载因子 3/4 时翻倍重建）——`put` / `get` / `getOr` / `remove` / `containsKey` / `size` / `toString`。桶位用 `sinter_gen_hash` 与结点同源，`str` 键与 `str.hashCode()` 是同一个 FNV-1a 64 |
| `std.io` | **标准流**：`eof` / `readLine` / `readAll` / `readByte`（stdin，按字节缓冲，EINTR 重试）；`write` / `writeErr` / `writeLine` / `writeErrLine` / `writeChar` / `writeErrChar` / `writeBytes` / `writeErrBytes`（write(2) 直写，无用户态缓冲，`flush` 是空实现）。EOF 语义：`readLine` 在 EOF 返回空串，是否结束看 `eof()` |
| `std.filesystem` | **兼容层**：把 `std.file` 与 `std.path` 重新导出。老代码 `import std.filesystem` 后写 `readText(path)` / `join(a,b)` 照常工作；新代码直接 import 那两个模块（`std.path` 不会把 `open` / `stat` / `dirent` 编进产物） |
| `std.path` | **纯字符串路径运算**（不碰磁盘）：`join` / `joinAll` / `join3` / `isAbsolute` / `isRelative` / `hasTrailingSep` / `fileName` / `parentOf` / `extension` / `stem` / `changeExtension` / `normalize`（压 `//`、`.`、按字面量消解 `..`）/ `segmentCount` / `segmentAt` / `containsSegment` / `cwd` / `absolute` |
| `std.file` | **`File` 类**（Kotlin 风格）+ 同名自由函数。`File(path)` 惰性、不碰磁盘；`path` / `name` / `parent` / `extension` / `nameWithoutExtension` / `isAbsolute` / `absolutePath` / `canonicalPath` / `withExtension` / `withParent` / `child` 是纯字符串运算；`exists` / `isFile` / `isDirectory` / `length` / `lastModified` 查磁盘；`readText` / `writeText` / `appendText` 读写；`removeFile`（`delete` 是保留字）/ `mkdir` / `mkdirs` / `renameTo` / `renameToPath` 变更；`list` / `listCount` / `listSortedText` / `entryAt` / `filesText` / `childrenText` 遍历。自由函数与 `std.filesystem` 同名同义 |
| `std.stream` | **流**：接口 `InputStream`（`read` / `close`）与 `OutputStream`（`write` / `flush` / `close`），接口方法进 vtable，按抽象类型写的 `copyStream` 能分派到具体实现。实现 `FileInputStream` / `FileOutputStream(path, append)` / `MemoryInputStream(text)` / `MemoryOutputStream()`（`toText()` 取内容，不重写 `toString`）/ `LineReader(bufSize)` + `openFile` / `useFile` / `openText`（`readLine` / `atEnd` / `readRest`）。便捷层 `copyStream` / `readAllBytes` / `readAllText` / `writeAllText` / `appendAllText`。全部 EINTR 重试 + 短写补齐 |
| `std.net` 连接包装 | `TcpListener(host, port, backlog)`（`acceptFd` / `localPort` / `close`）、`TcpConnection(fd)`（`read` / `readLine` / `readAll` / `write` / `writeLine` / `setTimeout` / `close`，内部缓冲，RAII 关闭） |
| `std.thread` | **pthread 薄封装**：`Thread(fn, arg)`（`fn` 是 `(ptr) -> ptr`，可传顶层函数或 lambda；`join` / `detach` / `isAlive`）、`Mutex`（`lock` / `tryLock` / `unlock`）、`Condvar`（`wait` / `signal` / `broadcast`）、`sleepMs` / `yieldThread` / `currentThreadId`，以及 `ptrFromInt` / `intFromPtr` 用于给线程传参 |
| `std.async` | **线程池**：`Executor(n)` + `submit(fn, arg)` 提交 `(ptr) -> ptr` 任务，`shutdown()` 等待所有已提交任务结束；每个 Executor 有独立队列，`newExecutor(n)` 是工厂 |
| `std.result` | **可恢复错误**：`Result<T>` 的 `isOk` / `isErr` / `expect(msg)` / `getOr(fallback)` / `errorOr(fallback)`；`okInt` / `errInt` / `okStr` / `errStr` 工厂。语言级异常用 `try { } catch (e: str) { } finally { }` 与 `throw "msg"`；未捕获异常和 `panic` / `assert` 会打印消息、源码位置与 backtrace 后 `abort()` |
| `std.option` | Rust/Kotlin 风格 `Option<T>`：`someInt` / `noneInt` / `someStr` / `noneStr` / `someBool` / `noneBool`，`isSome` / `isNone` / `getOr` / `expect` / `orElse` |
| `std.text` | `StringBuilder`（`append` / `appendByte` / `appendInt` / `appendFloat` / `appendBool` / `appendLine` / `newline` / `clear` / `length` / `toString`），`split` / `splitLines` / `joinToString` |
| `std.math` | `absInt` / `absFloat` / `signInt` / `minInt2` / `maxInt2` / `clampInt` / `gcd` / `lcm` / `isPrime` / `factorial` / `fibonacci` / `lerp` / `toRadians` / `toDegrees` / `square` / `cube` |
| `std.time` | `nowSeconds` / `nowMillis` / `cpuMillis`、`Stopwatch`（`restart` / `stop` / `elapsedMillis`） |
| `std.random` | xorshift64* `Random(seed)`：`nextInt` / `nextIntRange` / `nextFloat` / `nextBool` / `shuffleInts`，可复现 |
| `std.set` | `StrSet` / `IntSet`（基于 HashMap）：`add` / `contains` / `remove` / `size` / `isEmpty` / `clear` |
| `std.env` | `getenv`（返回 `Option<str>`）/ `hasEnv` / `exit` |
| `std.net`（独立 `net` 包） | **TCP/UDP**：`tcpListen` / `tcpAccept` / `tcpConnect` / `tcpSend` / `tcpRecv` / `tcpRecvAll` / `tcpRecvLine` / `tcpClose` / `tcpShutdown` / `setSocketTimeout` / `socketLocalPort` / `socketLastError`；`udpBind` / `udpSendTo` / `udpRecvFrom` / `udpLastPeerHost` / `udpLastPeerPort`。套接字是 `i64` 描述符，阻塞语义，DNS 由 `getaddrinfo` 解析；Linux 走 BSD sockets，Windows 走 Winsock2（`ws2_32`） |
| `std.sys` | 低层 OS 原语：socket 创建/连接/收发/关闭/超时/本机端口、UDP；供 `net` 使用，业务代码通常不直接用 |
| `std.binary` | 低层二进制原语：`inflate` / `deflate`（raw DEFLATE）、`bunzip2`、`unzstd`、`crc32`、`zipCryptEncrypt` / `zipCryptDecrypt`（传统 PKZIP）、`hexEncode` / `hexDecode`、`base64Encode` / `base64Decode`、`ByteBuffer`（`appendByte` / `appendU16` / `appendU32` / `bytes`）；供 `zip` 使用 |
| `std.algo` | 基础算法：`uniqueInt(xs, out)` / `rotateInt` / `partitionInt` / `nthSmallestInt` / `levenshtein` / `isPalindrome` / `longestCommonPrefix` / `countOccurrences` |
| `algo`（独立包） | **算法库**：`algo.aes` 的 `aesBlockEncrypt` / `aesBlockDecrypt`、`aesCbcEncrypt` / `aesCbcDecrypt`（PKCS#7）、`aesCtr`；纯 C 实现 FIPS-197，不依赖 OpenSSL |
| `json`（独立包） | **JSON**：`Json.parse` / `compact` / `pretty`；节点访问 `root` / `kind` / `size` / `indexAt` / `get` / `asString` / `asInt` / `asFloat` / `asBool`；构造 `objectValue` / `arrayValue` / `stringValue` / `intValue` / `floatValue` / `boolValue` / `nullValue` 与 `put*` / `add*`；支持转义、`\uXXXX` 与代理对 |
| `toml`（独立包） | **TOML**：`Toml.parse` / `serialize`；表、数组表、点号键、引号键、基本/字面/多行字符串、整数（十/十六/八/二进制）、浮点、布尔、数组、内联表；四种日期类型（offset/local date-time、local date、local time）结构化存储，支持纳秒、时区偏移、闰日/闰秒校验；注释保留模型（`leadingTrivia` / `trailingComment` / `documentTail`，多行数组保留元素注释与尾随逗号）；构造 `offsetDateTime` / `localDateTime` / `localDate` / `localTime`，访问 `dateYear` / `dateHour` / `dateNanosecond` / `dateOffsetMinutes` 等 |
| `zip`（独立包） | **ZIP 归档**：`zipNames` / `zipCount` / `zipContains` / `zipGet` / `zipGetPassword`（stored、DEFLATE、bzip2、zstd；ZipCrypto 解密）、`ZipReader`（`next` / `name` / `size` / `bytes` 逐条目惰性解压，`openFile` 按需 seek，`openPassword` / `openFilePassword`）、`ZipWriter`（stored/DEFLATE、`setPassword` ZipCrypto 加密、ZIP64 写入；内存 `finish` 或 `begin(path)` + `add` + `finishTo()` 流式写文件）；ZIP 结构与解压实现均在 Sinter / `std.binary`，不依赖 zlib/minizip |
| `std.memory` | **底层内存与指针**（薄包装，无额外分配）：`alloc` / `allocZero` / `realloc` / `dealloc` / `copy` / `move` / `fill` / `compare` / `offset` / `distance` / `ptrToInt` / `intToPtr` / `pointerSize` / `isNull`；定宽读写 `readI8`…`readI64` / `readU8` / `readU16` / `readU32` / `readU64` / `readF64` 与对应 `write*`；`load<T>` / `store<T>` / `ptrAt<T>` 类型化指针助手（跨模块泛型可用） |

### 按需导入（1.0 起）

`import std` **只**带来预置层。其余模块必须显式 import，否则其代码不会进入你的
工程 —— 这是刻意的：不用 `List` 就不会把容器实现编进产物。

```sinter
import std                  // println / printlnInt / sqrt …
import std.string           // "x".trim() 这类 str 扩展与 String 类
import std.arrays           // Array<T> / ByteArray + 三个 Kotlin 风格工厂
import std.box              // Box<T>
import std.pair             // Pair<A, B>
import std.list             // List<T>
import std.map              // Map<K, V>       有序，二分
import std.hashmap          // HashMap<K, V>   哈希，开放寻址
import std.io               // 标准流：readLine / write / writeLine …
import std.path             // 纯字符串路径：join / extension / normalize …
import std.file             // File("/a.txt").readText() / writeText …
import std.stream           // InputStream / OutputStream / LineReader
import std.filesystem       // 兼容层：转发到 std.file + std.path
```

`sinter build` 打印的 `[module]` 行就是实际参与编译的模块清单，可以直接用来核对
按需导入有没有生效（只 import 预置层时只应看到 `std` 与 `std.core`）。

模块也可以点分调用：`std.string.trim(s)`。容器没有「i64 版」和「str 版」两套：
只写一份泛型定义，按使用点的类型实参单态化。

### 扩展函数的三条硬规则

1. **扩展函数与普通函数共用同一个模块命名空间，同名时要给每个重载配别名**。
   Sinter 支持重载：同名函数各自带一个 `[]` 别名，编译器按**实参类型 + 接收者**
   推导该调哪一个；C 侧符号由编译器生成（带签名/别名片段），因此不会撞车。

   ```sinter
   fun twice(x: i64): i64 => x * 2
   fun twice[twice_str](x: str): str => x + x        // 重载：靠 [] 区分符号

   ext fun i64.twice[twice_i64](): i64 => self * 2   // 接收者也算签名的一部分
   ```

   真正的冲突只有两种：**同名同签名**、或**有一个同名函数没写 `[]`**（它会被拼成
   `<模块>_<名字>`，与先来的那个撞车）。两种情况都报 `E0204`，不会留到链接期。
   扩展函数的接收者计入签名，所以 `ext fun i64.twice()` 与 `fun twice(x: i64)`
   不算同签名 —— 调用形式不同（`5.twice()` / `twice(5)`）。
2. **`ext fun` 忘了写 `pub` 就是模块私有** —— 跨模块调用不会报错，只会生成一个
   隐式声明的裸调用，链接期才炸成 `undefined reference`。标准库里 17 条 `str`
   扩展全部带 `pub`。
3. **扩展函数的返回类型要能被 `c_type_of_expr` 查到** —— 生成 C 时按接收者类型
   匹配 `ext_receiver`，命中了才用声明上的返回类型；查不到就退回 `long long`，
   字符串会被当成整数传给 `sinter_string_from_int(...)`，C 阶段报
   `aggregate value used where an integer was expected`。这也是 `1.0.0` 修掉的
   一个缺陷（见 `CHANGELOG.md`）。

### 按需导入与漏 import 的诊断

`str` / `i64` 这类内建类型的方法来自 `std.string` 等子模块的 `ext fun`。忘了
import 时，`s.length()` 以前会被原样生成成 `length()`（丢了接收者），到 gcc 才报
`implicit declaration of function 'length'` —— 完全看不出是缺 import。现在语义分析
阶段就拦下，报 `E0306` 并直接告诉你补哪个 import：

```
src/main.sinter:6:16: error[E0306]: type `str` has no member named `length`
  = hint: no `ext fun str.length` is in scope; if it comes from the standard library,
          add the matching import (`std.string` for `str` helpers)
```

`IntArray` 有两种构造方式：工厂（`intArrayOf` / `intArray`，`owned = true`，
析构时释放）与视图包装 `IntArray(xs, n, false)`（包装已有 `i64[N]`，不接管释放）。

## 生成代码约定

- 每个模块生成 `sinter_<模块>.c` + `sinter_<模块>.h`。
- 函数命名：`<模块>_<函数>__<签名>`；方法：`<模块>_<类>_<方法>__<签名>`，
  各段都会先转成 snake_case。
- 仅返回类型不同的重载分支加 `_ret_<类型>` 后缀。
- 泛型在 `.c` 内单态化为 `_<类型>` 后缀，先出原型再出定义。
- 泛型类 / 泛型结构体在**语义分析之前**单态化：`class Box<T>` 本身不产生任何
  代码，每个使用点 `Box<i64>` 会在**声明模板的那个模块**里生成一个普通类
  `Box_i64`（`generics_box_i64`…），自此完全按普通类处理 —— RAII 析构、继承、
  虚表、复制/移动构造、跨模块 `pub` 导出都自动可用。
  - 命名规则：`Box<i64>` → `Box_i64`，`Pair<str, i64>` → `Pair_str_i64`；
    指针/可空/数组分别加 `_ptr` / `_opt` / `_arr`。
  - 使用处必须显式书写类型实参：写 `Box()` 或 `val b: Box` 会报
    「泛型类型 `Box` 需要显式类型实参，例如 `Box<i64>`」。
  - `<T must number>` 在实例化时校验，违反会报「第 N 个类型实参不满足 `must number`」。
  - 模板内部自引用（`class Node<T>(...)` 里的 `next: Node<T>?`）会改写成
    `Node_i64`，因此链表/树这类递归结构可以直接写。
  - 跨模块使用时，使用方模块会被自动补上声明模块的 `import`。
- 类布局：`sinter_vfn* __vptr` + 基类嵌入 `__base` + 字段；构造用 `X_new`，
  析构用 `X_del`，复制/移动为 `X_copy` / `X_move`，虚调用走 `__vptr[slot]`。
  槽位类型是函数指针 `typedef void (*sinter_vfn)(void)`（不是 `void*`），
  调用点 `((T(*)(...))obj->__vptr[i])(args)` 属于「函数指针 → 函数指针」的转换，
  ISO C 6.3.2.3 允许，因此生成代码在 `-std=c99 -pedantic-errors` 下零报错。
- 运行时 **单独生成** `sinter_runtime.h` + `sinter_runtime.c`（每个构建输出目录一份），
  不再嵌入各模块文件；模块 `.c` / `.h` 统一 `#include "sinter_runtime.h"`。
- 头文件用 `#ifdef __cplusplus extern "C" { … }` 包裹，可直接被 C++ 编译器包含。
- 析构时机：作用域结束、`return`、`break`、`continue`。`return` 先求值到临时变量
  （`__ret_N`）再做清理，避免返回表达式引用已析构对象；清理语句形如
  `if (x) { X_del(x); x = 0; }`，置空保证重复清理不会二次释放。
  提前退出已清理过的层会被记录，块结束时不再重复输出。`del x` 会把该变量从
  清理表中摘除，因此 `del` 与自动析构互斥。
- 声明分流：`pub` 声明进 `.h`；非 `pub` 声明进 `.c` 的“模块私有声明”段；
  vtable 定义只出现在 `.c`。
- 内部字符串在 `sinter_runtime.h` 中定义为
  `typedef struct sinter_string { char* data; int64_t len; int64_t cap; int32_t owned; } sinter_string;`，
  `data` 始终 NUL 结尾，可直接当 C 字符串使用；`owned` 标记缓冲归属
  （1 = 堆内存需释放，0 = 指向静态字面量池）。
- **生成代码不引入任何系统头文件。** 模块 `.c` / `.h` 只 `#include "sinter_runtime.h"`；
  `sinter_runtime.h` 自带定宽类型与 `size_t`（用 `__INT64_TYPE__` / `__SIZE_TYPE__`
  等编译器内置宏，避免与任意 libc 的定宽类型冲突）、自带 `NULL`，
  并只声明不含 `FILE*` 的 libc 函数。`std` 包同样零系统头文件，
  输出走自行声明的 `write(2)`。唯一例外是运行时实现文件 `sinter_runtime.c`，
  它为一处浮点 `%g` 格式化保留 `<stdio.h>`。
  需要 `printf` / `FILE*` 的用户代码请自行 `embed C { #include <stdio.h> }`。
  唯一例外写在 Sinter 源码里的 `bind C header "x.h"`：那一条会真的生成
  `#include "x.h"`（位置在 `sinter_runtime.h` 之后、模块头之前），
  因为契约就是那个头本身，见「C 互操作」。

## C 互操作

Sinter 调 C、C 调 Sinter，都不需要手抄原型：头文件本身就是唯一契约。

### `bind C header`

```sinter
bind C header "native.h"        // 路径按「模块目录 → -I 列表」顺序解析

pub fun main() {
    println(native_add(2, 3))   // 直接用头里的名字调用，不用再写 bind C fun
}
```

编译期做三件事：

1. 用 `src/cfront` 解析头（预处理 + 声明解析），把每个函数原型登记成一条
   `extern` 函数 —— 符号名不带模块前缀，也不导出到模块头文件。
2. 记住这个头，生成 `<模块>.c` 时在 `sinter_runtime.h` 之后、模块头之前
   `#include "native.h"`。**这是生成代码唯一会引入的用户头文件。**
3. 把头里的 `struct` / `union` tag 记成「外部类型」，供后续显式引用。
   `enum` 只按 `i32` 处理，常量本轮不逐个登记（见「已知限制」）。

一条只处理一个头，可以写多条。头找不到报 `E0801`，hint 提示加 `-I`，
或改成相对 `.sinter` 文件的路径。

### 类型映射（头 → Sinter）

| C 类型 | Sinter 类型 | 说明 |
|---|---|---|
| `int8_t`…`int64_t` / `uint8_t`…`uint64_t` | `i8`…`i64` / `u8`…`u64` | 定宽类型直通 |
| `int` / `signed int` | `i32` | |
| `long` | `i64` | 按 LP64 目标；非 LP64 目标见「已知限制」 |
| `size_t` / `uintptr_t` / `uintmax_t` | `u64` | |
| `ssize_t` / `intptr_t` / `ptrdiff_t` / `time_t` / `off_t` | `i64` | |
| `float` | `f32` | 必须显式走 `f32`：Sinter 的 `float` 别名是 `double`，映射错会让实参在 C 侧被悄悄提升 |
| `double` / `long double` | `f64` | |
| `char*` / `const char*` | `cstr` | |
| `void*` | `ptr` | 内建类型，就是 `void*` |
| `enum X`（作参数 / 返回值） | `i32` | C 的枚举实体就是 `int`；枚举常量本轮不登记 |
| `struct X*` / `X*` | `X` | Sinter 的类类型本身就是指针 |
| `struct X`（按值） | —— | 拒绝，报 `E0806`：C 侧布局未知 |
| 函数指针 | `ptr` | 只能传递，不能从 Sinter 侧解引用调用 |

映射表里没有的类型不会被「猜一个宽度」，一律 `E0806` —— ABI 猜错是静默错误，
比编译失败难查得多。

### `bind C struct` 与不透明类型

```sinter
bind C struct FILE              // 只登记，不生成定义
```

含义是「布局由 C 侧决定，Sinter 只知道它存在」。语义上它与「别的模块的类」
同类：拼写由对方给出、本模块不需要布局。生成 C 时只发 `struct X;` 前置声明，
**绝不发定义**；按值使用直接报错，只能通过 `X`（本身就是指针）或 `ptr` 传递。

### `extern fun` 与头里的原型

```sinter
bind C header "scale.h"         // 头里：int32_t c_scale(int32_t);

extern fun c_scale(x: i32): i32 {   // 实现写在 Sinter 里，由 C 侧调用
    return x * 3
}
```

同名原型已由 `bind C header` 登记时，编译器逐参数比对（个数 / 每个参数的
`type_c` / 返回类型），全一致才放行；不一致报 `E0803`，并指出是第几个参数、
两边各是什么类型：

```
main.sinter:8:12: error[E0803]: 'c_scale' does not match the C prototype: parameter 1 is 'i32' in the header, but 'i64' here
```

一致时不会重复登记第二个函数，而是把实现挂到已有声明上（形参名换成 Sinter
侧的名字，`is_extern` 清掉让 codegen 生成定义）—— 否则 C 侧只看到声明没有
定义，链接期报 `undefined reference`。

### 错误码

| 码 | 含义 |
|---|---|
| `E0801` | 找不到 `bind C header` 指定的头文件 |
| `E0803` | `extern fun` 与 C 头原型不匹配 |
| `E0806` | 类型无法映射（含按值传 `struct`） |

### cfront 的实测覆盖

解析器不是「能过自测」就算完：拿宿主上真实的 musl 头文件当语料扫过一遍
（`/usr/include` 下 270 个 `.h`，纯 C 的 259 个）：

| 头 | 结果 |
|---|---|
| `stdio.h` / `stdlib.h` / `string.h` / `time.h` / `unistd.h` / `fcntl.h` / `pthread.h` / `math.h` / `errno.h` / `limits.h` / `stdint.h` / `stddef.h` / `signal.h` / `sys/stat.h` / `sys/socket.h` / `netinet/in.h` | 0 解析错误 |
| `lcms2.h`（109 KB，500+ 声明） | 0 解析错误 |
| 全部 259 个纯 C 头 | 219 个 0 错误；剩余集中在 `wayland-*-protocol.h`、`xf86drm.h` 这类「代码生成器拼出来的」头 |

复现方式（自测二进制自带扫描模式，不进 ctest，因为依赖宿主头文件）：

```sh
cfront_selftest --parse /usr/include/stdio.h /usr/include/math.h /usr/include/lcms2.h -I/usr/include
# 每个文件打印 items / errors，全部干净时以 0 退出
```

这一轮靠它揪出并修掉的真实缺陷（都不是自测能覆盖的）：枚举最后一项没有尾逗号时
把 `}` 之后整份文件吃进 value（解析器一路跑到 EOF）、函数式宏最后一个形参永远
不参与替换、`#`/`##` 原样流进语法层、`bits/alltypes.h` 被「同文件只展开一次」
跳过导致 `clock_t` 丢失、指令里的跨行块注释把 `*/` 变成代码。
错误现在还会归属到**真正出错的文件**（`CParseError::file`），不再一律算在外层头头上。

### 已知限制

- 不展开系统头：`<stdint.h>` 这类只在 `-I` 里找，找不到不报错（定宽 typedef
  由内置表兜底），真正的缺失留到 C 编译阶段暴露。
- 不做 C++：`cursesf.h`、`form.h` 这些其实是 C++ 头（`class` / `template`），
  解析失败属预期。
- `long` 固定按 64 位映射，`Target.longBits == 32` 的目标上会偏宽。
- 不支持 K&R 老式形参、VLA、flexible array member、匿名 `struct`/`union` 成员。
- `#` / `##` 做「够用」的实现：`#param` 字符串化、`a ## b` 拼接后合并拼写；
  拼接结果不再做 token 校验。孤立 `#` / `##` 会被丢掉而不是流进语法层。
- `#include MACRO`（宏展开成头名）不支持。
- `_Generic` 只保留关联类型原文，不求值。
- 函数指针只能解析与传递，不能从 Sinter 侧解引用调用。
- `enum` 常量不登记：头里的枚举值只能当 `i32` 传，不能在 Sinter 侧按名字引用
  （需要时用 `comptime { val X = 3 }` 手抄，或加一条 `bind C fun` 取值函数）。

## 编译期特性

### 目标常量

`Target` 是编译器内置的编译期常量对象，只在表达式里使用，不需要 import：

| 常量 | 类型 | 说明 |
| --- | --- | --- |
| `Target.os` | `str` | `android` / `linux` / `windows` / `macos` / `unknown` |
| `Target.arch` | `str` | `arm64` / `arm` / `x86_64` / `x86` / `unknown` |
| `Target.pointerBits` | `i64` | 指针位宽（32 / 64） |
| `Target.longBits` | `i64` | C `long` 的位宽（Windows 是 32，其余常见 64） |
| `Target.endian` | `str` | `little` / `big` |
| `Target.isDebug` | `bool` | 默认 `false`，`--debug` 置真 |

它们在**语义分析之前**折成普通字面量，不进生成代码，也没有运行时开销。

### 源位置：`Site`

要「当前在哪」就写 `Site.<字段>` —— 它是编译期对象，用起来和普通成员访问一样：

```sinter
fun log(msg: str) {
    println("[" + Site.module + "] " + Site.function + "@" + Site.line + ": " + msg)
}

class Order(val id: i64) {
    fun where(): str => Site.class + "." + Site.function   // Order.where
    fun asStruct(): str => Site.struct                     // 结构体名（struct 里才有）
}
```

| 字段 | 类型 | 含义 |
|---|---|---|
| `Site.file` | `str` | 源文件路径 |
| `Site.line` / `.column` | `i64` | 该表达式所在行列 |
| `Site.module` | `str` | 模块名 |
| `Site.function` | `str` | 最近的外层函数/方法名（不在函数里则不可用） |
| `Site.class` / `.struct` | `str` | 最近的外层类/结构体名（类型不匹配则不可用） |

折叠发生在单态化之后、语义分析之前：泛型模板体里的 `Site.class` 取的是**实例化后**的外层类型，
和直觉一致。`Site.line` 之类也能直接进 `comptime if (Site.line > 10)`。
旧的 `__FILE__` 一族保留为兼容写法，新代码建议统一用 `Site`。

### 源位置关键字（兼容）

编译器内置七个「我在哪」关键字，直接写在表达式里，不需要 import：

| 关键字 | 类型 | 值 |
| --- | --- | --- |
| `__FILE__` | `str` | 当前源文件（编译器看到的路径；`sinter` 构建时是绝对路径，分隔符统一为 `/`） |
| `__MODULE__` | `str` | 当前模块名（`module foo` 里的 `foo`） |
| `__LINE__` | `i64` | 关键字所在行（1 基） |
| `__COLUMN__` | `i64` | 关键字所在列（1 基） |
| `__FUNCTION__` | `str` | 最近的外层函数 / 方法名 |
| `__CLASS__` | `str` | 当前 class 名 |
| `__STRUCT__` | `str` | 当前 struct 名 |

```sinter
class Point(val x: i64, val y: i64) {
    fun describe(): str {
        return "Point.describe @ ${__FILE__}:${__LINE__}"
    }
}

fun main() {
    println("module=${__MODULE__} fn=${__FUNCTION__}")
    println("line=${__LINE__} col=${__COLUMN__}")

    val p = Point(1, 2)
    println(p.describe())
}
```

语义要点：

- 它们是**关键字**，不是标识符：`val __LINE__ = 1` 不合法。
- 折叠时机在**泛型单态化之后、语义分析之前**，所以：
  - `__CLASS__` / `__STRUCT__` 在 `Box<T>` 的实例体里给出实例名
    （`Box_i64` / `Box_str`），而不是模板名 `Box`；
  - `__LINE__` / `__COLUMN__` 指向模板里真正写了这个关键字的那一行。
- `__FUNCTION__` 只给**声明的名字**（方法给 `describe`，不是 `Point.describe`），
  需要前缀就自己拼 `__CLASS__`。lambda 体里沿用最近的外层命名函数。
- 泛型**函数**的按调用点特化发生在代码生成阶段，因此特化体里的
  `__FUNCTION__` 是声明名（`show`），不是 `show_i64`。
- `__LINE__` / `__COLUMN__` 在字符串插值 `"${__LINE__}"` 里同样指向真实源码：
  插值片段由子解析器解析，位置会平移回外层源码（不再退化成 1:1）。
- 上下文用错是**编译错误**，不会给出空串：
  - `__FUNCTION__` 在函数体之外 → 报错；
  - `__CLASS__` 在 struct 体内或任何类型之外 → 报错，并提示改用 `__STRUCT__`；
  - `__STRUCT__` 在 class 体内 → 对称报错。
- 折叠后就是普通字面量：没有运行时开销，生成的 C 里不会残留关键字拼写。

### `comptime if` 条件裁剪

```sinter
comptime if (Target.os == "linux") {
    println("posix path")
} else {
    println("other path")
}
```

条件必须是编译期常量——只允许字面量、`Target.*`、`__FILE__` / `__LINE__`
等源位置关键字，以及它们之上的
`+ - * / %`、`== != < <= > >=`、`&& || !`、字符串 `== / !=`。
未命中的那一支完全不进入语义分析与代码生成（泛型也不会被它实例化）。
条件无法编译期求值时（例如调用普通函数）会报错，不会默默选一支。

### 命令行覆盖

```sh
sinterc main.sinter -o out --target-os=windows --debug
sinterc main.sinter -o out -D os=freebsd -D arch=x86_64
```

`-D` 的 key 与 `Target` 字段名一致：`os` / `arch` / `endian` /
`pointerBits` / `longBits` / `isDebug`，也接受 `target.os` 这种带前缀的写法。

### `comptime { }` 常量块

```sinter
comptime {
    val PageSize = 1024 * 4        // 编译期算好，落成 4096
    val TablePages = PageSize * 8  // 可以引用块内前面的常量
}
println("${PageSize}")
```

- 逐条求值，值折成字面量，**展开到外层作用域**（块后面的代码能直接写 `PageSize`）。
  生成的 C 是 `const int64_t page_size = 4096LL;`——编译期算一次，运行期零成本。
- 没写类型注解时默认 `i64`。原因是常量块常用于容量/掩码/表大小，
  按 i32 推断会让 `3e9` 这种值悄悄溢出。
- 块内只允许 `val` / `const`。控制流用 `comptime if`，普通代码请放到块外；
  违反会明确报错（不会静默丢弃）。

### `comptime assert` 静态断言

```sinter
comptime assert(Target.pointerBits == 64, "本库只支持 64 位目标")
```

- 条件必须是编译期常量（字面量、`Target.*`、源位置关键字，以及它们之上的
  算术 / 比较 / 逻辑）。
- 成立时整条语句被删除，生成的 C 里没有任何残留。
- 失败时报 `文件:行:列: comptime assert 失败：<你的提示文本>`；
  条件本身不能求值时也会报错并说明原因（例如引用了运行时变量）。

### `when` 的枚举穷尽检查

```sinter
enum Color { Red, Green, Blue }

val n = when (c) {
    Red => 1
    Green => 2
}   // 错误：when 未穷尽：`Color` 还缺少分支 Blue
```

- 只对**能确定是 enum** 的 subject 检查：变量类型已知，或所有 `Value` 模式
  都指向同一个枚举的变体。
- 有 `else` 视为穷尽；`in` / `is` 等非枚举模式不参与判定。
- 宁可漏报也不误报：类型推不出来时静默跳过（与 typeck 的保守原则一致）。

### 当前不支持

- 编译期函数（`comptime fun`）、编译期容器、`comptime` 递归展开不在本轮范围内。
- `comptime { }` 块里不能引用运行时变量，也不能放控制流（用 `comptime if`）。

### 语法糖：尾随逗号

四处都接受多余的尾随逗号，方便换行书写与 diff：

```sinter
val xs: i64[3] = [1, 2, 3,]
class Point<T,>(val x: T, val y: T,) { }
fun add<T,>(a: T, b: T,): T => a + b
```

数组字面量需配合定长声明 `i64[N]` 使用。Sinter 的数组是堆上的 `T*`，
字面量会 `malloc` 一份再 `memcpy` 过去，作用域结束时释放。

## 第二个 C 前端（cfront2）

仓库里有两套独立 C 前端：

- `src/cfront`：快速、容错，服务于 `embed C` 与 `bind C header`；
- `src/cfront2`：独立的词法 + C99 递归下降解析（声明符、struct/union/enum、
  typedef、完整表达式与语句，兼容常见 GCC `__attribute__` / `__extension__`），
  通过 `sinterd cparse2` / `symbols2` 暴露。它不共享 cfront 的 parser。

`cfront2` 不自己做宏展开：输入应已预处理（`clang -E -P` / 编译器 `-E`），
或者本身没有宏的生成 C。实测能零错误解析 sinter 生成的全部 `.c`，以及
`stdio.h` / `stdlib.h` / `string.h` / `pthread.h` / `unistd.h` / `sys/socket.h` /
`sys/stat.h` / `math.h` / `signal.h` / `errno.h` / `limits.h` / `stdint.h` /
`stddef.h` / `time.h` / `fcntl.h` 的预处理结果。

## 泛型容器与字符串特化

标准库里没有 `StrList` / `StrMap` / `StrHashMap` 这种「每种元素类型复制一份」的写法，
也没有引入偏特化语法。容器只写一份泛型定义，编译器在**语义分析之前**把它单态化：
每个使用点出现一组类型实参，就生成一个普通类。类型实参全是内建类型时实例落在
模板模块；含用户类时实例落在**声明该类的模块**，因为只有那个模块知道 C 结构名。

```sinter
import std

fun main(): void {
    val xs = List<str>()          // → class List_str
    xs.add("alpha")
    xs.add("beta")
    println(xs.get(1))

    val m = Map<str, i64>()       // → class Map_str_i64（键有序，二分查找）
    m.put("alpha", 1)
    m.put("beta", 2)
    printlnInt(m.get("beta"))

    val h = HashMap<str, i64>()   // → class HashMap_str_i64（FNV-1a 开放寻址）
    h.put("one", 1)
    printlnInt(h.getOr("one", -1))
}
```

| 使用点 | 单态化出的类 | 生成的 C 结构体 |
| --- | --- | --- |
| `List<str>` | `List_str` | `struct std_generic_list_str` |
| `List<i64>` | `List_i64` | `struct std_generic_list_i64` |
| `Map<str, i64>` | `Map_str_i64` | `struct std_generic_map_str_i64` |
| `HashMap<str, i64>` | `HashMap_str_i64` | `struct std_generic_hash_map_str_i64` |
| `Box<str>` | `Box_str` | `struct std_generic_box_str` |
| `Pair<str, i64>` | `Pair_str_i64` | `struct std_generic_pair_str_i64` |
| `List<Point>` | `List_Point` | `struct <my_module>_list_point`（在 `Point` 所属模块） |

同一份定义的两个实例互不相干：`List_str` 与 `List_i64` 是两条独立的代码路径，
字段类型（`*str` / `*i64`）、元素拷贝方式、释放方式、哈希与比较方式全都不同。

### 元素操作按类型参数分派

容器方法体里写的是裸名字，它们不是真实函数，而是「按 `T` 分派」的占位：

```sinter
pub class List<T> {
    var data: *T
    var cnt: i64
    var cap: i64
    fun push(v: T): void {
        self.reserve(self.cnt + 1)
        self.data[self.cnt] = sinter_gen_clone(v)   // 克隆方式由 T 决定
        self.cnt = self.cnt + 1
    }
}
```

代码生成阶段按实例名尾部的类型实参归约成具体函数名：

| 占位 | `List<str>` 里 | `List<i64>` 里 |
| --- | --- | --- |
| `sinter_gen_clone(v)` | `sinter_gen_clone_str` | `sinter_gen_clone_i64` |
| `sinter_gen_grow(p, n)` | `sinter_gen_grow_str` | `sinter_gen_grow_i64` |
| `sinter_gen_drop(v)` | `sinter_gen_drop_str` | `sinter_gen_drop_i64` |
| `sinter_gen_blank()` | `sinter_gen_blank_str` | `sinter_gen_blank_i64` |
| `sinter_gen_cmp(a, b)` | `sinter_gen_cmp_str` | `sinter_gen_cmp_i64` |
| `sinter_gen_hash(k)` | `sinter_gen_hash_str`（FNV-1a 64） | `sinter_gen_hash_i64` |
| `sinter_gen_text(v)` | `sinter_gen_text_str`（克隆字符串本身） | `sinter_gen_text_i64`（十进制数字） |

`sinter_gen_text` 这一行是最直观的「字符串特化」：`Box<T>` 里只有一份

```sinter
fun describe(): str => sinter_gen_text(self.value)
```

而 `Box<str>("x").describe()` 得到 `x`，`Box<i64>(7).describe()` 得到 `7`。
`Pair<A, B>` 的 `toText()` 还把第二个分量按 `B` 分派（标签写成模板形参名）：

```sinter
fun toText(): str => sinter_gen_join_text(
    sinter_gen_join_text(sinter_gen_text(self.first), sinter_gen_eq_text()),
    sinter_gen_text(self.second, B),     // 末尾 B 只是标签：Pair_str_i64 走 _i64，Pair_i64_str 走 _str
)
```

`Pair<A, B>` 的 `swap(): Pair<B, A>` 则直接产出**另一份单态化产物**：
`Pair_str_i64` 的 `swap()` 返回 `Pair_i64_str` 对象，两份实例可以互相转换。

一个类有多个类型参数时（`Map<K, V>`），用**类型标签**指定这条语句按哪个参数分派：

```sinter
val nk: *K = sinter_gen_grow(self.keys, next)          // 走 K
val nv: *V = sinter_gen_grow(self.vals, next, i64)     // 末尾 i64 只是标签
```

末尾的 `i64` 只参与归约（`Map_str_i64` 的第二个类型实参），既不求值也不进 C 代码，
所以 `Map<str, i64>` 的键数组按 `str` 扩容、值数组按 `i64` 扩容，而 C 侧调用
`sinter_gen_grow_i64()` 时仍然是两个实参。

### 字符串特化的具体差异

- **所有权**：`str` 元素深拷贝（`sinter_string_clone`），`clear` / 析构 / `pop` /
  `remove` 释放；数值类型直接赋值。用户类元素按**借用指针**存储：`clone` / `blank`
  保留指针，`drop` / `free` 是空操作，`cmp` / `hash` 用指针身份（不是 `.equals`）。
  容器析构不会释放用户类对象，生命周期由调用方管理。
- **有序性**：`Map<K, V>` 的键按升序保存，比较走 `sinter_gen_cmp`：`str` 用
  `sinter_string_compare`（字典序），`i64` 用数值比较。
- **哈希**：`HashMap<str, i64>` 的桶位由 FNV-1a 64 决定，`HashMap<i64, i64>` 用
  splitmix 式的整数混淆。
- **取值**：`get` / `at` / `keyAt` / `valueAt` 都返回**克隆**。Sinter 的语句级
  临时字符串会在语句末尾自动释放，返回容器内部指针会被调用方释放掉，
  所以不提供零拷贝引用语义。
- **`IntArray` 与 `List` 的分工**：`IntArray` / `FloatArray` 是定长数组的对象包装
  （`T[N]` + 长度，就地排序/求和），`owned` 决定是否接管释放；`std.generic` 的
  `List<T>` 才是可增长容器，负责分配、扩容与释放。

完整可运行的例子：`projects/collections`（回归脚本里的用例名 `collections`），
它同时用到 `Box<str>`、`Box<i64>`、`Pair<str, i64>`、`Pair<i64, str>`、
`List<str>`、`List<i64>`、`Map<str, i64>`、`HashMap<str, i64>`。

## 已知限制

- 方法重写需在同一模块内声明；跨模块继承未做完整验证。
- 泛型基类与泛型类支持用户自定义类实参；含用户类的实例生成在该类所属模块。
- 主构造函数的形参会被自动登记成**字段**（`class C(val x: i32)` 的既有语义）。
  因为这一点，一个已经显式声明字段的类不要再让构造形参重名 —— 否则会多出
  一套平行字段，构造函数体写的是 A 套、方法读的是 B 套。标准库里的
  `Array<T>` / `ByteArray` 因此把形参写成 `buf` / `count` / `takeOwn`。
- 构造函数名字不能与普通方法名重合；泛型类单态化后构造函数可能跟实例改名
  （`Array_i64`），所以它不会被登记进虚表，也不应该靠名字去调用。
- 语言没有位运算符（`&` / `|` / `^` / `<<`）；需要位运算时写在 `embed C` 里。
  例如 `str.hashSlot(mask)` 的实现就是 C 侧的 `hash & mask`。
- 跨模块对 `List<T>` / `Map<K,V>` / `HashMap<K,V>` / `Box<T>` 的实例方法可正常调用；
  自定义 `Array<T>` 的实例方法仍是实验性覆盖，复杂组合请优先用 `List<T>`。
- 泛型函数的模板签名不会直接进模块头文件；跨模块调用会把实际用到的单态化实例
  （如 `load_i64`）写进定义模块头文件。未被调用的实例不会生成。
- `sinter_gen_*` 这一组分派族函数靠「当前类/函数的类型实参」归约类型后缀，
  在**普通函数体**里裸调（如 `sinter_gen_alloc(n, str)`）归约不出来，会链接失败。
  需要按类型分配时放在泛型类的方法体里，或直接在 `embed C` 里 `malloc`。
- **类字段名只能用小写单词**：字段声明会转 snake_case（`filePath` → `file_path`），
  但 `self.filePath` 这类引用不转，于是 gcc 报 `no member named 'filePath'`。
  标准库里的类字段一律写成 `content` / `label` / `buf` / `size` / `used` / `pos` / `done`
  这种形式。另外 `path` 是保留字，别用 `path` / `pathText` 当字段名。
- **`str` 形参是借用，类里存字符串字段必须克隆**：`self.content = path` 只存指针，
  构造返回后形参的缓冲被回收，字段就悬空（实测 `name()` / `readText()` 拿到垃圾或
  直接段错误）。标准库用 `sinter_file_clone_text` / `sinter_stream_clone_text`（内部
  `sinter_string_from_buf`）做这件事。
- **`static fun f(): 自身类型 { val r = 自身(..); ret r }` 会被插入 `_del(r)`**：
  编译器把「返回局部对象」当成交付+销毁，调用方拿到已释放对象（实测 `atEnd()`
  立即为 true）。所以 `LineReader` 不用静态工厂返回自身，改成主构造 + `openFile` /
  `useFile` / `openText` 三个实例方法设置来源。
- **接口类型的字段会让类自动生成 `toString`**，去调 `<接口>_to_str`，而接口没有
  实现体，那个符号链接期不存在。所以不要写 `var src: InputStream` 这种字段；
  `LineReader` 因此改成持 `FileInputStream` / `MemoryInputStream` 两个具体字段，
  读的时候按谁非空分派。
- **类只保留一个主构造**，第二个重载构造会被忽略（参数对不上直接编译报错），
  多入口要用工厂或实例方法。
- **`str + <C 函数返回的 sinter_string>` 会走整数化路径**（报
  `aggregate value used where an integer was expected`）。改成在 C 侧提供一个
  「追加」函数（如 `sinter_stream_append_chr`）一次拿到结果。
- **对 C 函数返回值取反不可靠**：`!path.startsWith("/")` 实测恒为 true。
  布尔取反写成显式 `if (...) { ret false } ret true`。
- **`var x: str = y` 之后再重新赋值会 double free**：别名共享同一份缓冲，
  作用域结束两边各释放一次（实测段错误）。中间结果别用同类型局部变量接。
- `comptime { val ... }` 常量块只支持常量声明，不支持 `comptime fun` 编译期函数；
  块内初始化式引用运行时变量会报错并指出具体名字（不会静默当成 0）；
- 数组字面量必须配合定长声明 `i64[N]` 使用；无类型标注的 `val xs = [1,2,3]`
  会被推断成标量而非数组，本轮未处理。
- `is` 模式按类型名做静态判断，不做运行时 RTTI。
- 移动构造按「接管 + 置空」实现：逐字段搬运后把源对象字段清零（`str` 字段复位为空串），基类子对象一并清零；用户写 `T(T&&)` 时用其函数体，默认体仍是逐字段搬运 + 清零。
- 内部字符串 `str`（C 侧 `sinter_string`）是值类型，带 `owned` 所有权标记：
  - 局部变量在作用域退出时自动 `sinter_string_free`；
  - 形参是**借用**，不释放（`println(s)` 不会销毁调用方的 `s`）；
  - 返回值做所有权移交，调用方接管；
  - 语句里的临时值（`println("a" + b)`、`foo(bar(x))`、`var n = len(join(a, b))`、
    `return wrap(join(a, b))`、`if (len(join(a, b)) > 0)`）由编译器提前落到
    局部变量，语句执行完立即释放。覆盖的语句形态：`var` 初始化、赋值、
    `return`、解构、表达式语句、`if` 条件、`for` 的上下界；
  - 循环条件（`while` / `do-while` / `for`）里的临时值在**每次迭代内**求值与释放：
    `while` / `for` 改写成 `for (;;) { 临时值; 条件; 释放; if (!条件) break; 循环体 }`，
    `do-while` 的条件挪到循环体末尾（`continue` 改成 `goto` 跳到条件代码之前），
    因此不会退化成「条件只求值一次」，`continue` 也不会跳过条件求值；
  - 三元分支、`&&` / `||` 右侧、`when` 分支和数组字面量元素**不做**提升：
    前者是条件求值（提前落地会把没选中的分支也执行一遍），后者是 `memcpy`
    进去的指针副本（提前释放会给数组留下悬空指针）；
  - 拼接按「借用 / 临时」分流（`sinter_string_concat` / `_take_left` /
    `_take_right` / `_concat_own`），避免 double free；
  - 字符串**字面量**是 `owned=0` 的静态视图，零拷贝且释放为无操作。
- 内建数组 `T[N]` 的 `calloc` 缓冲随作用域退出自动 `free`；元素类型带析构时
  按**逆序**逐个析构（`if (arr[i]) { T_del(arr[i]); arr[i] = 0; }`）再 `free`。
  置空是必需的：元素可能已经被 `del` 过，正序析构又会让后面的元素引用到已释放的对象。
- 只有声明了析构器（`~T()`）的类才参与自动释放。没有析构器的类
  常常被存入静态字段或被他处别名引用，提前 `free` 会造成 use-after-free，
  因此保持保守策略（这类对象在进程结束时由系统回收）。
- 数组（`T[N]`）只做批量**析构**（作用域退出逆序 `T_del` + `free`），不做批量**构造**：
  元素由 `calloc` 零初始化，需要对象时逐个赋值 `arr[i] = Res(...)`。
  初始化式借用了另一个数组变量（而不是本语句自己 `calloc` / 字面量）时不登记析构，
  否则同一块缓冲会被释放两次。
- `struct`/`class` 括号内参数（`class Res(val id: i64)`）由生成器直接赋字段；
  若同时存在函数体构造函数 `fun Res(...)`，`<T>_new` 会在字段赋值后调用它，
  因此构造函数体里的语句会真正执行（此前会被静默丢弃）。
- 泛型容器的元素操作目前只覆盖 `i64` / `u8` / `f64` / `bool` / `str` 的
  clone / free / drop / grow / blank 分派，`cmp` / `hash` 只提供
  `i64`、`u8`、`f64`、`str`：`List<自定义类>`、`HashMap<f64, V>` 这类组合
  还需要在 `packages/std/src/std/generic.sinter` 的 `embed C` 里补一个
  `sinter_gen_<动作>_<类型>` 薄封装，否则归约出的函数名没有定义（C 编译阶段报错，
  不会静默跑错）。
- 泛型类的构造形参**就是**字段（Sinter 的既有语义）：`struct Box<T>(v: T)` 会生成
  字段 `v`。若类体里另外写 `var value: T`，会得到两个字段而只有 `v` 被赋值，
  `value` 恒为零值。正确写法是让构造形参与字段同名（`Box(value: T)`），
  `packages/std` 的 `Box` / `Pair` 即如此。
- 旧版 `stdlib`（`sinter_String*` 指针版）已移除；字符串、数组算法与容器统一由
  `packages/std` 的新模块提供。新 API 的入门用例见 `projects/hello`。
- `std.list` / `std.map` / `std.hashmap` 三个模块以及 `StrList` / `StrMap` /
  `StrHashMap` 三个「字符串专用副本」已删除，容器只剩 `std.generic` 里的
  `List<T>` / `Map<K, V>` / `HashMap<K, V>` 泛型版本。
- 表达式体返回 `void`（`fun dump(x: i64): void => println(x)`）、泛型参数表尾随逗号
  （`fun add<T,>(a: T, b: T)`）、含转义字面量的长度，这三条**已在 1.0 前修掉**：
  1. `void` 函数里的 `=> expr` 不再生成 `void __ret = expr;`，而是按
     「表达式语句 + 补析构 + `return;`」发（ISO C 也不允许 `void` 函数带值 `return`）；
  2. 泛型模板不再把自己的原型导进头文件 —— `(T a, T b)` 这种未代入的类型参数
     不会再出现在生成的 C 里（历史症状是 `unknown type name 'T'`）；
  3. 字符串字面量的长度按**解码后**的字节数算：`"a\"b"` 是 3 字节（`a`、`"`、`b`），
     不是 4 个源码字符；非可打印字节按三位八进制重新转义，`\x41` 这类不会再溢出。
  回归用例 `scripts/cases/langfix`，断言 `==0|void __ret`、`==0|(T a, T b)`、
  `>=1|"a\"b", 3)`、`>=1|"back\\slash", 10)`。
- 泛型类/函数、跨模块泛型函数实例化、用户类容器都已支持；`List<Point>`、
  `Map<str, Point>`、`HashMap<i64, Point>`、`Box<Point>` 有回归用例
  `scripts/cases/genericclass`、`scripts/cases/crossgeneric`。
### 对象层 / 命名相关的已知缺陷

- 函数重载按参数类型签名解析；仅返回类型不同时需要 `[]` 别名区分。
  `printlnInt` / `printlnFloat` 等是早期兼容入口，仍保留。
- **类的 `static fun` 与 `operator+` 不会导出到头文件**：跨模块调用时会退化成
  「未声明的自由函数」，在 C 编译阶段报 `implicit declaration of function`。
  因此 `String` 不提供 `String.fromInt(...)`（改用模块级 `fromInt`），`Int` 的算术
  用 `plus` / `minus` / `times` / `div` / `rem`（与 Kotlin 的 `+` 实际调用的方法同名）。
- **方法名不能叫 `typeName`**：会和生成代码里的 `<类>_type_name`（RTTI 类型名常量）
  撞符号，`Any` 里改用 `typeLabel`。
- **泛型类继承 `Any` 需要 vtable 原型**（本轮已修）：`List<T> : Any` 会让
  `List_str_vtable` 直接引用基类实现 `std_core_any_to_string`，而类的成员函数
  **不导出到头文件**（跨模块方法调用一律走 `__vptr`），于是 C 阶段报
  `'std_core_any_to_string' undeclared`。修法在
  `Codegen::emit_vtables_impl`：槽位回落到「别的模块」的基类实现时，先在
  vtable 前补一条模块私有原型。现在 `Int` / `Float` / `Bool` / `String` /
  `IntArray` / `FloatArray` / `Box` / `Pair` / `List` / `Map` / `HashMap`
  **全部**继承 `Any`。
- **`str` 局部变量累加会双重释放**：`sinter_gen_join_text` 走
  `sinter_string_concat_own`（两边都 free），而 `out = ...` 走
  `sinter_string_assign`（先 free 目标）。于是
  `out = sinter_gen_join_text(out, x)` 会 free 同一个缓冲区两次，运行时直接
  SIGSEGV。`std.generic` 里所有文本拼接因此只有两种写法：嵌套表达式（无局部
  变量，如 `Pair.toText()`），或把累加放进 C（`sinter_gen_sb_*` 这套 builder，
  `List` / `Map` / `HashMap` 的 `toString()` 用它）。
- **类型标签写错不会报错，只会静默选错函数**：`Map` 里值数组的
  `sinter_gen_grow(self.vals, next, i64)` / `sinter_gen_clone(..., i64)` /
  `sinter_gen_blank(i64)` 曾把标签写死成 `i64`，`Map<str, i64>` 恰好对，
  `Map<i64, str>` 就编译失败（`sinter_gen_blank_i64` 初始化 `sinter_string`）。
  现在值侧统一写 `V`。改泛型代码时，标签要跟着形参走，不要跟着「当前测试里的
  那一种实例」走。
- **形参名 `from` 是保留字**：`fun replace(from: str, to: str)` 解析失败，
  改成 `target` / `replacement`。
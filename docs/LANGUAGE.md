# Sinter 语言指南

> 中文在前，英文在后。本文描述当前 C 后端真实支持的语法；示例均来自仓库内的
> [`../examples/`](../examples/) 与 [`../tests/cases/`](../tests/cases/)。

## 中文

### 设计取向

Sinter 是一门现代静态类型语言，语法面向工程实践：声明简洁、控制流直观、抽象能力完整，
同时保持产物是普通 C。语言特性在编译期完成 lowering，不引入专有虚拟机或垃圾回收器。
当前后端强调**可编译、可验证、可互操作**，因此部分语法仍处于解析或规划阶段，见文末限制。

### 模块与导入

```sinter
module demo

import std
import std.string
import std.list
```

- `module <name>` 声明模块名；没有显式 `module` 的顶层文件归入合成模块 `program`。
- `import std` 导入 prelude（基础类型、常用函数与 `println`）。
- 非 prelude 模块必须显式导入，例如 `std.string`、`std.list`、`std.hashmap`、`algo.aes`。
- 包路径由 `sinter.toml` 的 `path:` 依赖提供；仓库内统一使用相对路径。

### 变量与类型

```sinter
val answer = 42          // 不可变绑定
var counter = 0          // 可变绑定
counter = counter + 1

val a: i8 = 127
val b: i16 = -1000
val c: i32 = 2000000
val d: i64 = 9000000000
val e: u8 = 255
val pi: f64 = 3.14159
val half: f32 = 0.5
val flag: bool = true
val ch: char = 'A'
val s: str = "hello"
```

标量类型包括 `i8`、`i16`、`i32`、`i64`、`u8`、`u16`、`u32`、`u64`、`f32`、`f64`、
`bool`、`char`、`str`、`ptr` 与 `cstr`。复合类型包括定长数组 `T[N]`、指针 `*T`、
引用 `T&`、可空 `T?`、函数类型 `(T...) -> R`。

```sinter
var nums: i64[3]
nums[0] = 7

val p: *i64 = &nums[0]
println(*p)

val raw: ptr = p
val back: *i64 = raw as *i64
```

### 字面量与表达式

- 十进制整数，以及 `0x1234`、`0b1010`、`0o17`。
- 浮点数，如 `3.14`、`0.5`。
- 字符 `'A'`，字符串 `"..."`，支持转义与插值：`"${expr}"`。
- 算术 `+ - * / %`，比较 `== != < <= > >=`，逻辑 `&& || !`，位运算与复合赋值。
- 三元表达式：`cond ? a : b`。
- 闭区间：`a..b`；要表达“小于 n”需写成 `a..(n - 1)`。

```sinter
val parity = answer % 2 == 0 ? "even" : "odd"
println("${name} v1")
println(0x1234)
println(0b1010)
println(0o17)
```

### 控制流

```sinter
if (n % 2 == 0) {
    println("even")
} else if (n > 5) {
    println("odd and big")
} else {
    println("odd and small")
}

while (i < 5) {
    total = total + i
    i = i + 1
}

for (x in 0..5) { sum = sum + x }   // 闭区间

when (v) {
    0 => { println("zero") }
    1 => { println("one") }
    else => { println("many") }
}
```

`if` 与 `when` 当前是语句，不是可赋值的表达式。需要按分支产生值时，先声明 `var`，
再在分支中赋值。

### 函数

```sinter
fun add(a: i64, b: i64): i64 {
    ret a + b
}

fun square(x: i64): i64 => x * x

fun intro(name: str, age: i64 = 18, city: str = "unknown"): str {
    ret "${name}/${age}/${city}"
}

fun sumAll(xs: i64...): i64 {
    var total = 0
    var i = 0
    while (i < xs__n) { total = total + xs[i]; i = i + 1 }
    ret total
}
```

函数特性：

- `=>` 表达式体；`ret` 或 `return` 返回；`void` 返回类型可省略。
- 默认参数与命名参数，例如 `intro("ada", city = "nyc")`。
- 可变参数 `xs: i64...`；元素个数变量名为 `xs__n`。
- 重载函数需要各自链接名：`fun twice[twice_i64](...)`。
- 扩展函数使用 `ext fun T.name[...]`，函数体内用 `self`。
- lambda 作为实参直接传入，函数类型写作 `(i64) -> i64`。

```sinter
ext fun i64.double[double_i64](): i64 => self * 2
fun apply(f: (i64) -> i64, x: i64): i64 => f(x)

println(5.double())
println(apply(fun (n: i64): i64 => n + 1, 41))
```

### 类型声明与泛型

```sinter
data class Point(val x: i64, val y: i64)

class Counter {
    var value: i64
    Counter(start: i64 = 0) { self.value = start }
    fun inc(): void { self.value = self.value + 1 }
}

struct Pair(val first: i64, val second: i64) {
    fun sum(): i64 => self.first + self.second
}

enum Color { Red, Green, Blue }
interface Shape { fun area(): f64 }
```

- `data class` 自动生成 `toString`、`equals`、`copy`、`copyWith` 与 `componentN`。
- `class` 支持构造、析构、`self`/`this`、继承与虚表。
- `struct` 的括号参数自动成为字段；当前后端按类处理。
- `enum` 配合 `when` 使用；`pub` / `priv` 块设置默认可见性。
- 泛型函数与泛型类在语义分析前单态化，零装箱、无运行时类型擦除。

```sinter
fun id<T>(v: T): T => v
class Box<T>(val value: T) {
    fun get(): T => self.value
}

println(id<i64>(7))
val bi = Box<i64>(7)
```

### 异常与资源管理

```sinter
try {
    throw RuntimeException("boom")
} catch (e: RuntimeException) {
    println(e.message)
} finally {
    println("cleanup")
}

class Resource(val name: str) {
    ~Resource() { println("dtor " + self.name) }
}

var early = Resource("early")
del early
```

- `try` / `catch` / `finally` / `throw` 由后端展开为 C 控制流。
- `~Name()` 析构函数在作用域结束时自动运行；`del x` 提前释放。
- `@no_raii` 关闭自动析构；目前有基础重复删除与局部指针逃逸检查。

### C 互操作

```sinter
embed C header {
    #include <math.h>
}

embed C {
    static double c_sqrt(double x) { return sqrt(x); }
}

bind C fun c_sqrt(x: f64): f64
```

`embed C` 与 `embed C header` 分别把正文写入生成的 `.c` 与 `.h`；`bind C fun` 声明外部函数。

### 标准库与包

`std` 是 prelude；其他标准库模块需显式导入。常用模块：

`std.core`、`std.string`、`std.math`、`std.text`、`std.arrays`、`std.box`、`std.pair`、
`std.list`、`std.map`、`std.hashmap`、`std.option`、`std.result`、`std.io`、`std.file`、
`std.filesystem`、`std.path`、`std.stream`、`std.env`、`std.memory`、`std.sys`、
`std.thread`、`std.async`、`std.time`、`std.binary`、`std.random`、`std.algo`、`std.set`。

独立包位于 [`../packages/`](../packages/)：`algo`、`json`、`net`、`toml`、`zip`。
它们通过相对 `path:` 依赖接入工程，详见
[`SELFHOST_PACKAGES.md`](SELFHOST_PACKAGES.md)。

### 当前边界

以下特性尚未由当前 C 后端实现，或只是解析但未参与检查：

- `..<` 半开区间、`?:` Elvis、`|>` 管道、`is` / `in` 表达式、`++` / `--`。
- `?.` 短路、完整闭包捕获、`i128` / `u128`、`bind C struct`、运算符重载。
- 完整空安全、所有权借用检查、`when` 穷尽性检查。
- `when` / `if` 不是表达式；`struct` 按类处理；泛型约束 `must` 会被解析但暂不检查。
- `0xFF` 当前会生成无效的 `0x,`，请使用 `0x1234` 等完整形式。
- 不存在 `std.collections`；容器在 `std.box`、`std.list`、`std.map`、`std.hashmap`、`std.arrays`。

---

## English

### Design direction

Sinter is a modern statically typed language aimed at practical engineering: concise declarations,
straightforward control flow and complete abstraction features, while keeping ordinary C as the
artifact. Features are lowered at compile time; there is no proprietary VM or garbage collector.
The current backend prioritizes compilability, verifiability and interoperability, so some syntax
is still parsed-only or planned; see the limitations below.

### Modules and imports

```sinter
module demo

import std
import std.string
import std.list
```

- `module <name>` declares the module; a top-level file without one goes into the synthetic `program` module.
- `import std` brings in the prelude: basic types, common functions and `println`.
- Non-prelude modules must be imported explicitly, such as `std.string`, `std.list` or `algo.aes`.
- Package locations come from relative `path:` dependencies in `sinter.toml`.

### Variables and types

```sinter
val answer = 42          // immutable binding
var counter = 0          // mutable binding
counter = counter + 1

val d: i64 = 9000000000
val pi: f64 = 3.14159
val flag: bool = true
val ch: char = 'A'
val s: str = "hello"
```

Scalars: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64`, `bool`, `char`,
`str`, `ptr`, `cstr`. Composite forms include fixed arrays `T[N]`, pointers `*T`, references `T&`,
nullable `T?` and function types `(T...) -> R`.

### Literals and expressions

- Decimal integers plus `0x1234`, `0b1010`, `0o17`.
- Floating-point literals such as `3.14`, `0.5`.
- Characters `'A'`, strings `"..."`, escapes and interpolation `"${expr}"`.
- Arithmetic, comparison, logical and bitwise operators, plus compound assignment.
- Ternary `cond ? a : b`.
- Inclusive ranges `a..b`; express “less than n” as `a..(n - 1)`.

### Control flow

```sinter
if (n % 2 == 0) {
    println("even")
} else {
    println("odd")
}

for (x in 0..5) { sum = sum + x }

when (v) {
    0 => { println("zero") }
    else => { println("other") }
}
```

`if` and `when` are statements, not assignable expressions. Declare a `var` first and assign inside
branches when a value is needed.

### Functions

```sinter
fun add(a: i64, b: i64): i64 { ret a + b }
fun square(x: i64): i64 => x * x

fun intro(name: str, age: i64 = 18, city: str = "unknown"): str {
    ret "${name}/${age}/${city}"
}

fun sumAll(xs: i64...): i64 {
    var total = 0
    var i = 0
    while (i < xs__n) { total = total + xs[i]; i = i + 1 }
    ret total
}
```

Functions support expression bodies, `ret`/`return`, optional `void`, default and named arguments,
variadics with a `<name>__n` count, overload link names via `fun f[link_name]`, extension functions
with `self`, lambdas passed directly as arguments and function-typed parameters.

```sinter
ext fun i64.double[double_i64](): i64 => self * 2
fun apply(f: (i64) -> i64, x: i64): i64 => f(x)

println(5.double())
println(apply(fun (n: i64): i64 => n + 1, 41))
```

### Type declarations and generics

```sinter
data class Point(val x: i64, val y: i64)

class Counter {
    var value: i64
    Counter(start: i64 = 0) { self.value = start }
    fun inc(): void { self.value = self.value + 1 }
}

struct Pair(val first: i64, val second: i64)
enum Color { Red, Green, Blue }
interface Shape { fun area(): f64 }
```

`data class` generates `toString`, `equals`, `copy`, `copyWith` and `componentN`. `class` supports
constructors, destructors, `self`/`this`, inheritance and vtables. `struct` fields come from the
parenthesized parameters and are currently treated as a class. `enum` works with `when`;
`pub`/`priv` blocks set default visibility. Generic functions and classes are monomorphized before
semantic analysis: no boxing and no runtime type erasure.

### Errors and resources

```sinter
try {
    throw RuntimeException("boom")
} catch (e: RuntimeException) {
    println(e.message)
} finally {
    println("cleanup")
}

class Resource(val name: str) {
    ~Resource() { println("dtor " + self.name) }
}
```

`try`/`catch`/`finally`/`throw` are lowered to C control flow. `~Name()` runs at scope exit;
`del x` releases early. `@no_raii` disables automatic destruction. Basic double-delete and
local-pointer-escape checks are available.

### C interop

```sinter
embed C header {
    #include <math.h>
}

embed C {
    static double c_sqrt(double x) { return sqrt(x); }
}

bind C fun c_sqrt(x: f64): f64
```

`embed C` and `embed C header` append their bodies to generated `.c` and `.h` files;
`bind C fun` declares an external function.

### Standard library and packages

`std` is the prelude; other standard modules need explicit imports: `std.core`, `std.string`,
`std.math`, `std.text`, `std.arrays`, `std.box`, `std.pair`, `std.list`, `std.map`,
`std.hashmap`, `std.option`, `std.result`, `std.io`, `std.file`, `std.filesystem`, `std.path`,
`std.stream`, `std.env`, `std.memory`, `std.sys`, `std.thread`, `std.async`, `std.time`,
`std.binary`, `std.random`, `std.algo`, `std.set`.

Standalone packages live in [`../packages/`](../packages/): `algo`, `json`, `net`, `toml`, `zip`.
They are wired in through relative `path:` dependencies; see
[`SELFHOST_PACKAGES.md`](SELFHOST_PACKAGES.md).

### Current boundaries

Not implemented by the current C backend, or parsed but not checked:

- `..<` half-open ranges, Elvis `?:`, pipe `|>`, `is`/`in` expressions, `++`/`--`.
- `?.` short-circuiting, full closure capture, `i128`/`u128`, `bind C struct`, operator overloading.
- Full null safety, ownership borrow checking and `when` exhaustiveness.
- `when`/`if` are not expressions; `struct` is treated as a class; generic `must` constraints are
  parsed but not enforced.
- `0xFF` currently emits invalid `0x,`; use complete forms such as `0x1234`.
- There is no `std.collections`; containers live in `std.box`, `std.list`, `std.map`,
  `std.hashmap` and `std.arrays`.

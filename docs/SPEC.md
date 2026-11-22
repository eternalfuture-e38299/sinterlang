# Sinter Language Specification

Status: **1.0 draft**. This document is normative for the source language and its
observable runtime semantics. The standard library is specified separately and is
versioned independently.

Conventions: `MUST` / `MUST NOT` are requirements on implementations; examples are
non-normative unless marked otherwise.

---

## 1. Scope and model

Sinter is a statically typed, compiled language. A compiler translates a module
graph into ISO C99 (C11 or newer may be selected). The C output is an implementation
detail, but the following are part of this specification:

- value and object lifetime rules;
- nullability and pointer semantics;
- exception propagation;
- module visibility;
- the C ABI used by `extern fun` / `bind C`.

Memory is not garbage-collected. Objects with a destructor are destroyed by scope
exit (RAII). Raw pointers are unsafe; the compiler rejects only statically provable
local-lifetime errors (see §8).

---

## 2. Lexical structure

- Source is UTF-8. Comments: `// ...` and `/* ... */` (non-nesting).
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- Keywords:
  `module import fun class struct interface enum data annotation const val var
   return ret if else while for do break continue true false null nil in when is
   override must embed bind from alias comptime pub priv inline static abstract
   final extends implements super this self del ext default operator try catch
   finally throw`
- Integer literals: decimal, `0x`, `0o`, `0b`; optional type suffix (`5i32`, `5u8`).
  No suffix means `i64`.
- Float literals: digits with `.` and/or exponent, optional `f32`/`f64` suffix.
  No suffix means `f64`.
- Character literals: `'a'`, with the same escapes as strings.
- String literals:
  - `"..."` supports `${expr}` interpolation and escapes (`\n`, `\t`, `\"`, `\\`,
    `\xNN`, `\u{...}`).
  - `""" ... """` is a multi-line string: newlines are preserved, `"` needs no
    escaping, and `${expr}` still interpolates. `\r` is discarded.
- Separators: newlines and `;`. Inside parentheses/brackets newlines are ignored.

---

## 3. Modules and visibility

A file begins with `module <name>`. `import <module>` makes declarations of that
module visible; `import std` enables the prelude.

Names are module-scoped. A declaration is exported to other modules only when it is
marked `pub`, or appears in a `pub { ... }` block:

```sinter
pub fun f(): void
pub class C { pub fun m(): void }
pub { fun g(): void; class D { ... } }
```

Class members default to **module-private** (visible inside their module, not
exported). `pub fun` exports a method; `priv fun` / `priv { ... }` states the default
explicitly. Calling a non-exported method from another module is an error (`E0202`).
Fields declared with `val`/`var` in the primary constructor are public data fields.

`annotation class` declares compile-time metadata; its arguments are checked at use
sites (count, required vs. defaulted, literal type). Annotation classes do not
generate runtime values.

The standard lifecycle annotations are:

| Annotation | Meaning |
|---|---|
| `@since("1.2")` | first version in which the API exists |
| `@frozen("1.0")` | API contract is frozen; no further breaking changes |
| `@experimental` | call sites emit `W0304`; API may change |
| `@deprecated("use X", "1.2", "2.0")` | call sites emit `W0302`; message, deprecation version, planned removal |
| `@must_use` | ignoring the return value emits `W0303` |

---

## 4. Types

### 4.1 Primitive types

`bool`, `i8`/`i16`/`i32`/`i64`/`i128`, `u8`/`u16`/`u32`/`u64`/`u128`,
`f32`/`f64`, `str` (immutable length-carrying string value), `cstr`
(`const char*`), `ptr` (`void*`), `void`.

`str` values are copied on assignment and passed by value; they own their buffer
when `owned != 0`. `cstr` and `ptr` are borrowed raw pointers.

### 4.2 Nullability

`T?` is an optional value and lowers to a nullable pointer for reference types. The
safe access form is `x?.m`; `x ?: y` supplies a fallback; `x!!` asserts non-null and
aborts (with backtrace) when null. Dereferencing a null `T?` without one of these is
undefined behavior.

### 4.2a References

`T&` is a non-null, non-rebindable reference:

```sinter
fun inc(x: i64&): void { x = x + 1 }
var n: i64 = 1
inc(&n)                 // n == 2
val r: i64& = &n
r = 10                  // writes through to n
```

- A reference is created from an lvalue with `&expr` and cannot be `null`.
- Reading or assigning a reference auto-dereferences: `r` behaves like the
  referenced value, `*r` is accepted as an explicit form.
- References to classes (`Box&`) alias the object (class values are already
  pointers); `b.field` and `b.method()` work directly.
- Type checking rejects uninitialized references, `null` initializers/assignments,
  and reference arithmetic is not part of the language.
- References do not add borrow tracking: aliasing rules from §8 still apply.

Casts use `expr as T` (for example `p as *i64`, `p as i64`, `p as ptr`). Pointer
values support indexing `p[i]`, `+`/`-` with an integer offset, comparison with
`null`, and value equality.

### 4.3 Classes, structs, interfaces

```sinter
class Point(val x: i64, val y: i64)
struct Pair<A, B>(val first: A, val second: B)
interface Shape { fun area(): f64 }
class Circle(r: f64) : Shape {
    pub fun area(): f64 => 3.14159 * r * r
}
```

- Classes are reference types; the value representation is a pointer to a heap
  object.
- `data class` / `data struct` additionally generate `equals`, `hashCode`,
  `toString`, `copy` (defaults are the current field values), `copyWith`, and
  `componentN`.
- Inheritance is single for classes and multiple for interfaces. Virtual dispatch
  uses a vtable. `override` is required to override.
- A class may declare `~T()`; it runs on scope exit, `return`, `break`, `continue`,
  and `del`.

### 4.4 Functions, lambdas, generics

- Functions are first-class values: `(i64) -> i64`.
- Receiver function types use `T.() -> R`; the receiver is passed as the first
  parameter. `b.block()` compiles to `block(b)`.
- Trailing-lambda sugar: `build { ... }` appends a block lambda to the call.
- Generic parameters: `<T>`, with optional constraints `<T must number>` and type
  packs `<Args...>`.
- Generic functions may declare aliases with their own parameter list:

  ```sinter
  fun arrayOf<T>[anyArrayOf<T>](e: T...): T => e[0]
  ```

  Calls may use either name, with explicit or inferred type arguments.

### 4.5 Type parameters in containers

Generic containers accept built-in types, `str`, and user-defined classes. An
instance whose type arguments contain a user class is materialized in the module
that declares that class; built-in-only instances stay in the template module.
Generic functions are monomorphized on demand across module boundaries: the
defining module emits the concrete prototype and definition requested by any
other module.

Container ownership for class elements is **borrowed**: clone/blank preserve the
pointer and drop/free are no-ops. `cmp`/`hash` use pointer identity. The caller
owns the objects.

---

## 5. Expressions and statements

Operators, in increasing precedence: `? :`, `??`, `||`, `&&`, comparisons, `|`,
`^`, `&`, shifts, `+ -`, `* / %`, unary `! - ~ & *`, postfix calls/member/index,
`!!`, `?.`, `?:`, `|>`.

Statements: declarations, assignment, expression statements, `if`/`else`,
`while`, `do`/`while`, `for (x in it)`, `break`, `continue`, `return`,
`del x`, `no_raii { ... }`, `comptime { ... }`, `try`/`catch`/`finally`,
`throw`.

`when` is an expression with `is`, `in`, literal and enum patterns; enum subjects
must be exhaustively covered unless an `else` exists.

---

## 6. Compile-time evaluation

`comptime if`, `comptime { val ... }`, `comptime assert`, `Target.*`, and `Site.*`
are evaluated before semantic analysis/code generation. Values must be statically
evaluable; otherwise a diagnostic is emitted. Compile-time code cannot reference
runtime variables and cannot allocate runtime objects.

---

## 7. Exceptions

```sinter
try {
    throw "message"
} catch (e: str) {
    println(e)
} finally {
    println("cleanup")
}
```

- `throw expr` requires `expr: str`.
- `try` propagates to the nearest enclosing `catch` in the dynamic call stack.
- `finally` runs after the `try` body or after the `catch` body on the normal path.
- **RAII and unwinding:** `throw` runs the destructors of every owning local
  (string, class with `~T`, built-in array) that was created after the catch
  frame was entered, then jumps.
- `finally` also runs for `ret`, `break`, and `continue` that leave the `try`
  or `catch` block, and when a `throw` escapes a `catch` body into an outer
  handler.
- An uncaught exception prints `uncaught exception: <message> (at <module>:<line>)`,
  prints a backtrace, and aborts.
- `panic(msg)` / `assert(cond, msg)` print `panic: <msg>`, print a backtrace, and
  abort. They are fail-fast and are not catchable.

**Lifetime rule (normative):** `throw` transfers control with `longjmp`. Destructors
of objects whose scope is exited by the jump do not run. Therefore owning resources
must not rely on RAII inside a `try`/`catch` region; use `Result<T>` for recoverable
resource-handling paths, or close resources in `finally`. `finally` does not run when
the `try` body returns early (`ret`/`break`/`continue`).

---

## 8. Ownership and lifetimes

Sinter has no borrow checker in the Rust sense. Values are moved/copied by the
compiler's C lowering; class values are pointers and may alias.

The compiler performs a conservative **local-lifetime check** and rejects:

| Code | Meaning | Diagnostic |
|---|---|---|
| `return &local` / `return p` where `p = &local` | returning a stack address | `E0901` |
| use of a variable after `del x` | use-after-delete | `E0902` |
| `del x` twice | double delete | `E0903` |
| `del x` while `&x` was taken | delete with a live borrow | `E0904` |
| storing `&local` into a field/global | escaping local address | `E0905` |

Everything else involving `ptr`, `*T`, integer/pointer casts, C calls, or aliases
across modules is **unsafe** and unchecked. Programs must not:
- return pointers into stack frames;
- use pointers after the pointed-to object is destroyed;
- alias a mutable object across threads without synchronization.

The checker is intentionally conservative and may reject a safe program; there is
currently no `unsafe` escape hatch.

---

## 9. Standard library packaging and versioning

The standard library is **not part of the compiler release**. It is a set of
independent packages with their own semantic versions:

- `std` — core library (`std.core`, `std.string`, `std.list`, `std.map`,
  `std.file`, `std.thread`, `std.result`, …).
- `net` — networking (`std.net`), depending on `std`.

Rules:

1. `import std` enables only the prelude. Other modules are explicit imports.
2. `import std.net` requires the independent `net` package in the project's
   dependencies.
3. The compiler seed-installs bundled packages only when the package version is not
   already present. It never overwrites an installed package of the same version;
   set `SINTER_REFRESH_BUNDLED=1` to force a refresh.
4. A std/net update requires bumping that package's version; the compiler version
   does not change.
5. The language runtime (`sinter_runtime.h/.c`) is generated by the compiler and is
   not a library package.

---

## 10. C ABI and platform

- `extern fun` uses the C symbol name and C ABI; parameter/return types must be
  C-compatible (`cstr`, fixed-width integers, pointers, structs by pointer).
- `bind C header "x.h"` imports C declarations; `bind C struct FILE` declares an
  opaque C type.
- `str` is not C ABI compatible; convert with `toCStr` / `fromCStr`.
- Supported targets: Linux, macOS, Android, and Windows (MinGW-w64).
  Windows uses Win32 threads and Winsock2; POSIX targets use pthreads and BSD
  sockets. `std.net`/`std.thread` keep the same source-level API.

---

## 11. Diagnostics

Errors are rendered as `file:line:col: error[CODE]: message` with a hint and source
snippet. Codes are grouped by area: `E01xx` lexical/syntax, `E02xx` declarations and
visibility, `E03xx` types, `E04xx` generics, `E05xx` comptime, `E06xx` codegen,
`E07xx` `extern C`, `E08xx` C interop, `E09xx` ownership/lifetimes.

// sinter backend: codegen.cpp
#include "backend/codegen.hpp"
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <ranges>

namespace sinter {
    void Codegen::emit_program_body(Program &p) {
        // 块式 module { ... } 的成员等价于当前模块顶层声明，
        // 因此在此处一次性展平，后续所有遍历都按普通顶层语句处理。
        flatten_module_blocks(p);
        // 泛型实例：优先使用 driver 汇总的跨模块请求；单独调用时退回本模块扫描。
        GenericFunctionInstantiations local_insts;
        if (!external_generic_insts_) collect_generic_instantiations(p, local_insts);
        const GenericFunctionInstantiations &generic_insts =
            external_generic_insts_ ? *external_generic_insts_ : local_insts;

        // 先输出类型
        for (auto &s: p.statements) {
            if (auto *e = dynamic_cast<EnumDecl *>(s.get())) emit_enum(e);
            else if (auto *c = dynamic_cast<ClassDecl *>(s.get())) emit_class_decl(c);
            // 接口：生成只有对象头的 `struct`（`__vptr` / `__parent` / `__type_chain`）。
            // 没有这一步，`fun f(s: Shape)` 会拼出不存在的 `struct ex_shape`。
            // 接口方法是抽象的、没有实现，所以只发类型不发原型。
            else if (auto *it = dynamic_cast<InterfaceDecl *>(s.get())) {
                ClassDecl synthetic;
                synthetic.name = it->name;
                synthetic.is_struct = true;
                synthetic.is_abstract = true;
                synthetic.is_pub = it->is_pub;
                emit_class_decl(&synthetic);
            }
            else if (auto *a = dynamic_cast<AliasStmt *>(s.get())) emit_alias(a);
        }
        // 静态字段（声明进 .h，定义进 .c）
        for (auto &s: p.statements) {
            if (auto *c = dynamic_cast<ClassDecl *>(s.get())) emit_static_fields(c);
        }
        // 模块级 val/var（原先被静默忽略，引用时会生成未声明符号）
        for (auto &s: p.statements) {
            if (auto *v = dynamic_cast<VarDeclStmt *>(s.get())) {
                if (!v->is_static) emit_module_global(v);
            }
        }
        // 再输出 vtable（vtable 引用方法符号，必须先有方法原型）
        for (auto &s: p.statements) {
            if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                for (auto &m: c->members)
                    if (auto *mf = dynamic_cast<FunctionDecl *>(m.get()))
                        emit_function_decl(mf, true, c->name);
                if (c->destructor)
                    if (auto *df = dynamic_cast<FunctionDecl *>(c->destructor.get()))
                        emit_function_decl(df, true, c->name);
                emit_vtables(c);
            }
        }
        // 函数前置声明
        for (auto &s: p.statements) {
            if (auto *f = dynamic_cast<FunctionDecl *>(s.get()))
                emit_function_decl(f, false, "");
            else if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                for (auto &m: c->members)
                    if (auto *mf = dynamic_cast<FunctionDecl *>(m.get()))
                        emit_function_decl(mf, true, c->name);
                if (c->destructor)
                    if (auto *df = dynamic_cast<FunctionDecl *>(c->destructor.get()))
                        emit_function_decl(df, true, c->name);
            }
        }

        // 泛型单态化原型（必须在任何函数定义之前）
        for (auto &[decl, insts]: generic_insts) {
            const FunctionInfo *gfi0 = fi_of_decl(decl);
            if (!gfi0) continue;
            const bool gm0 = gfi0->is_method;
            const std::string go0 = gm0 ? gfi0->owner_class : std::string();
            for (auto &subst: insts) emit_function_proto_generic(decl, subst, gm0, go0);
        }

        // 泛型实例的成员函数原生声明（含 this）：调用点按 c_name 生成，
        // 若这里不声明，C 会报隐式声明。它们同时也是下文定义的对照。
        for (auto &s: p.statements) {
            if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                for (auto &m: c->members)
                    if (auto *mf = dynamic_cast<FunctionDecl *>(m.get()))
                        if (!mf->tparams.empty()) emit_function_decl(mf, true, c->name);
            }
        }

        // 泛型实例成员的定义侧：emit_function_def_generic 生成的名字必须与上面对齐。

        // 类构造/析构
        for (auto &s: p.statements)
            if (auto *c = dynamic_cast<ClassDecl *>(s.get())) emit_class_ctor_def(c);

        // 普通函数
        for (auto &s: p.statements) {
            if (auto *f = dynamic_cast<FunctionDecl *>(s.get())) {
                if (!f->tparams.empty()) continue;
                emit_function_def(f, false, "");
            } else if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                current_class_ = c->name;
                current_class_c_ = class_c_name(c->name);
                for (auto &m: c->members) {
                    if (auto *mf = dynamic_cast<FunctionDecl *>(m.get())) {
                        if (!mf->tparams.empty()) continue;
                        emit_function_def(mf, true, c->name);
                    }
                }
                if (c->destructor) {
                    if (auto *df = dynamic_cast<FunctionDecl *>(c->destructor.get()))
                        emit_function_def(df, true, c->name);
                }
                current_class_.clear();
                current_class_c_.clear();
            }
        }

        // 泛型单态化定义（原型已在上面统一输出）
        for (auto &[decl, insts]: generic_insts) {
            const FunctionInfo *gfi = fi_of_decl(decl);
            const bool gmethod = gfi && gfi->is_method;
            const std::string gowner = gmethod ? gfi->owner_class : std::string();
            for (auto &subst: insts) emit_function_def_generic(decl, subst, gmethod, gowner);
        }
    }

    // ==========================================================================
    // 运行时支持库：独立 .h / .c（不再逐模块内嵌）
    // ==========================================================================
    std::string Codegen::runtime_header() {
        return
                "/* sinter runtime support - generated by sinterc, do not edit */\n"
                "#ifndef SINTER_RUNTIME_H\n"
                "#define SINTER_RUNTIME_H\n"
                "/* POSIX feature-test macros must precede every system header. */\n"
                "#ifndef _POSIX_C_SOURCE\n"
                "#define _POSIX_C_SOURCE 200809L\n"
                "#endif\n"
                "#ifndef _DEFAULT_SOURCE\n"
                "#define _DEFAULT_SOURCE 1\n"
                "#endif\n"
                "\n"
                "/* No system headers are included: fixed-width types come from compiler\n"
                "   builtins so they are identical to the target libc's int64_t/uint64_t. */\n"
                "#ifndef SINTER_BASIC_TYPES\n"
                "#define SINTER_BASIC_TYPES\n"
                "typedef signed char        int8_t;\n"
                "typedef __INT16_TYPE__     int16_t;\n"
                "typedef __INT32_TYPE__     int32_t;\n"
                "typedef __INT64_TYPE__     int64_t;\n"
                "typedef unsigned char      uint8_t;\n"
                "typedef __UINT16_TYPE__    uint16_t;\n"
                "typedef __UINT32_TYPE__    uint32_t;\n"
                "typedef __UINT64_TYPE__    uint64_t;\n"
                "typedef __SIZE_TYPE__      size_t;\n"
                "#endif\n"
                "\n"
                "#ifndef NULL\n"
                "#define NULL ((void*)0)\n"
                "#endif\n"
                "\n"
                "/* Portable unused marker. */\n"
                "#ifndef sinter_maybe_unused\n"
                "#if defined(__GNUC__) || defined(__clang__)\n"
                "#define sinter_maybe_unused __attribute__((unused))\n"
                "#else\n"
                "#define sinter_maybe_unused\n"
                "#endif\n"
                "#endif\n"
                "\n"
                "/* libc declarations (C11 signatures, no system headers). */\n"
                "#ifndef SINTER_LIBC_DECLS\n"
                "#define SINTER_LIBC_DECLS\n"
                "typedef size_t sinter_size_t;\n"
                "void* malloc(sinter_size_t);\n"
                "void* calloc(sinter_size_t, sinter_size_t);\n"
                "void* realloc(void*, sinter_size_t);\n"
                "void  free(void*);\n"
                "void* memcpy(void*, const void*, sinter_size_t);\n"
                "void* memmove(void*, const void*, sinter_size_t);\n"
                "void* memset(void*, int, sinter_size_t);\n"
                "int   memcmp(const void*, const void*, sinter_size_t);\n"
                "sinter_size_t strlen(const char*);\n"
                "int   strcmp(const char*, const char*);\n"
                "int   puts(const char*);\n"
                "int   putchar(int);\n"
                "void  exit(int);\n"
                "void  abort(void);\n"
                "double sqrt(double);\n"
                "double pow(double, double);\n"
                "double fabs(double);\n"
                "double floor(double);\n"
                "double ceil(double);\n"
                "#endif\n"
                "\n"
"/* Virtual dispatch. Every vtable slot has the same function-pointer type `sinter_vfn`:\n"
                 "   call sites are written as `((T(*)(...))obj->__vptr[i])(args)`, which is a\n"
                 "   function-pointer-to-function-pointer conversion; ISO C 6.3.2.3 allows it,\n"
                 "   so `-std=c99 -pedantic-errors` no longer reports \"ISO C forbids conversion of\n"
                 "   object pointer to function pointer type\". */\n"
                 "#ifndef SINTER_VSLOT_DECLS\n"
                 "#define SINTER_VSLOT_DECLS\n"
                 "typedef void (*sinter_vfn)(void);\n"
                 "#endif\n"
                "\n"
                "#ifdef __cplusplus\n"
                "extern \"C\" {\n"
                "#endif\n"
                "\n"
                "/* ---- internal string ---------------------------------------------------- */\n"
                "typedef struct sinter_string {\n"
                "    char*   data;\n"
                "    int64_t len;\n"
                "    int64_t cap;\n"
                "    /* owned=1: data is heap memory and is freed on destruction.\n"
                "       owned=0: data points into the static literal pool, nothing to free. */\n"
                "    int32_t owned;\n"
                "} sinter_string;\n"
                "\n"
                "sinter_string sinter_string_from_literal(const char* s, int64_t len);\n"
                "sinter_string sinter_string_from_cstr(const char* s);\n"
                "sinter_string sinter_string_from_buf(const char* buf, int64_t len);\n"
                "sinter_string sinter_string_clone(sinter_string s);\n"
                "sinter_string sinter_string_from_int(int64_t v);\n"
                "sinter_string sinter_string_from_num(double v);\n"
                "sinter_string sinter_string_from_bool(uint8_t v);\n"
                "sinter_string sinter_string_concat(sinter_string a, sinter_string b);\n"
                "int32_t sinter_string_compare(sinter_string a, sinter_string b);\n"
                "uint8_t sinter_string_equals(sinter_string a, sinter_string b);\n"
                "const char* sinter_string_to_cstr(sinter_string s);\n"
                "void sinter_string_free(sinter_string s);\n"
                "\n"
                "/* ---- exceptions + diagnostics ---- */\n"
                "#include <setjmp.h>\n"
                "#if defined(_MSC_VER)\n"
                "#define SINTER_TLS __declspec(thread)\n"
                "#else\n"
                "#define SINTER_TLS __thread\n"
                "#endif\n"
                "typedef struct sinter_exc_frame { jmp_buf buf; struct sinter_exc_frame* prev; int cleanup_top; } sinter_exc_frame;\n"
                "extern SINTER_TLS sinter_exc_frame* sinter_exc_top;\n"
                "extern SINTER_TLS sinter_string sinter_exc_value;\n"
                "extern SINTER_TLS char sinter_exc_where[320];\n"
                "void sinter_throw(sinter_string msg, const char* file, int line);\n"
                "sinter_string sinter_exc_take(void);\n"
                "void sinter_backtrace(void);\n"
                "\n"
                "typedef void (*sinter_cleanup_fn)(void*);\n"
                "void sinter_cleanup_push(void* p, sinter_cleanup_fn fn);\n"
                "void sinter_cleanup_pop(int n);\n"
                "void sinter_cleanup_run_to(int mark);\n"
                "int sinter_cleanup_mark(void);\n"
                "int sinter_cleanup_depth(void);\n"
                "void sinter_cleanup_reset(void);\n"
                "void sinter_cleanup_string(void* p);\n"
                "void sinter_string_assign(sinter_string* dst, sinter_string src);\n"
                "sinter_string sinter_string_concat_take_left(sinter_string a, sinter_string b);\n"
                "sinter_string sinter_string_concat_take_right(sinter_string a, sinter_string b);\n"
                "sinter_string sinter_string_concat_own(sinter_string a, sinter_string b);\n"
                "\n"
                "/* Compile-time literal initializer (the malloc version is runtime only). */\n"
                "#define sinter_string_static(lit, n) { (char*)(lit), (int64_t)(n), (int64_t)(n), 0 }\n"
                "\n"
                "const char* sinter_int_to_str(int64_t v);\n"
                "const char* sinter_num_to_str(double v);\n"
                "\n"
                "/* String concatenation (returns heap memory). */\n"
                "const char* sinter_str_concat(const char* a, const char* b);\n"
                "\n"
                "/* Runtime type test: chain looks like {\"Dog\", \"Animal\", 0}. */\n"
                "uint8_t sinter_type_is(const char* const* chain, const char* want);\n"
                "\n"
                "#ifdef __cplusplus\n"
                "}\n"
                "#endif\n"
                "\n"
                "#endif /* SINTER_RUNTIME_H */\n"
                "\n";
    }

    std::string Codegen::runtime_source() {
        return
                "/* Sinter runtime support library - generated by sinterc, do not edit */\n"
                "#include \"sinter_runtime.h\"\n"
                "\n"
                "#include <stdio.h>\n"
                "#include <stdlib.h>\n"
                "#include <string.h>\n"
                "#ifdef _WIN32\n"
                "#include <windows.h>\n"
                "#elif !defined(__ANDROID__)\n"
                "#include <execinfo.h>\n"
                "#endif\n"
                "\n"
                                "/* ---- sinter_string implementation ---- */\n"
                "static sinter_string sinter_string_alloc(int64_t cap) {\n"
                "    sinter_string s;\n"
                "    s.len = 0;\n"
                "    s.cap = cap < 8 ? 8 : cap;\n"
                "    s.data = (char*)malloc((size_t)s.cap + 1);\n"
                "    s.owned = 1;\n"
                "    if (!s.data) { s.cap = 0; s.owned = 0; return s; }\n"
                "    s.data[0] = 0;\n"
                "    return s;\n"
                "}\n"
                "\n"
                "/* Compile-time literals: no copy, point straight at bytes in .rodata (owned=0).\n"
                "   This avoids a malloc per evaluation and lets the destructor skip them safely. */\n"
                "sinter_string sinter_string_from_literal(const char* src, int64_t len) {\n"
                "    sinter_string s;\n"
                "    if (len < 0) len = 0;\n"
                "    if (len > 0 && !src) len = 0;\n"
                "    s.data = (char*)(len > 0 ? src : \"\");\n"
                "    s.len = len;\n"
                "    s.cap = len;\n"
                "    s.owned = 0;\n"
                "    return s;\n"
                "}\n"
                "\n"
                "/* When an owned copy is required (e.g. building from a C buffer), copy to the heap. */\n"
                "static sinter_string sinter_string_copy_from(const char* src, int64_t len) {\n"
                "    if (len < 0) len = 0;\n"
                "    if (len > 0 && !src) len = 0;\n"
                "    sinter_string s = sinter_string_alloc(len);\n"
                "    if (!s.data) { s.len = 0; return s; }\n"
                "    if (len > 0) memcpy(s.data, src, (size_t)len);\n"
                "    s.data[len] = 0;\n"
                "    s.len = len;\n"
                "    return s;\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_from_cstr(const char* src) {\n"
                "    if (!src) return sinter_string_alloc(0);\n"
                "    return sinter_string_copy_from(src, (int64_t)strlen(src));\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_clone(sinter_string s) {\n"
                "    return sinter_string_copy_from(s.data, s.len);\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_from_buf(const char* buf, int64_t n) {\n"
                "    if (n < 0) return sinter_string_alloc(0);\n"
                "    return sinter_string_copy_from(buf, (int64_t)n);\n"
"}\n"
                 "\n"
                 "sinter_string sinter_string_from_int(int64_t v) {\n"
                 "    /* Decimal conversion is done by hand so the runtime only needs <stdio.h>\n"
                 "       for one float-formatting call site. INT64_MIN would overflow on negation,\n       so accumulate through an unsigned value. */\n"
                 "    char buf[24];\n"
                 "    int n = 0;\n"
                 "    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;\n"
                 "    if (v < 0) buf[n++] = '-';\n"
                 "    char tmp[20];\n"
                 "    int m = 0;\n"
                 "    do { tmp[m++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u);\n"
                 "    while (m > 0) buf[n++] = tmp[--m];\n"
                 "    return sinter_string_from_buf(buf, n);\n"
                 "}\n"
                 "\n"
                 "sinter_string sinter_string_from_num(double v) {\n"
                 "    /* Full %g semantics (precision, exponent switching, round-trip consistency)\n"
                 "       are not worth reimplementing by hand, so the runtime keeps one snprintf here.\n       Generated user code does not need <stdio.h>. */\n"
                 "    char buf[64];\n"
                 "    return sinter_string_from_buf(buf, snprintf(buf, sizeof(buf), \"%g\", v));\n"
                 "}\n"
                "\n"
                "sinter_string sinter_string_from_bool(uint8_t v) {\n"
                "    return sinter_string_from_cstr(v ? \"true\" : \"false\");\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_concat(sinter_string a, sinter_string b) {\n"
                "    const int64_t need = a.len + b.len;\n"
                "    sinter_string s = sinter_string_alloc(need);\n"
                "    if (!s.data) { s.len = 0; return s; }\n"
                "    if (a.len > 0 && a.data) memcpy(s.data, a.data, (size_t)a.len);\n"
                "    if (b.len > 0 && b.data) memcpy(s.data + a.len, b.data, (size_t)b.len);\n"
                "    s.data[need] = 0;\n"
                "    s.len = need;\n"
                "    return s;\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_concat_take_right(sinter_string a, sinter_string b) {\n"
                "    sinter_string s = sinter_string_concat(a, b);\n"
                "    /* The right operand is a temporary and nobody needs it after concatenation,\n"
                "       so its buffer is released here. The left operand is never touched: the\n       caller may still be using it. */\n"
                "    if (b.owned && b.data) free(b.data);\n"
                "    return s;\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_concat_take_left(sinter_string a, sinter_string b) {\n"
                "    sinter_string s = sinter_string_concat(a, b);\n"
                "    if (a.owned && a.data) free(a.data);\n"
                "    return s;\n"
                "}\n"
                "\n"
                "sinter_string sinter_string_concat_own(sinter_string a, sinter_string b) {\n"
                "    sinter_string s = sinter_string_concat(a, b);\n"
                "    if (a.owned && a.data) free(a.data);\n"
                "    if (b.owned && b.data) free(b.data);\n"
                "    return s;\n"
                "}\n"
                "\n"
                "int32_t sinter_string_compare(sinter_string a, sinter_string b) {\n"
                "    const int64_t n = a.len < b.len ? a.len : b.len;\n"
                "    if (n > 0 && a.data && b.data) {\n"
                "        const int r = memcmp(a.data, b.data, (size_t)n);\n"
                "        if (r) return r < 0 ? -1 : 1;\n"
                "    }\n"
                "    if (a.len == b.len) return 0;\n"
                "    return a.len < b.len ? -1 : 1;\n"
                "}\n"
                "\n"
                "uint8_t sinter_string_equals(sinter_string a, sinter_string b) {\n"
                "    return (uint8_t)(sinter_string_compare(a, b) == 0);\n"
                "}\n"
                "\n"
                "const char* sinter_string_to_cstr(sinter_string s) {\n"
                "    return s.data ? s.data : \"\";\n"
                "}\n"
                "\n"
                "void sinter_string_free(sinter_string s) {\n"
                "    /* Only heap-allocated buffers are freed; static literals (owned=0) are left alone.\n"
                "       Freeing an owned=0 literal buffer would crash. */\n"
                "    if (s.owned && s.data) free(s.data);\n"
                "}\n"
                "\n"
                "void sinter_string_assign(sinter_string* dst, sinter_string src) {\n"
                "    if (!dst) return;\n"
                "    sinter_string_free(*dst);\n"
                "    *dst = src;\n"
                "}\n"
                "\n"
"const char* sinter_int_to_str(int64_t v) {\n"
                "    char buf[32];\n"
                "    int n = snprintf(buf, sizeof(buf), \"%lld\", (long long)v);\n"
                "    if (n < 0) return NULL;\n"
                "    char* out = (char*)malloc((size_t)n + 1);\n"
                "    if (!out) return NULL;\n"
                "    memcpy(out, buf, (size_t)n + 1);\n"
                "    return out;\n"
                "}\n"
                "\n"
                "const char* sinter_num_to_str(double v) {\n"
                "    char buf[64];\n"
                "    int n = snprintf(buf, sizeof(buf), \"%g\", v);\n"
                "    if (n < 0) return NULL;\n"
                "    char* out = (char*)malloc((size_t)n + 1);\n"
                "    if (!out) return NULL;\n"
                "    memcpy(out, buf, (size_t)n + 1);\n"
                "    return out;\n"
                "}\n"
                "\n"
                "const char* sinter_str_concat(const char* a, const char* b) {\n"
                "    if (!a) a = \"\";\n"
                "    if (!b) b = \"\";\n"
                "    size_t la = strlen(a), lb = strlen(b);\n"
                "    char* out = (char*)malloc(la + lb + 1);\n"
                "    if (!out) return NULL;\n"
                "    memcpy(out, a, la);\n"
                "    memcpy(out + la, b, lb + 1);\n"
                "    return out;\n"
                "}\n"
                "\n"
                "uint8_t sinter_type_is(const char* const* chain, const char* want) {\n"
                "    if (!chain || !want) return 0;\n"
                "    for (int i = 0; chain[i]; ++i) {\n"
                "        const char* a = chain[i];\n"
                "        const char* b = want;\n"
                "        while (*a && *b && *a == *b) { ++a; ++b; }\n"
                "        if (*a == 0 && *b == 0) return 1;\n"
                "    }\n"
                "    return 0;\n"
                "}\n"
                "\n"
                "/* ---- exceptions ---- */\n"
                "SINTER_TLS sinter_exc_frame* sinter_exc_top = NULL;\n"
                "SINTER_TLS sinter_string sinter_exc_value;\n"
                "SINTER_TLS char sinter_exc_where[320];\n"
                "\n"
                "void sinter_backtrace(void) {\n"
                "#ifdef _WIN32\n"
                "    void* frames[32];\n"
                "    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);\n"
                "    for (USHORT i = 0; i < n; ++i) fprintf(stderr, \"  #%u %p\\n\", (unsigned)i, frames[i]);\n"
                "#elif defined(__ANDROID__)\n"
                "    fprintf(stderr, \"  (backtrace unavailable on Android)\\n\");\n"
                "#else\n"
                "    void* frames[32];\n"
                "    int n = backtrace(frames, 32);\n"
                "    backtrace_symbols_fd(frames, n, 2);\n"
                "#endif\n"
                "}\n"
                "\n"
                "/* ---- exception cleanup stack (RAII across longjmp) ---- */\n"
                "SINTER_TLS void** sinter_cleanup_ptrs = NULL;\n"
                "SINTER_TLS sinter_cleanup_fn* sinter_cleanup_fns = NULL;\n"
                "SINTER_TLS int sinter_cleanup_n = 0;\n"
                "SINTER_TLS int sinter_cleanup_cap = 0;\n"
                "\n"
                "int sinter_cleanup_mark(void) { return sinter_cleanup_n; }\n"
                "int sinter_cleanup_depth(void) { return sinter_cleanup_n; }\n"
                "void sinter_cleanup_reset(void) { sinter_cleanup_n = 0; }\n"
                "void sinter_cleanup_push(void* p, sinter_cleanup_fn fn) {\n"
                "    if (!fn) return;\n"
                "    if (sinter_cleanup_n >= sinter_cleanup_cap) {\n"
                "        int nc = sinter_cleanup_cap ? sinter_cleanup_cap * 2 : 256;\n"
                "        void** np = (void**)realloc(sinter_cleanup_ptrs, sizeof(void*) * (size_t)nc);\n"
                "        sinter_cleanup_fn* nf = (sinter_cleanup_fn*)realloc(sinter_cleanup_fns, sizeof(sinter_cleanup_fn) * (size_t)nc);\n"
                "        if (!np || !nf) abort();\n"
                "        sinter_cleanup_ptrs = np;\n"
                "        sinter_cleanup_fns = nf;\n"
                "        sinter_cleanup_cap = nc;\n"
                "    }\n"
                "    sinter_cleanup_ptrs[sinter_cleanup_n] = p;\n"
                "    sinter_cleanup_fns[sinter_cleanup_n] = fn;\n"
                "    sinter_cleanup_n++;\n"
                "}\n"
                "void sinter_cleanup_pop(int n) {\n"
                "    if (n <= 0) return;\n"
                "    if (n > sinter_cleanup_n) n = sinter_cleanup_n;\n"
                "    sinter_cleanup_n -= n;\n"
                "}\n"
                "void sinter_cleanup_run_to(int mark) {\n"
                "    if (mark < 0) mark = 0;\n"
                "    while (sinter_cleanup_n > mark) {\n"
                "        sinter_cleanup_n--;\n"
                "        sinter_cleanup_fns[sinter_cleanup_n](sinter_cleanup_ptrs[sinter_cleanup_n]);\n"
                "    }\n"
                "}\n"
                "void sinter_cleanup_string(void* p) {\n"
                "    sinter_string* s = (sinter_string*)p;\n"
                "    sinter_string_free(*s);\n"
                "    s->data = 0; s->len = 0; s->cap = 0; s->owned = 0;\n"
                "}\n"
                "\n"
                "void sinter_throw(sinter_string msg, const char* file, int line) {\n"
                "    sinter_string prev = sinter_exc_value;\n"
                "    sinter_exc_value = sinter_string_clone(msg);\n"
                "    if (prev.owned) sinter_string_free(prev);\n"
                "    snprintf(sinter_exc_where, sizeof(sinter_exc_where), \"%s:%d\", file ? file : \"?\", line);\n"
                "    if (!sinter_exc_top) {\n"
                "        sinter_cleanup_run_to(0);\n"
                "        fputs(\"uncaught exception: \", stderr);\n"
                "        if (sinter_exc_value.data) fwrite(sinter_exc_value.data, 1, (size_t)sinter_exc_value.len, stderr);\n"
                "        fprintf(stderr, \" (at %s)\\n\", sinter_exc_where);\n"
                "        sinter_backtrace();\n"
                "        fflush(stderr);\n"
                "        abort();\n"
                "    }\n"
                "    sinter_cleanup_run_to(sinter_exc_top->cleanup_top);\n"
                "    longjmp(sinter_exc_top->buf, 1);\n"
                "}\n"
                "\n"
                "sinter_string sinter_exc_take(void) {\n"
                "    sinter_string v = sinter_exc_value;\n"
                "    sinter_exc_value = sinter_string_from_literal(\"\", 0);\n"
                "    return v;\n"
                "}\n";
    }

    std::string Codegen::generate_c(Program &p, const std::string &module_name) {
        if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] generate_c " << module_name << "\n";
        emit_source_only_ = true;
        module_name_ = module_name;
        module_ident_ = sem_.module_to_ident(module_name);
        indent_ = 0;
        tmp_ = 0;
        locals_.clear();
        local_types_.clear();
        used_.clear();
        scopes_.clear();
        scope_cleanup_counts_.clear();
        try_depth_ = 0;
        active_finally_.clear();
        type_decls_.clear();
        fun_decls_.clear();
        vtable_defs_.clear();
        h_pre_.str("");
        h_funs_.str("");
        h_types_.str("");
        c_embed_.str("");
        c_funs_.str("");
        pending_.str("");

        // 不自动引入任何系统头文件：所需类型与 libc 声明全部由 sinter_runtime.h
        // 提供，用户需要更多 C 能力时用 `embed C` 自行 #include。
        // `bind C header "x.h"`：把用户声明的 C 头放在模块头之前，
        // 因为模块头里的原型可能引用头文件里的不透明类型（FILE* 等）。
        for (auto &s: p.statements)
            if (auto *h = dynamic_cast<CBindHeaderStmt *>(s.get()))
                c_embed_ << "#include " << char(34) << h->header << char(34) << char(10);
        c_embed_ << "#include \"sinter_runtime.h\"\n";
        c_embed_ << "#include \"sinter_" << module_ident_ << ".h\"\n\n";

        for (auto &s: p.statements) {
            if (auto *e = dynamic_cast<EmbedCStmt *>(s.get())) {
                c_embed_ << e->code << "\n";
            }
        }
        c_embed_ << "\n";

        emit_program_body(p);
        if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] body done\n";

        std::ostringstream priv;
        for (auto &d: type_decls_) if (!d.is_pub) priv << d.text;
        for (auto &d: fun_decls_) if (!d.is_pub) priv << d.text;
        for (auto &v: vtable_defs_) priv << v;

        std::ostringstream out;
        out << c_embed_.str();
        if (!priv.str().empty())
            out << "/* ---- module-private declarations (not exported to the header) ---- */\n" << priv.str() << "\n";
        out << pending_.str() << c_funs_.str();
        return out.str();
    }

    std::string Codegen::generate_h(Program &p, const std::string &module_name) {
        emit_source_only_ = false;   // .h 这一轮不重复报告诊断
        module_name_ = module_name;
        module_ident_ = sem_.module_to_ident(module_name);
        indent_ = 0;
        tmp_ = 0;
        locals_.clear();
        local_types_.clear();
        used_.clear();
        scopes_.clear();
        scope_cleanup_counts_.clear();
        try_depth_ = 0;
        active_finally_.clear();
        type_decls_.clear();
        fun_decls_.clear();
        vtable_defs_.clear();
        h_pre_.str("");
        h_funs_.str("");
        h_types_.str("");
        c_embed_.str("");
        c_funs_.str("");
        pending_.str("");

        emit_program_body(p);

        // Cross-module generic calls need the monomorphized prototype in the
        // defining module's header, otherwise the caller sees an implicit decl.
        std::ostringstream generic_h;
        if (external_generic_insts_) {
            generic_proto_out_ = &generic_h;
            for (auto &[decl, substs]: *external_generic_insts_) {
                const FunctionInfo *fi = fi_of_decl(decl);
                if (!fi || !fi->is_pub) continue;
                for (auto &subst: substs)
                    emit_function_proto_generic(decl, subst, fi->is_method, fi->owner_class);
            }
            generic_proto_out_ = nullptr;
        }

        std::string guard = "SINTER_MODULE_" + module_ident_ + "_H";
        std::ranges::transform(guard, guard.begin(), ::toupper);

        std::vector<std::string> imports;
        collect_imports(p, imports);

        std::ostringstream out;
        out << "/* Sinter module " << module_name << " public interface - generated by sinterc */\n";
        out << "#ifndef " << guard << "\n#define " << guard << "\n\n";
        out << "#include \"sinter_runtime.h\"\n";
        for (auto &m: imports) out << "#include \"sinter_" << m << ".h\"\n";
        out << h_pre_.str();
        out << "\n/* ---- embed C header: 显式公开给其它编译单元的 C 片段 ---- */\n";
        for (auto &s2: p.statements) {
            if (auto *e = dynamic_cast<EmbedCStmt *>(s2.get()); e && e->in_header)
                out << e->code << "\n";
        }
        out << "\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

        // 只有 pub 声明进入头文件；其余为模块内私有。
        for (auto &d: type_decls_) if (d.is_pub) out << d.text;
        for (auto &d: fun_decls_) if (d.is_pub) out << d.text;
        out << generic_h.str();

        out << "\n#ifdef __cplusplus\n} /* extern \"C\" */\n#endif\n\n";
        out << "#endif /* " << guard << " */\n";
        return out.str();
    }
} // namespace sinter
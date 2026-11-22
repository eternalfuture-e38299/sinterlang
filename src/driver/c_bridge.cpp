// c_bridge.cpp —— C 头文件声明 → Sinter AST 节点
#include "c_bridge.hpp"

#include "cfront/c_parser.hpp"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace sinter_bridge {
    namespace {
        using sinter::TypeAnnotation;
        using sinter::cfront::CDeclarator;
        using sinter::cfront::TypeSpec;

        std::string read_file(const std::string &p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return {};
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        bool is_void_spec(const TypeSpec &s) {
            return s.kinds.size() == 1 && s.kinds[0] == sinter::cfront::TypeSpecKind::Void;
        }

        bool is_typedef_spec(const TypeSpec &s) {
            for (const auto k: s.kinds)
                if (k == sinter::cfront::TypeSpecKind::TypedefName) return true;
            return false;
        }

        /// C 标量类型名 → Sinter 类型名的唯一映射表。
        /// 缺失的一律报错，而不是猜一个「差不多」的宽度 —— ABI 猜错是静默错误。
        const char *map_scalar(const std::string &c) {
            static const std::map<std::string, const char *> m = {
                {"void", "void"},
                {"char", "char"}, {"signed char", "i8"}, {"unsigned char", "u8"},
                {"short", "i16"}, {"short int", "i16"},
                {"signed short", "i16"}, {"signed short int", "i16"},
                {"unsigned short", "u16"}, {"unsigned short int", "u16"},
                {"int", "i32"}, {"signed", "i32"}, {"signed int", "i32"},
                {"unsigned", "u32"}, {"unsigned int", "u32"},
                {"long", "i64"}, {"long int", "i64"},
                {"signed long", "i64"}, {"signed long int", "i64"},
                {"unsigned long", "u64"}, {"unsigned long int", "u64"},
                {"long long", "i64"}, {"long long int", "i64"}, {"signed long long", "i64"},
                {"unsigned long long", "u64"}, {"unsigned long long int", "u64"},
                {"float", "f32"}, {"double", "f64"}, {"long double", "f64"},
                {"_Bool", "bool"}, {"bool", "bool"},
                // 定宽 typedef（stdint.h）
                {"int8_t", "i8"}, {"int16_t", "i16"}, {"int32_t", "i32"}, {"int64_t", "i64"},
                {"uint8_t", "u8"}, {"uint16_t", "u16"}, {"uint32_t", "u32"}, {"uint64_t", "u64"},
                // 指针宽度 / 标准整数别名（目标按 64 位）
                {"size_t", "u64"}, {"uintptr_t", "u64"}, {"uintmax_t", "u64"},
                {"ssize_t", "i64"}, {"intptr_t", "i64"}, {"ptrdiff_t", "i64"},
                {"intmax_t", "i64"}, {"time_t", "i64"}, {"off_t", "i64"},
                {"wchar_t", "i32"}, {"wint_t", "u32"},
            };
            const auto it = m.find(c);
            return it == m.end() ? nullptr : it->second;
        }

        /// 只能以指针形式出现的不透明 C 类型（typedef 到不完整结构体的那类）。
        bool is_opaque_typedef(const std::string &c) {
            static const std::set<std::string> s = {"FILE", "va_list", "jmp_buf", "DIR"};
            return s.contains(c);
        }

        struct Mapper {
            std::set<std::string> opaque;                  // Sinter 名
            std::map<std::string, std::string> opaque_c;   // Sinter 名 → C 拼写
            std::set<std::string> typedef_structs;          // `typedef struct {...} Foo;` 的 Foo
            std::string error;

            static std::unique_ptr<TypeAnnotation> named(const std::string &n) {
                auto t = std::make_unique<TypeAnnotation>();
                t->kind = TypeAnnotation::Named;
                t->name = n;
                return t;
            }

            static std::unique_ptr<TypeAnnotation> ptr_to(std::unique_ptr<TypeAnnotation> inner) {
                auto t = std::make_unique<TypeAnnotation>();
                t->kind = TypeAnnotation::Pointer;
                t->inner_type = std::move(inner);
                return t;
            }

            /// 加一级指针：`char*` → cstr、`void*` → ptr，其余正常加星。
            std::unique_ptr<TypeAnnotation> wrap_ptr(std::unique_ptr<TypeAnnotation> inner) const {
                if (inner && inner->kind == TypeAnnotation::Named) {
                    if (inner->name == "char") return named("cstr");
                    if (inner->name == "void") return named("ptr");
                    if (inner->name == "cstr") return named("ptr");   // char**
                }
                return ptr_to(std::move(inner));
            }

            std::unique_ptr<TypeAnnotation> base_of(const TypeSpec &s, bool &is_struct,
                                                    std::string &tag) {
                is_struct = false;
                tag.clear();
                const std::string c = sinter::cfront::canonical_type(s);
                if (c.rfind("struct ", 0) == 0 || c.rfind("union ", 0) == 0) {
                    is_struct = true;
                    tag = c.substr(c.find(' ') + 1);
                    opaque.insert(tag);
                    opaque_c[tag] = c;   // 生成 C 里要用 `struct Foo`
                    return named(tag);
                }
                if (c.rfind("enum ", 0) == 0) {
                    // C 的 enum 实体就是 int（C99 6.7.2.2）；常量本轮不逐个登记。
                    return named("i32");
                }
                if (const char *m = map_scalar(c)) return named(m);
                if (is_typedef_spec(s) && typedef_structs.contains(c)) {
                    // typedef 出来的结构体：C 侧拼写就是 typedef 名本身（`Foo*`）
                    is_struct = true;
                    tag = c;
                    opaque.insert(c);
                    opaque_c[c] = c;
                    return named(c);
                }
                if (is_typedef_spec(s) && is_opaque_typedef(c)) {
                    is_struct = true;
                    tag = c;
                    opaque.insert(c);
                    opaque_c[c] = c;
                    return named(c);
                }
                error = "cannot map C type '" + c + "' to a sinter type";
                return nullptr;
            }

            /// 把「基类型 + 声明符」变成 Sinter 类型注解。
            /// 关键规则：`struct X*` 在 Sinter 里就是 `X`（类类型本身就是指针），
            /// 所以指针层级要吃掉一层；`struct X`（按值）直接报错。
            std::unique_ptr<TypeAnnotation> build(const TypeSpec &s, const CDeclarator *d,
                                                  std::string &err) {
                bool is_struct = false;
                std::string tag;
                auto t = base_of(s, is_struct, tag);
                if (!t) {
                    err = error;
                    return nullptr;
                }
                bool ptr_seen = false;
                const CDeclarator *cur = d;
                while (cur) {
                    if (cur->kind == CDeclarator::Kind::Paren) {
                        cur = cur->inner.get();
                        continue;
                    }
                    if (cur->kind == CDeclarator::Kind::Identifier) break;
                    if (cur->kind == CDeclarator::Kind::Function) return named("ptr");
                    if (cur->kind == CDeclarator::Kind::Array ||
                        cur->kind == CDeclarator::Kind::Pointer) {
                        if (is_struct && !ptr_seen) {
                            ptr_seen = true;   // struct Foo* → 直接就是 Foo
                            cur = cur->inner.get();
                            continue;
                        }
                        t = wrap_ptr(std::move(t));
                        ptr_seen = true;
                        cur = cur->inner.get();
                        continue;
                    }
                    break;
                }
                if (is_struct && !ptr_seen) {
                    err = "C type '" + tag +
                          "' cannot be passed by value: sinter does not know its layout; pass a pointer";
                    return nullptr;
                }
                return t;
            }

            /// 函数返回类型：只把「外层」包装（在 Function 节点之前的）作用到基类型上。
            /// `char *f(void)` 的树是 Pointer{Function{...}}，返回类型应为 char*。
            std::unique_ptr<TypeAnnotation> return_type(const TypeSpec &s, const CDeclarator *root,
                                                        const CDeclarator *fn_node, std::string &err) {
                bool is_struct = false;
                std::string tag;
                auto t = base_of(s, is_struct, tag);
                if (!t) {
                    err = error;
                    return nullptr;
                }
                bool ptr_seen = false;
                const CDeclarator *cur = root;
                while (cur && cur != fn_node) {
                    if (cur->kind == CDeclarator::Kind::Pointer) {
                        if (is_struct && !ptr_seen) ptr_seen = true;
                        else {
                            t = wrap_ptr(std::move(t));
                            ptr_seen = true;
                        }
                    }
                    cur = cur->inner.get();
                }
                if (is_struct && !ptr_seen) {
                    err = "C function returning '" + tag +
                          "' by value is not supported: pass a pointer instead";
                    return nullptr;
                }
                return t;
            }

            /// 最靠近标识符的那层包装是 Function ⇔ 这是函数声明（而不是函数指针变量）。
            static const CDeclarator *function_node(const CDeclarator *d) {
                const CDeclarator *innermost = nullptr;
                const CDeclarator *cur = d;
                while (cur && cur->kind != CDeclarator::Kind::Identifier) {
                    innermost = cur;
                    cur = cur->inner.get();
                }
                if (innermost && innermost->kind == CDeclarator::Kind::Function) return innermost;
                return nullptr;
            }
        };

        std::string pos_prefix(const std::string &display, const sinter::cfront::CUnitItem &item) {
            std::ostringstream os;
            os << display << ":" << item.range.begin_line << ":" << item.range.begin_col << ": ";
            return os.str();
        }
    } // namespace

    Result load_header(const std::string &path, const std::string &display,
                       const Options &options) {
        Result r;
        const std::string src = read_file(path);
        if (src.empty()) {
            r.errors.push_back(display + ": cannot read C header (E0801)");
            return r;
        }

        sinter::cfront::CParseOptions opts;
        opts.include_dirs = options.include_dirs;
        opts.predefined = options.predefined;
        const sinter::cfront::CParseResult parsed = sinter::cfront::CParser::parse(src, path, opts);

        for (const auto &e: parsed.errors) {
            std::ostringstream os;
            // e.file 是真正出错的文件：bind 的头自己出错时等于 resolved 路径，
            // 出错位置在 `#include` 进来的子头里时就是子头的路径。
            os << (e.file.empty() ? display : e.file) << ":" << e.line << ":" << e.column
               << ": C parse error: " << e.message;
            r.errors.push_back(os.str());
        }
        for (const auto &e: parsed.preproc_errors) r.errors.push_back(display + ": " + e);
        // 未解析到的 #include 不报错：系统头（<stdint.h> / <stdio.h>）本轮不展开，
        // 里面的定宽 typedef 由 map_scalar 的内置表兜底。真正的本地头缺失会在
        // 生成 C 的编译阶段暴露（#include 仍然原样写进生成物）。

        Mapper m;
        // `typedef struct {...} Foo;` / `typedef struct Tag Foo;`：真实头文件里
        // 绝大多数结构体都是这种形态，必须能把 Foo 认成不透明 C 结构体。
        for (const auto &item: parsed.unit.items) {
            if (item.kind != sinter::cfront::CUnitItem::Kind::Declaration) continue;
            if (!item.decl.is_typedef || !item.decl.decl) continue;
            const std::string c = sinter::cfront::canonical_type(item.decl.spec);
            if (c.rfind("struct ", 0) == 0 || c.rfind("union ", 0) == 0) {
                const std::string n = item.decl.name();
                if (!n.empty()) m.typedef_structs.insert(n);
            }
        }
        std::set<std::string> emitted_structs;

        for (const auto &item: parsed.unit.items) {
            const sinter::cfront::CDeclItem *decl = nullptr;
            if (item.kind == sinter::cfront::CUnitItem::Kind::FunctionDef && item.func)
                decl = &item.func->decl;
            else if (item.kind == sinter::cfront::CUnitItem::Kind::Declaration)
                decl = &item.decl;
            else
                continue;
            if (!decl || !decl->decl) continue;

            const CDeclarator *fn = Mapper::function_node(decl->decl.get());
            if (!fn) continue;   // 变量声明 / 函数指针变量：本轮不登记

            const std::string name = decl->decl->leaf_name();
            if (name.empty()) continue;

            std::string err;
            auto ret = m.return_type(decl->spec, decl->decl.get(), fn, err);
            if (!ret) {
                r.errors.push_back(pos_prefix(display, item) + "error[E0806]: " + err +
                                   " (function '" + name + "')");
                ++r.skipped;
                continue;
            }

            auto b = std::make_unique<sinter::BindCFunctionStmt>();
            b->c_name = name;
            b->line = static_cast<int>(item.range.begin_line);
            b->col = static_cast<int>(item.range.begin_col);
            b->variadic = fn->variadic;
            b->return_type = std::move(ret);

            bool bad_param = false;
            size_t index = 0;
            for (const auto &p: fn->params) {
                // `f(void)`：唯一的形参是 void 且没有名字 → 无参
                if (is_void_spec(p.spec) && !p.decl) {
                    ++index;
                    continue;
                }
                std::string perr;
                auto pt = m.build(p.spec, p.decl.get(), perr);
                if (!pt) {
                    r.errors.push_back(pos_prefix(display, item) + "error[E0806]: " + perr +
                                       " (parameter " + std::to_string(index + 1) + " of '" + name + "')");
                    bad_param = true;
                    break;
                }
                sinter::Parameter prm;
                prm.name = p.name().empty() ? ("arg" + std::to_string(index)) : p.name();
                prm.type = std::move(pt);
                b->params.push_back(std::move(prm));
                ++index;
            }
            if (bad_param) {
                ++r.skipped;
                continue;
            }
            r.nodes.push_back(std::move(b));
            ++r.functions;
        }

        // 不透明结构体：每个名字只登记一次，且排在函数之前（类型先于使用）
        std::vector<sinter::StmtPtr> structs;
        for (const auto &name: m.opaque) {
            if (!emitted_structs.insert(name).second) continue;
            auto s = std::make_unique<sinter::CStructDeclStmt>();
            s->name = name;
            s->c_name = m.opaque_c[name];
            structs.push_back(std::move(s));
            ++r.structs;
        }
        if (!structs.empty()) {
            structs.insert(structs.end(),
                           std::make_move_iterator(r.nodes.begin()),
                           std::make_move_iterator(r.nodes.end()));
            r.nodes = std::move(structs);
        }
        return r;
    }
} // namespace sinter_bridge
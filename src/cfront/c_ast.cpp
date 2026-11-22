// cfront: c_ast.cpp —— 类型规范化与符号收集工具
#include "cfront/c_ast.hpp"


namespace sinter::cfront {
    namespace {
        const char *k_type_name(const TypeSpecKind k) {
            switch (k) {
                case TypeSpecKind::Void: return "void";
                case TypeSpecKind::Char: return "char";
                case TypeSpecKind::Short: return "short";
                case TypeSpecKind::Int: return "int";
                case TypeSpecKind::Long: return "long";
                case TypeSpecKind::Float: return "float";
                case TypeSpecKind::Double: return "double";
                case TypeSpecKind::Signed: return "signed";
                case TypeSpecKind::Unsigned: return "unsigned";
                case TypeSpecKind::Bool: return "_Bool";
                case TypeSpecKind::Complex: return "_Complex";
                case TypeSpecKind::Struct: return "struct";
                case TypeSpecKind::Union: return "union";
                case TypeSpecKind::Enum: return "enum";
                case TypeSpecKind::TypedefName: return "";
                case TypeSpecKind::Typeof: return "typeof";
                case TypeSpecKind::Auto: return "auto";
                case TypeSpecKind::None: return "";
            }
            return "";
        }

    } // namespace

    std::string canonical_type(const TypeSpec &s) {
        if (s.kinds.empty()) {
            if (!s.typedef_name.empty()) return s.typedef_name;
            return "int";   // C 的隐式 int
        }
        std::string r;
        for (const auto k: s.kinds) {
            if (k == TypeSpecKind::TypedefName) {
                if (!s.typedef_name.empty()) {
                    if (!r.empty()) r += ' ';
                    r += s.typedef_name;
                }
                continue;
            }
            const char *n = k_type_name(k);
            if (!n || !*n) continue;
            if (!r.empty()) r += ' ';
            r += n;
            if (k == TypeSpecKind::Struct || k == TypeSpecKind::Union || k == TypeSpecKind::Enum) {
                if (!s.tag.empty()) {
                    r += ' ';
                    r += s.tag;
                }
            }
        }
        if (r.empty()) r = "int";
        return r;
    }

    std::string TypeSpec::canonical() const { return canonical_type(*this); }

    namespace {
        // 把声明符树里出现的 struct/union/enum 标签收集出来
        void collect_tags(const TypeSpec &spec, std::vector<std::string> &out) {
            if (spec.struct_def && !spec.struct_def->tag.empty())
                out.push_back(spec.struct_def->tag);
        }

        void collect_structs(const CTranslationUnit &tu, std::vector<std::string> &out) {
            for (const auto &item: tu.items) {
                if (item.kind == CUnitItem::Kind::FunctionDef && item.func) {
                    collect_tags(item.func->decl.spec, out);
                } else {
                    collect_tags(item.decl.spec, out);
                }
            }
        }
    } // namespace

    std::vector<std::string> CTranslationUnit::function_names() const {
        std::vector<std::string> r;
        for (const auto &item: items) {
            if (item.kind != CUnitItem::Kind::FunctionDef) continue;
            if (!item.func) continue;
            const std::string n = item.func->decl.name();
            if (!n.empty()) r.push_back(n);
        }
        return r;
    }

    std::vector<std::string> CTranslationUnit::tag_names() const {
        std::vector<std::string> r;
        collect_structs(*this, r);
        // 参数里的 struct 定义也收集
        for (const auto &item: items) {
            const CDeclItem *di = item.func ? &item.func->decl : &item.decl;
            if (!di->decl) continue;
            std::vector<const CDeclarator *> stack{di->decl.get()};
            while (!stack.empty()) {
                const CDeclarator *d = stack.back();
                stack.pop_back();
                if (!d) continue;
                for (const auto &p: d->params) {
                    if (p.spec.struct_def && !p.spec.struct_def->tag.empty())
                        r.push_back(p.spec.struct_def->tag);
                    if (p.decl) stack.push_back(p.decl.get());
                }
                if (d->inner) stack.push_back(d->inner.get());
            }
        }
        return r;
    }

    std::vector<std::string> CTranslationUnit::typedef_names() const {
        std::vector<std::string> r;
        for (const auto &item: items) {
            if (item.kind == CUnitItem::Kind::Declaration && item.decl.is_typedef) {
                const std::string n = item.decl.name();
                if (!n.empty()) r.push_back(n);
            }
        }
        return r;
    }

    std::vector<std::string> CTranslationUnit::includes() const {
        std::vector<std::string> r;
        for (const auto &item: items) {
            if (item.kind != CUnitItem::Kind::Preproc) continue;
            if (item.preproc_directive != "include") continue;
            const std::string &t = item.preproc_text;
            // #include <stdio.h>  /  #include "foo.h"
            const auto lt = t.find('<');
            const auto gt = t.find('>');
            if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
                r.push_back(t.substr(lt + 1, gt - lt - 1));
                continue;
            }
            const auto q1 = t.find('"');
            if (q1 != std::string::npos) {
                const auto q2 = t.find('"', q1 + 1);
                if (q2 != std::string::npos) r.push_back(t.substr(q1 + 1, q2 - q1 - 1));
            }
        }
        return r;
    }
} // namespace sinter::cfront
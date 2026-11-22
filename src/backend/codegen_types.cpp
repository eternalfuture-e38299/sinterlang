// sinter backend: codegen_types.cpp
#include "backend/codegen.hpp"
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <ranges>

namespace sinter {
    std::string Codegen::fresh(const std::string &b) {
        // 局部变量也按同一规则归一化为 snake_case；同时避开当前函数的形参名，
        // 否则 C 里「最外层块的同名声明」会与形参重名（C 不允许同作用域重声明）。
        const std::string base = Semantic::to_snake_ident(b);
        std::string n = base;
        while (used_.contains(n) || cur_params_.contains(n)) n = base + "_" + std::to_string(tmp_++);
        used_.insert(n);
        return n;
    }

    std::string Codegen::c_type(const TypeAnnotation *t) const { return sem_.type_to_c(t); }

    std::string Codegen::subst_type_annotation(
        const TypeAnnotation *t,
        const std::unordered_map<std::string, std::string> &subst) const {
        if (!t) return "long long";
        if (t->pack_index >= 0) {
            const auto it = subst.find(t->name + "#" + std::to_string(t->pack_index));
            return it == subst.end() ? std::string("int64_t") : it->second;
        }
        if (t->kind == TypeAnnotation::Named && subst.contains(t->name)) {
            std::string r = subst.at(t->name);
            if (t->is_ref && r.find('*') == std::string::npos) r += "*";
            return r;
        }
        if (t->kind == TypeAnnotation::Pointer)
            return subst_type_annotation(t->inner_type.get(), subst) + "*";
        if (t->kind == TypeAnnotation::Array)
            return subst_type_annotation(t->inner_type.get(), subst) + "*";
        if (t->kind == TypeAnnotation::Optional) {
            std::string inner = subst_type_annotation(t->inner_type.get(), subst);
            if (!inner.empty() && inner.back() == '*') return inner;
            return inner + "*";
        }
        if (t->kind == TypeAnnotation::FunctionType) {
            std::string ret = subst_type_annotation(t->inner_type.get(), subst);
            if (ret.empty()) ret = "void";
            std::string sig = ret + " (*)(";
            if (t->generic_args.empty()) {
                sig += "void";
            } else {
                for (size_t i = 0; i < t->generic_args.size(); ++i) {
                    if (i) sig += ", ";
                    std::string pt = subst_type_annotation(t->generic_args[i].get(), subst);
                    sig += pt.empty() ? std::string("void*") : pt;
                }
            }
            sig += ")";
            return sig;
        }
        return c_type(t);
    }

    // ==========================================================================
    // 类型参数分派：`sinter_gen_clone(x)` → `sinter_gen_clone_str(x)`
    // ==========================================================================
    // 泛型在语义分析之前单态化，所以类 `List<T>` 实例化出的 `List_str` 里的方法体
    // 还带着「按 T 分派」的裸调用（`sinter_gen_clone` / `sinter_gen_grow` …）。
    // 这些名字不是真实函数，必须在代码生成时按当前实例的类型实参归约：

    //   1) 类作用域：从 current_class_（实例名，如 `List_str` / `Map_str_i64`）
    //      回溯到模板名，把尾部后缀逐段还原成类型实参。
    //      若实例在别的模块里（`other.List_str`），只按后缀长度匹配并校验类型参数个数。
    //   2) 函数作用域：从当前函数自身的显式类型实参（`<i64>` / `<str>`）取第一个。

    // 只对白名单前缀生效，避免误改用户函数名。
    namespace {
        bool is_dispatch_family(const std::string &n, const std::string &family) {
            if (n == family) return true;
            return n.size() > family.size() && n.compare(0, family.size(), family) == 0 &&
                   n[family.size()] == '_';
        }

        // 后缀片段 -> C 类型
        std::string token_to_c_type(const std::string &tok) {
            static const std::unordered_map<std::string, std::string> kTok = {
                {"i64", "int64_t"}, {"i32", "int32_t"}, {"i16", "int16_t"}, {"i8", "int8_t"},
                {"u64", "uint64_t"}, {"u32", "uint32_t"}, {"u16", "uint16_t"}, {"u8", "uint8_t"},
                {"f64", "double"}, {"f32", "float"},
                {"str", "sinter_string"}, {"cstr", "const char*"},
                {"bool", "uint8_t"}, {"void", "void"},
            };
            const auto it = kTok.find(tok);
            if (it != kTok.end()) return it->second;
            return "struct " + tok + "*";
        }

        // `str_i64_ptr` -> {"str", "i64_ptr"}；`str` -> {"str"}
        std::vector<std::string> split_suffix(const std::string &s) {
            std::vector<std::string> out;
            std::string cur;
            for (char ch: s) {
                if (ch == '_' && !cur.empty()) {
                    // `str_ptr` 里的 `_ptr` 是同一段的修饰，不切开
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        }
    } // namespace

    // 分派族前缀白名单
    bool Codegen::is_generic_dispatch_call(const std::string &name) {
        // 元素动作族（容器用）+ 数组动作族（`Array<T>` 用）。
        // 数组族和元素动作族一样，都是在单态化后的类体里以裸名字出现，
        // 按实例尾部的类型实参归约成 `_i64` / `_f64` / `_str` / `_u8`。
        static const std::vector<std::string> kFamilies = {
            "sinter_gen_clone", "sinter_gen_free", "sinter_gen_drop", "sinter_gen_grow",
            "sinter_gen_hash", "sinter_gen_cmp", "sinter_gen_blank", "sinter_gen_text",
            // 数组动作
            "sinter_gen_alloc", "sinter_gen_fill", "sinter_gen_sum", "sinter_gen_index_of",
            "sinter_gen_reverse", "sinter_gen_sort", "sinter_gen_copy",
        };
        for (auto &f: kFamilies)
            if (is_dispatch_family(name, f)) return true;
        return false;
    }

    std::string Codegen::generic_dispatch_name_at(const std::string &generic, size_t idx) {
        if (!is_generic_dispatch_call(generic)) return {};

        // 两个来源：
        //   1) 泛型类实例的类型实参（`Map_str_i64` → {"str","i64"}）
        //   2) 显式调用里的类型标签（`grow(vals, n, i64)` 末尾的 i64 标记）
        std::vector<std::string> type_tokens;
        if (!tag_token_.empty()) type_tokens.push_back(tag_token_);

        if (type_tokens.empty() && generic_table_ && !current_class_.empty()) {
            if (!generic_table_->split(current_class_, type_tokens)) type_tokens.clear();
        }
        // 类作用域（回退）：实例名尾部后缀（`List_str` / `Map_str_i64`）即类型实参
        if (type_tokens.empty() && !current_class_.empty() && !cur_type_params_.empty()) {
            std::string inst = current_class_;
            const auto dot = inst.rfind('.');
            if (dot != std::string::npos) inst = inst.substr(dot + 1);
            const size_t nparams = cur_type_params_.size();
            auto parts = split_suffix(inst);
            if (parts.size() > nparams)
                type_tokens.assign(parts.end() - static_cast<long>(nparams), parts.end());
        }
        // 函数级显式类型实参：`fun kind<i64>()` → i64
        if (type_tokens.empty() && !cur_type_args_.empty()) type_tokens = cur_type_args_;
        if (idx >= type_tokens.size()) return {};

        // grow 的第一个实参是 *T，后缀会带 `_ptr`，去掉修饰后才是元素类型
        std::string raw = type_tokens[idx];
        while (raw.size() > 4 && raw.compare(raw.size() - 4, 4, "_ptr") == 0)
            raw = raw.substr(0, raw.size() - 4);
        // 已是 C 类型（`sinter_string` / `int64_t`）时反查 token
        if (raw.find(' ') != std::string::npos || raw.find('*') != std::string::npos) {
            static const std::unordered_map<std::string, std::string> kRev = {
                {"int64_t", "i64"}, {"int32_t", "i32"}, {"int16_t", "i16"}, {"int8_t", "i8"},
                {"uint64_t", "u64"}, {"uint32_t", "u32"}, {"uint16_t", "u16"}, {"uint8_t", "u8"},
                {"double", "f64"}, {"float", "f32"},
                {"sinter_string", "str"}, {"const char*", "cstr"},
            };
            const auto it = kRev.find(raw);
            if (it == kRev.end()) return {};
            raw = it->second;
        }
        if (raw.empty() || raw == "void") return {};
        // User class type argument: use its module-qualified C token, matching
        // the dispatch helpers emitted in that class's header.
        if (const auto cit = sem_.classes().find(raw); cit != sem_.classes().end()) {
            raw = cit->second.c_name + "_ptr";
        } else if (const auto xit = sem_.external_classes().find(raw);
                   xit != sem_.external_classes().end()) {
            std::string cn = xit->second.c_name;
            if (cn.rfind("struct ", 0) == 0) cn = cn.substr(7);
            while (!cn.empty() && cn.back() == '*') cn.pop_back();
            raw = cn + "_ptr";
        }

        // 已经带后缀的（`sinter_gen_clone_i64`）原样保留
        static const std::vector<std::string> kSuffixes = {
            "_i64", "_i32", "_i16", "_i8", "_u64", "_u32", "_u16", "_u8",
            "_f64", "_f32", "_str", "_cstr", "_bool",
        };
        for (auto &s: kSuffixes)
            if (generic.size() > s.size() &&
                generic.compare(generic.size() - s.size(), s.size(), s) == 0)
                return generic;
        return generic + "_" + raw;
    }

    std::string Codegen::generic_dispatch_name(const std::string &generic) {
        return generic_dispatch_name_at(generic, 0);
    }

    // ==========================================================================
    // 泛型单态化：类型实参 -> C 名字片段
    // ==========================================================================
    std::string Codegen::generic_type_token(const std::string &raw) {
        std::string t = raw;
        while (t.rfind("const ", 0) == 0) t = t.substr(6);
        if (t.rfind("struct ", 0) == 0) t = t.substr(7);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty() && t.back() == '*') {
            return generic_type_token(t.substr(0, t.size() - 1)) + "_ptr";
        }
        static const std::unordered_map<std::string, std::string> kShort = {
            {"int64_t", "i64"}, {"int32_t", "i32"}, {"int16_t", "i16"}, {"int8_t", "i8"},
            {"uint64_t", "u64"}, {"uint32_t", "u32"}, {"uint16_t", "u16"}, {"uint8_t", "u8"},
            {"double", "f64"}, {"float", "f32"}, {"sinter_string", "str"},
            {"const char*", "cstr"}, {"long long", "i64"}, {"void", "void"},
        };
        if (auto it = kShort.find(t); it != kShort.end()) return it->second;
        // 已是 Sinter 短名或类/枚举名：统一 snake_case，并把非字母数字压成下划线
        std::string out;
        for (char ch: Semantic::to_snake_ident(t))
            out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        return out.empty() ? std::string("void") : out;
    }

    std::unordered_map<std::string, std::string>
    Codegen::generic_subst_for_call(const FunctionDecl *decl, CallExpr *c,
                                    const std::function<std::string(Expression *)> &arg_type) {
        std::unordered_map<std::string, std::string> subst;
        if (!decl || !c) return subst;
        auto type_of_arg = [&](Expression *e) -> std::string {
            if (arg_type) return arg_type(e);
            return c_type_of_expr(e);
        };
        // 1) 显式类型实参 f<i64>(...)
        //    遇到类型参数包时，把从该位置起剩余的类型实参依次绑定成 `Args#0`…，
        //    并把个数记作 `Args#count`（这是 `sizeof...` 的替代品）。
        for (size_t ti = 0, ai = 0; ti < decl->tparams.size(); ++ti) {
            auto &tp = decl->tparams[ti];
            if (tp.is_pack) {
                size_t n = 0;
                for (size_t k = ti; ai + n < c->type_args.size(); ++n)
                    subst[tp.name + "#" + std::to_string(n)] = c_type(c->type_args[ai + n].get());
                subst[tp.name + "#count"] = std::to_string(n);
                ai += n;
                continue;
            }
            if (ai < c->type_args.size()) subst[tp.name] = c_type(c->type_args[ai].get());
            ++ai;
        }
        // 2) 从实参推断：形参类型正好是裸的类型参数时
        for (size_t pi = 0; pi < decl->params.size() && pi < c->args.size(); ++pi) {
            auto &formal = decl->params[pi];
            if (!formal.type || formal.type->kind != TypeAnnotation::Named) continue;
            const std::string &tn = formal.type->name;
            // 变长形参 `xs: Args...`：把剩余实参的类型全部绑到包里
            bool is_pack = false;
            for (auto &tp: decl->tparams)
                if (tp.is_pack && tp.name == tn) is_pack = true;
            if (is_pack) {
                if (subst.contains(tn + "#0")) continue;   // 显式实参优先
                size_t n = 0;
                for (size_t ai = pi; ai < c->args.size(); ++ai, ++n) {
                    const std::string at = type_of_arg(c->args[ai].value.get());
                    subst[tn + "#" + std::to_string(n)] = at.empty() ? "int64_t" : at;
                }
                subst[tn + "#count"] = std::to_string(n);
                continue;
            }
            bool is_t = false;
            for (auto &tp: decl->tparams) if (!tp.is_pack && tp.name == tn) is_t = true;
            if (!is_t || subst.contains(tn)) continue;
            const std::string at = type_of_arg(c->args[pi].value.get());
            if (!at.empty()) subst[tn] = at;
        }
        // 3) 仍未知的按 int64_t 兜底（与 collect_generic_instantiations 一致）
        for (auto &tp: decl->tparams) {
            if (tp.is_pack) {
                if (!subst.contains(tp.name + "#count")) subst[tp.name + "#count"] = "0";
                continue;
            }
            if (!subst.contains(tp.name)) subst[tp.name] = "int64_t";
        }
        return subst;
    }

    std::string Codegen::generic_suffix(const std::vector<std::string> &tparam_names,
                                       const std::unordered_map<std::string, std::string> &subst) {
        std::string suffix;
        for (auto &tn: tparam_names) {
            const auto it = subst.find(tn);
            suffix += "_" + generic_type_token(it == subst.end() ? std::string("int64_t") : it->second);
        }
        return suffix;
    }

    std::string Codegen::generic_pack_suffix(const std::vector<TemplateParam> &tparams,
                                             const std::unordered_map<std::string, std::string> &subst) {
        std::string suffix;
        for (auto &tp: tparams) {
            if (tp.is_pack) {
                // 包里每个类型各贡献一段，`log<i64, str>` → `_i64_str`
                const auto it = subst.find(tp.name + "#count");
                const size_t n = it == subst.end() ? 0 : static_cast<size_t>(std::stoul(it->second));
                for (size_t i = 0; i < n; ++i) {
                    const auto a = subst.find(tp.name + "#" + std::to_string(i));
                    suffix += "_" + generic_type_token(a == subst.end() ? std::string("int64_t") : a->second);
                }
                continue;
            }
            const auto it = subst.find(tp.name);
            suffix += "_" + generic_type_token(it == subst.end() ? std::string("int64_t") : it->second);
        }
        return suffix;
    }

    std::string Codegen::generic_fn_c_name(const FunctionInfo &fi,
                                          const std::unordered_map<std::string, std::string> &subst) {
        std::string name = sem_.module_to_ident(fi.module_name);
        if (fi.is_method && !fi.owner_class.empty())
            name += "_" + Semantic::to_snake_ident(fi.owner_class);
        std::string base;
        for (char ch: Semantic::to_snake_ident(fi.name))
            base += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        name += "_" + base;
        // 优先用 AST 里的完整参数表（含 `Args...` 包标记）；没有 decl 时退回名字列表
        if (fi.decl && !fi.decl->tparams.empty())
            return name + generic_pack_suffix(fi.decl->tparams, subst);
        return name + generic_suffix(fi.template_params, subst);
    }

    std::string Codegen::generic_call_return(const FunctionInfo *fi, CallExpr *c) {
        if (!fi || fi->template_params.empty() || !fi->decl || !fi->decl->return_type) return "";
        const auto subst = generic_subst_for_call(fi->decl, c);
        const TypeAnnotation *rt = fi->decl->return_type.get();
        if (rt->kind == TypeAnnotation::Named && subst.contains(rt->name)) {
            const auto it = subst.find(rt->name);
            if (it != subst.end() && !it->second.empty()) return it->second;
        }
        return "";
    }

    const FunctionInfo *Codegen::fi_of_decl(const FunctionDecl *decl) {
        if (!decl) return nullptr;
        auto it = sem_.functions().find(decl->name);
        if (it == sem_.functions().end()) return nullptr;
        for (auto &fi: it->second)
            if (fi.decl == decl) return &fi;
        return nullptr;
    }

    const ClassInfo &Codegen::class_by_name(const std::string &n) const {
        static ClassInfo empty;
        auto it = sem_.classes().find(n);
        return it == sem_.classes().end() ? empty : it->second;
    }

    std::string Codegen::class_c_name(const std::string &sinter_name) const {
        auto it = sem_.classes().find(sinter_name);
        if (it != sem_.classes().end()) return it->second.c_name;
        return module_ident_ + "_" + Semantic::to_snake_ident(sinter_name);
    }

    std::string Codegen::str_cast(const std::string &code, const std::string &type_c) {
        // 目标：内部字符串 sinter_string
        if (type_c == "sinter_string") return code;                       // 已是内部字符串
        // 结构体 / 类：走编译器为每个类型生成的 `_to_str`（反射式字符串化）。
        // 这样 `${obj}`、`"x=" + obj`、`println(obj)` 都自动拿到 `Type(field=...)`。
        if (type_c.rfind("struct ", 0) == 0) {
            std::string cn = type_c.substr(7);
            while (!cn.empty() && cn.back() == '*') cn.pop_back();
            for (const auto &kv: sem_.classes()) {
                if (kv.second.c_name == cn) return cn + "_to_str(" + code + ")";
            }
        }
        if (type_c == "const char*" || type_c == "char*")
            return "sinter_string_from_cstr(" + code + ")";
        if (type_c == "double" || type_c == "float")
            return "sinter_string_from_num((double)(" + code + "))";
        if (type_c == "uint8_t" && false) return code;
        if (type_c == "long long" || type_c == "int32_t" || type_c == "int16_t" ||
            type_c == "int8_t" || type_c == "uint8_t" || type_c == "uint16_t" ||
            type_c == "uint32_t" || type_c == "int64_t" || type_c == "uint64_t")
            return "sinter_string_from_int((int64_t)(" + code + "))";
        return "sinter_string_from_int((int64_t)(" + code + "))";
    }

    // 把任意表达式转成 C 字符串（const char*），用于调用 C API
    std::string Codegen::cstr_cast(const std::string &code, const std::string &type_c) {
        if (type_c == "sinter_string") return "sinter_string_to_cstr(" + code + ")";
        return code;
    }

    std::string Codegen::module_path_of(const Expression *e) const {
        if (auto *id = dynamic_cast<const IdentifierExpr *>(e)) {
            if (id->name == "this" || locals_.contains(id->name) ||
                sem_.classes().contains(id->name) || sem_.external_classes().contains(id->name))
                return "";
            return id->name;
        }
        if (auto *m = dynamic_cast<const MemberAccessExpr *>(e)) {
            const std::string left = module_path_of(m->object.get());
            if (left.empty()) return "";
            return left + "." + m->member;
        }
        return "";
    }

    std::string Codegen::c_type_of_expr(Expression *e) {
        if (!e) return "long long";
        if (auto *l = dynamic_cast<LiteralExpr *>(e)) {
            switch (l->lit) {
                case LiteralExpr::Int: {
                    if (l->suffix == "i8") return "int8_t";
                    if (l->suffix == "i16") return "int16_t";
                    if (l->suffix == "i32") return "int32_t";
                    if (l->suffix == "i64") return "int64_t";
                    if (l->suffix == "u8") return "uint8_t";
                    if (l->suffix == "u64") return "uint64_t";
                    // 无后缀整数字面量是 `i64`（与生成的 `3LL` 一致）。
                    // 曾经在这里返回 `int32_t`，而 codegen 写的是 `3LL` —— 类型与值
                    // 对不上，泛型参数推断出 `T = i32`，`println(maxOf2(3, 9))` 就
                    // 找不到 `println(i32)` 重载，最后落到 bool 重载输出 `true`。
                    return "int64_t";
                }
                case LiteralExpr::Float:
                    if (l->suffix == "f32") return "float";
                    return "double";
                case LiteralExpr::String: return "sinter_string";
                case LiteralExpr::Char: return "char";
                case LiteralExpr::Bool: return "uint8_t";
                case LiteralExpr::Null: return "void*";
            }
        }
        if (auto *id = dynamic_cast<IdentifierExpr *>(e)) {
            if (const auto it = local_types_.find(id->name); it != local_types_.end()) {
                if (ref_vars_.count(id->name) && !ref_class_vars_.count(id->name)) {
                    std::string t = it->second;
                    while (!t.empty() && t.back() == '*') t.pop_back();
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    if (!t.empty()) return t;
                }
                return it->second;
            }
            if (id->name == "this") return "struct " + current_class_c_ + "*";
        }
        // lambda / 作用域函数块：裸字段名的类型
        if (auto *id2 = dynamic_cast<IdentifierExpr *>(e);
            id2 && !local_types_.count(id2->name) && !lambda_receiver_.empty() &&
            locals_.contains(lambda_receiver_)) {
            const std::string rt2 = local_types_.count(lambda_receiver_)
                                        ? local_types_[lambda_receiver_]
                                        : std::string();
            if (!rt2.empty() && rt2.back() == '*') {
                std::string cur2;
                for (auto &[cn, ci]: sem_.classes())
                    if ("struct " + ci.c_name + "*" == rt2) { cur2 = cn; break; }
                if (cur2.empty())
                    for (auto &[cn, ci]: sem_.external_classes())
                        if ("struct " + ci.c_name + "*" == rt2) { cur2 = cn; break; }
                int guard2 = 0;
                while (!cur2.empty() && guard2++ < 32) {
                    const ClassInfo *cc2 = sem_.find_class(cur2);
                    if (!cc2) break;
                    for (auto &f: cc2->fields)
                        if (f.name == id2->name) return f.type_c;
                    cur2 = cc2->base_class;
                }
            }
        }
        // lambda 接收者上的裸方法名：返回类型按继承链查找
        if (auto *c0 = dynamic_cast<CallExpr *>(e);
            c0 && !lambda_receiver_.empty() &&
            dynamic_cast<IdentifierExpr *>(c0->callee.get()) &&
            locals_.contains(lambda_receiver_)) {
            const std::string mname = dynamic_cast<IdentifierExpr *>(c0->callee.get())->name;
            const std::string rtype = local_types_.count(lambda_receiver_)
                                          ? local_types_[lambda_receiver_]
                                          : std::string();
            if (!rtype.empty() && rtype.back() == '*') {
                if (auto *fit0 = (sem_.functions().count(mname) ? &sem_.functions().at(mname)
                                                                : nullptr)) {
                    for (auto &f: *fit0) {
                        if (!f.is_method) continue;
                        const ClassInfo *fc0 = sem_.find_class(f.owner_class);
                        if (!fc0) continue;
                        if (is_subclass_of_type(rtype, "struct " + fc0->c_name + "*"))
                            return f.return_type_name;
                    }
                }
            }
        }
        if (auto *c = dynamic_cast<CallExpr *>(e)) {
            // 类型参数分派族（`sinter_gen_text(v)` → `sinter_gen_text_i64(v)`）。
            // 这些名字在归约前不是真实函数，查不到声明，返回类型只能按族前缀
            // 直接给出 —— 否则 `"x=" + sinter_gen_text(v)` 会被当成整数拼进
            // `sinter_string_from_int((int64_t)(...))`，对 sinter_string 直接报错。
            if (auto *qid = dynamic_cast<IdentifierExpr *>(c->callee.get());
                qid && is_generic_dispatch_call(qid->name)) {
                // `sinter_gen_text_sb_item_*` 是文本构建器的「追加一段」助手，
                // 返回 void —— 它名字前缀虽然落在 `sinter_gen_text` 族里，
                // 但不能当成「转成字符串」处理。
                if (qid->name.find("_sb_item_") != std::string::npos) return "void";
                static const std::vector<std::string> kTextFams = {
                    "sinter_gen_text", "sinter_gen_join",
                };
                for (auto &f: kTextFams)
                    if (is_dispatch_family(qid->name, f)) return "sinter_string";
                // 指针类：alloc / copy 的返回类型随实例变化，这里不猜，
                // 让调用点自己带声明（`val p: *T = ...`）。
            }
            // 模块限定调用：std.str.len(s)（支持多级模块路径）
            if (auto *ma = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
                const std::string mp = module_path_of(ma->object.get());
                if (!mp.empty()) {
                    std::vector<CType> ats;
                    for (auto &arg: c->args) ats.push_back(arg_type_of(arg.value.get()));
                    if (const FunctionInfo *qfi = sem_.resolve_overload(
                            ma->member, ats, nullptr, sem_.module_to_ident(mp))) {
                        const std::string g = generic_call_return(qfi, c);
                        return g.empty() ? qfi->return_type_name : g;
                    }
                }
            }
            // 编译器生成方法：obj.copy() / obj.move() 返回对象自身类型的指针
            if (auto *m0 = dynamic_cast<MemberAccessExpr *>(c->callee.get());
                m0 && (m0->member == "copy" || m0->member == "move")) {
                const std::string ot = c_type_of_expr(m0->object.get());
                if (!ot.empty() && ot.back() == '*') return ot;
            }
            // 作用域函数：apply/also 返回接收者本身
            if (auto *m0 = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
                if (m0->member == "apply" || m0->member == "also")
                    return c_type_of_expr(m0->object.get());
                // let/run：返回 lambda 体的类型（在接收者类型上下文中推断）
                if ((m0->member == "let" || m0->member == "run") && !c->args.empty()) {
                    if (auto *lam0 = dynamic_cast<LambdaExpr *>(c->args[0].value.get())) {
                        const std::string rt = c_type_of_expr(m0->object.get());
                        if (!lam0->is_block) {
                            if (auto *body0 = std::get_if<ExprPtr>(&lam0->body)) {
                                auto saved_l = locals_;
                                auto saved_t = local_types_;
                                const std::string saved_recv = lambda_receiver_;
                                // 形参：优先 lambda 自身标注，否则用接收者类型
                                for (size_t i = 0; i < lam0->params.size(); ++i) {
                                    std::string pt = lam0->params[i].type
                                                         ? c_type(lam0->params[i].type.get())
                                                         : rt;
                                    if (!pt.empty())
                                        local_types_[lam0->params[i].name] = pt;
                                    else
                                        local_types_.erase(lam0->params[i].name);
                                }
                                // lambda 里裸方法名要解析到接收者：以首形参名义登记
                                std::string recv_name;
                                if (lam0->params.empty())
                                    recv_name = std::string();
                                else if (lam0->params.size() == 1 &&
                                         !local_types_.count(lam0->params[0].name))
                                    recv_name = std::string();
                                else
                                    recv_name = lam0->params[0].name;
                                if (!recv_name.empty()) {
                                    lambda_receiver_ = recv_name;
                                    locals_[recv_name] = "<<recv>>";
                                    local_types_[recv_name] = rt;
                                }
                                std::string bt = c_type_of_expr(body0->get());
                                lambda_receiver_ = saved_recv;
                                locals_ = saved_l;
                                local_types_ = saved_t;
                                return bt;
                            }
                        }
                    }
                }
            }
            // 局部函数变量优先：参数 `f: (i64) -> void` 不能解析到同名的全局函数。
            if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get())) {
                auto lt = local_types_.find(id->name);
                if (lt != local_types_.end()) {
                    const auto pos = lt->second.find(" (*");
                    if (pos != std::string::npos) return lt->second.substr(0, pos);
                }
            }
            // 类构造
            if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get())) {
                if (sem_.classes().contains(id->name)) return "struct " + class_c_name(id->name) + "*";
                if (const auto fit = sem_.functions().find(id->name);
                    fit != sem_.functions().end() && !fit->second.empty()) {
                    // 有实参时按重载解析挑真正命中的那个分支，别拿首个分支的返回类型
                    // （`fun join(cstr, i64...): i64` 与同名 `join: str` 并存时曾误判）。
                    if (!c->args.empty()) {
                        std::vector<CType> ats;
                        for (auto &arg: c->args) ats.push_back(arg_type_of(arg.value.get()));
                        std::string oerr;
                        if (const FunctionInfo *qfi = sem_.resolve_overload(id->name, ats, &oerr)) {
                            const std::string g = generic_call_return(qfi, c);
                            if (!g.empty()) return g;
                            return qfi->return_type_name;
                        }
                    }
                    for (auto &f: fit->second) {
                        const std::string g = generic_call_return(&f, c);
                        if (!g.empty()) return g;
                    }
                    return fit->second.front().return_type_name;
                }
            }
            // 静态方法调用：Class.staticFun(...)
            if (auto *m = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
                if (auto *oid = dynamic_cast<IdentifierExpr *>(m->object.get());
                    oid && sem_.classes().contains(oid->name) && !locals_.contains(oid->name)) {
                    for (auto &[fname, bucket] : sem_.functions()) {
                        for (auto &f : bucket) {
                            if (f.is_static && f.name == m->member &&
                                f.owner_class == oid->name) {
                                const std::string g = generic_call_return(&f, c);
                                return g.empty() ? f.return_type_name : g;
                            }
                        }
                    }
                }
            }
            // 方法调用（含跨模块/继承链上的方法）：返回类型按 owner 可达链解析
            if (auto *m = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
                const std::string owner = c_type_of_expr(m->object.get());
                for (auto &[fname, bucket]: sem_.functions()) {
                    for (auto &f: bucket) {
                        if (!f.is_method || f.name != m->member) continue;
                        if (f.owner_class.empty()) continue;
                        const ClassInfo *fc = sem_.find_class(f.owner_class);
                        if (!fc) continue;
                        if ("struct " + fc->c_name + "*" != owner) continue;
                        const std::string g = generic_call_return(&f, c);
                        return g.empty() ? f.return_type_name : g;
                    }
                }
                // 扩展函数：`"x".uppercase()` / `s.trim()` 的返回类型写在 ext 声明上。
                // 没有这一支，调用会被当成 i64（`str_cast` 于是生成
                // sinter_string_from_int(<结构体>)，C 阶段报
                // "aggregate value used where an integer was expected"）。
                {
                    const std::string recv_c = c_type_of_expr(m->object.get());
                    for (auto &[fname, bucket]: sem_.functions()) {
                        for (auto &f: bucket) {
                            if (f.name != m->member || f.ext_receiver.empty()) continue;
                            // 一律用 CType::to_c() 取 C 名：`make_str()` 只设了
                            // `sinter_name = "str"`，`name` 是空的（Str 的 C 名由
                            // to_c() 硬编码成 sinter_string），拿 `.name` 比较/返回
                            // 会得到空串，等于没匹配上。
                            if (sem_.resolve_type_name(f.ext_receiver).to_c() != recv_c) continue;
                            if (f.return_type_name.empty()) return std::string();
                            return sem_.resolve_type_name(f.return_type_name).to_c();
                        }
                    }
                }
                // 未找到时按继承链向上找
                std::string cur;
                for (auto &[cn, xci]: sem_.external_classes())
                    if ("struct " + xci.c_name + "*" == owner) { cur = cn; break; }
                if (cur.empty())
                    for (auto &[cn, xci]: sem_.classes())
                        if ("struct " + xci.c_name + "*" == owner) { cur = cn; break; }
                int guard = 0;
                while (!cur.empty() && guard++ < 32) {
                    for (auto &[fname, bucket]: sem_.functions()) {
                        for (auto &f: bucket) {
                            if (f.is_method && f.name == m->member && f.owner_class == cur) {
                                const std::string g = generic_call_return(&f, c);
                                return g.empty() ? f.return_type_name : g;
                            }
                        }
                    }
                    const ClassInfo *cc = sem_.find_class(cur);
                    if (!cc) break;
                    cur = cc->base_class;
                }
            }
        }
        // 一元运算：`*p` 的类型是「去掉一层 `*`」，`&x` 的类型是「加一层 `*`」。
        // 缺这条会让 `println(*p)` 走 `long long` 兜底（曾被当成 bool 输出 true）。
        if (auto *n = dynamic_cast<NonNullExpr *>(e)) return c_type_of_expr(n->operand.get());
        if (auto *u = dynamic_cast<UnaryExpr *>(e)) {
            if (u->op == TokenType::Star) {
                std::string t = c_type_of_expr(u->operand.get());
                while (!t.empty() && t.back() == ' ') t.pop_back();
                if (!t.empty() && t.back() == '*') {
                    t.pop_back();
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    if (!t.empty()) return t;
                }
                return std::string();
            }
            if (u->op == TokenType::Ampersand) {
                const std::string t = c_type_of_expr(u->operand.get());
                return t.empty() ? std::string() : t + "*";
            }
            if (u->op == TokenType::Bang) return "uint8_t";
            // `-x` / `+x` / `~x`：类型保持
            return c_type_of_expr(u->operand.get());
        }
        // 下标访问：`data[i]` 的类型 = 被索引对象的元素类型（指针去掉一层 `*`）。
        // 没有这条，`self.data[i]` 会退回默认的 long long，
        // 把 `*str` 列表的元素当成整数解释（曾导致 segfault）。
        if (auto *ix = dynamic_cast<IndexAccessExpr *>(e)) {
            std::string base = c_type_of_expr(ix->object.get());
            while (!base.empty() && base.back() == ' ') base.pop_back();
            if (!base.empty() && base.back() == '*') {
                base.pop_back();
                while (!base.empty() && base.back() == ' ') base.pop_back();
                if (!base.empty()) return base;
            }
            return "long long";
        }
        if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) {
            // 编译器生成方法：obj.copy() / obj.move() 返回对象自身类型的指针
            if (m->member == "copy" || m->member == "move") {
                if (auto *mc = dynamic_cast<CallExpr *>(e); mc) {
                    const std::string ot = c_type_of_expr(m->object.get());
                    if (!ot.empty() && ot.back() == '*') return ot;
                }
            }
            // 静态字段：Class.field
            if (auto *oid = dynamic_cast<IdentifierExpr *>(m->object.get());
                oid && sem_.classes().contains(oid->name) && !locals_.contains(oid->name)) {
                const auto key = oid->name + "." + m->member;
                if (const auto sit = sem_.static_fields().find(key); sit != sem_.static_fields().end())
                    return sit->second.type_c;
            }
            // 字段（沿继承链查找）
            {
                const std::string owner = c_type_of_expr(m->object.get());
                std::string cur;
                for (auto &[cn2, ci2]: sem_.classes())
                    if ("struct " + ci2.c_name + "*" == owner) { cur = cn2; break; }
                if (cur.empty())
                    for (auto &[cn2, ci2]: sem_.external_classes())
                        if ("struct " + ci2.c_name + "*" == owner) { cur = cn2; break; }
                int guard = 0;
                while (!cur.empty() && guard++ < 32) {
                    const ClassInfo *cc = sem_.find_class(cur);
                    if (!cc) break;
                    for (auto &f: cc->fields) if (f.name == m->member) return f.type_c;
                    cur = cc->base_class;
                }
            }
        }
        if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
            const auto lt = c_type_of_expr(b->left.get());
            const auto rt = c_type_of_expr(b->right.get());
            const auto is_str_t = [](const std::string &t) {
                return t == "sinter_string" || t == "const char*" || t == "char*";
            };
            const bool lstr = is_str_t(lt);
            const bool rstr = is_str_t(rt);
            // 运算符重载：结果类型取重载函数的返回类型
            const std::string opfn = find_binary_operator(b->op, lt, rt);
            if (!opfn.empty()) {
                for (auto &[oname, bucket]: sem_.functions()) {
                    for (auto &f: bucket) {
                        if (f.c_name == opfn) return f.return_type_name;
                    }
                }
            }
            if (b->op == TokenType::Plus && (lstr || rstr)) return "sinter_string";
            if (b->op == TokenType::EqualEqual || b->op == TokenType::NotEqual ||
                b->op == TokenType::Less || b->op == TokenType::LessEqual ||
                b->op == TokenType::Greater || b->op == TokenType::GreaterEqual ||
                b->op == TokenType::DoubleAmpersand || b->op == TokenType::DoublePipe)
                return "uint8_t";
            if (lt == "double" || rt == "double") return "double";
            if (lt == "int64_t" || rt == "int64_t") return "int64_t";
            return lt;
        }
        if (auto *t = dynamic_cast<TernaryExpr *>(e)) {
            const std::string lt = c_type_of_expr(t->then_b.get());
            const std::string rt = c_type_of_expr(t->else_b.get());
            // Elvis（`a ?: b`）的结果不是指针：`str?` 那一侧会被解引用，
            // 所以结果类型取「非指针」的一侧。缺这条时 `val shown = name ?: x`
            // 会被声明成 `sinter_string*`，后续拼接把它当整数（或报类型不符）。
            if (t->is_elvis) {
                if (!rt.empty() && rt.back() != '*') return rt;
                if (!lt.empty() && lt.back() == '*') {
                    std::string v = lt;
                    v.pop_back();
                    while (!v.empty() && v.back() == ' ') v.pop_back();
                    return v;
                }
                return lt.empty() ? rt : lt;
            }
            if (lt == rt) return lt;
            if (is_subclass_of_type(lt, rt)) return rt;   // 子类 → 基类
            if (is_subclass_of_type(rt, lt)) return lt;   // 基类 ← 子类
            return lt;
        }
        if (auto *sc0 = dynamic_cast<ScopeCallExpr *>(e)) {
            const std::string rt = c_type_of_expr(sc0->receiver.get());
            if (sc0->kind_sc == ScopeCallExpr::Apply || sc0->kind_sc == ScopeCallExpr::Also)
                return rt;
            // let/run/with：接收者类型上下文中推断表达式体
            Expression *bodye = sc0->body_expr.get();
            if (!bodye)
                if (auto *blk0 = dynamic_cast<BlockStmt *>(sc0->body.get()); blk0 && !blk0->statements.empty())
                    if (auto *ex0 = dynamic_cast<ExprStmt *>(blk0->statements.back().get()))
                        bodye = ex0->expr.get();
            if (bodye) {
                auto saved_l = locals_;
                auto saved_t = local_types_;
                const std::string saved_recv = lambda_receiver_;
                const std::string bind = !sc0->it_name.empty() ? sc0->it_name : std::string("__it");
                locals_[bind] = "<<recv>>";
                local_types_[bind] = rt;
                locals_["this"] = "<<recv>>";
                local_types_["this"] = rt;
                locals_["self"] = "<<recv>>";
                local_types_["self"] = rt;
                lambda_receiver_ = bind;
                const std::string bt = c_type_of_expr(bodye);
                lambda_receiver_ = saved_recv;
                locals_ = saved_l;
                local_types_ = saved_t;
                return bt;
            }
            return rt;
        }
        if (auto *i = dynamic_cast<IfExpr *>(e)) {
            // `val x = if (c) { "a" } else { "b" }` 必须推断成 sinter_string。
            // 以前直接取 then 分支，且这里用的是 if/else 两条路径的简单返回 ——
            // 一旦 then 是字面量、else 是别的东西就退回 long long，
            // 字符串被塞进 `long long` 变量里，C 阶段报
            // "incompatible types when initializing type 'long long int' using type 'sinter_string'"。
            const std::string lt = c_type_of_expr(i->then_b.get());
            const std::string rt = c_type_of_expr(i->else_b.get());
            if (lt == rt) return lt;
            if (is_subclass_of_type(lt, rt)) return rt;
            if (is_subclass_of_type(rt, lt)) return lt;
            const auto is_str_t3 = [](const std::string &t) {
                return t == "sinter_string" || t == "const char*" || t == "char*";
            };
            if (is_str_t3(lt) || is_str_t3(rt)) return "sinter_string";
            if (lt == "double" || rt == "double") return "double";
            if (lt == "int64_t" || rt == "int64_t") return "int64_t";
            return lt;
        }
        if (auto *w = dynamic_cast<WhenExpr *>(e)) {
            if (!w->branches.empty()) return c_type_of_expr(w->branches[0].body.get());
            if (w->else_branch) return c_type_of_expr(w->else_branch.get());
        }
        if (auto *g = dynamic_cast<GroupingExpr *>(e)) return c_type_of_expr(g->expr.get());
        // 块表达式 `{ "x" }`：类型就是块中最后一条表达式语句的类型。
        // `val label = if (c) { "a" } else { "b" }` 的两个分支都是 BlockExpr，
        // 没有这一支就退回 long long，字符串被塞进 `long long` 变量里。
        if (auto *be = dynamic_cast<BlockExpr *>(e)) {
            if (auto *blk = dynamic_cast<BlockStmt *>(be->block.get()); blk && !blk->statements.empty()) {
                if (auto *ex = dynamic_cast<ExprStmt *>(blk->statements.back().get()))
                    return c_type_of_expr(ex->expr.get());
            }
            return std::string();
        }
        if (dynamic_cast<StringTemplateExpr *>(e)) return "sinter_string";
        // 管道 `x |> f(a)` 的类型 = 目标函数的返回类型。
        // 没有这一支会退回 `long long`，`println(x |> f(1))` 就会去匹配
        // `println(str)`，生成 `sys_println_str(syn_f(5LL, 1LL))` ——
        // 参数类型不符，gcc 才报错，与用户代码无关。
        if (auto *pp = dynamic_cast<PipelineExpr *>(e)) {
            const std::string *fname = nullptr;
            std::string mname;
            if (auto *cc = dynamic_cast<CallExpr *>(pp->right.get())) {
                if (auto *id = dynamic_cast<IdentifierExpr *>(cc->callee.get()))
                    fname = &id->name;
                else if (auto *m = dynamic_cast<MemberAccessExpr *>(cc->callee.get()))
                    mname = m->member;
            } else if (auto *id = dynamic_cast<IdentifierExpr *>(pp->right.get()))
                fname = &id->name;

            if (fname) {
                auto fit = sem_.functions().find(*fname);
                if (fit != sem_.functions().end() && !fit->second.empty())
                    return fit->second.front().return_type_name;
            }
            if (!mname.empty()) {
                // `x |> obj.m(a)` 形式：按接收者静态类型找方法
                const std::string recv_c = c_type_of_expr(
                    dynamic_cast<MemberAccessExpr *>(
                        dynamic_cast<CallExpr *>(pp->right.get())->callee.get())->object.get());
                for (auto &[cn, ci]: sem_.classes())
                    if ("struct " + ci.c_name + "*" == recv_c) {
                        // 方法与自由函数共用 sem_.functions()，靠 is_method +
                        // owner_class 区分；沿继承链往上找第一个匹配。
                        std::string cur = cn;
                        int guard = 0;
                        while (!cur.empty() && guard++ < 32) {
                            auto fit = sem_.functions().find(mname);
                            if (fit != sem_.functions().end())
                                for (auto &ff: fit->second)
                                    if (ff.is_method && ff.owner_class == cur)
                                        return ff.return_type_name;
                            const ClassInfo *cc = sem_.find_class(cur);
                            if (!cc) break;
                            cur = cc->base_class;
                        }
                    }
            }
            return std::string();
        }
        return "long long";
    }

    void Codegen::flatten_module_blocks(Program &p) {
        std::vector<StmtPtr> flat;
        std::function<void(std::vector<StmtPtr> &, std::vector<StmtPtr> &)> append =
            [&](std::vector<StmtPtr> &from, std::vector<StmtPtr> &to) {
                for (auto &s: from) {
                    if (auto *ms = dynamic_cast<ModuleStmt *>(s.get()); ms && ms->is_block) {
                        append(ms->body, to);
                        continue;
                    }
                    if (auto *pb = dynamic_cast<PubBlockStmt *>(s.get())) {
                        // pub { ... } 是纯导出标记，块内声明直接提升到模块顶层
                        append(pb->statements, to);
                        continue;
                    }
                    to.push_back(std::move(s));
                }
            };
        append(p.statements, flat);
        p.statements = std::move(flat);
    }

    void Codegen::collect_imports(const Program &p, std::vector<std::string> &out) const {
        for (auto &s: p.statements) {
            if (const auto *imp = dynamic_cast<ImportStmt *>(s.get()))
                out.push_back(sem_.module_to_ident(imp->path));
        }
    }

    void Codegen::emit_enum_impl(const EnumDecl *e) {
        auto it = sem_.enums().find(e->name);
        if (it == sem_.enums().end()) return;
        auto &ei = it->second;
        h_types_ << "typedef enum " << ei.c_name << " {\n";
        for (size_t i = 0; i < e->variants.size(); ++i) {
            h_types_ << "    " << ei.c_name << "_" << Semantic::to_snake_ident(e->variants[i]);
            if (i + 1 < e->variants.size()) h_types_ << ",";
            h_types_ << "\n";
        }
        h_types_ << "} " << ei.c_name << ";\n\n";
    }

    void Codegen::emit_class_decl_impl(const ClassDecl *c) {
        auto it = sem_.classes().find(c->name);
        if (it == sem_.classes().end()) return;
        auto &ci = it->second;

        const bool has_vtable = !ci.vtable.empty() || !ci.base_class.empty();

        h_types_ << "typedef struct " << ci.c_name << " {\n";
        if (has_vtable) h_types_ << "    sinter_vfn* __vptr;\n";
        // __parent：指回最外层的完整对象。
        // 基类方法里 `this` 指向内嵌的 __base 子对象，读取自身字段没问题；
        // 但虚调用沿继承链分派时需要一个真正意义上的对象起点，
        // 因此每个内嵌子对象都记录自己的完整对象地址。
        h_types_ << "    void* __parent;\n";
        // 运行时类型标识：`x is T` 需要在运行期判断，仅靠静态类型不够。
        // __type_chain 是「本类 → 基类 …」的类型名表（NULL 结尾）。
        h_types_ << "    const char* const* __type_chain;\n";
        // 基类嵌入（单继承；同模块或跨模块统一按 __base 内嵌）
        const ClassInfo *base_ci = ci.base_class.empty() ? nullptr
                                                        : sem_.find_class(ci.base_class);
        if (base_ci) {
            if (base_ci->module_name != ci.module_name && !base_ci->module_name.empty())
                h_pre_ << "#include \"sinter_" << sem_.module_to_ident(base_ci->module_name) << ".h\"\n";
            h_types_ << "    struct " << base_ci->c_name << " __base;\n";
            // 让子模块也能看到基类的 vtable 槽位布局（只声明，不重复定义）
            for (size_t i = 0; i < base_ci->vtable_slots.size(); ++i)
                h_types_ << "    /* inherited slot " << i << ": "
                         << base_ci->vtable_slots[i] << " */\n";
        }
        for (auto &f: ci.fields) {
            const std::string ft = f.type_c.empty() ? "long long" : f.type_c;
            h_types_ << "    " << Semantic::param_decl(ft, f.c_name) << ";\n";
        }
        h_types_ << "} " << ci.c_name << ";\n\n";

        // 类型链：Class -> Base -> Base.Base ...（用于 `is` 判定）
        {
            std::vector<std::string> chain;
            std::string cur = ci.name;
            int guard = 0;
            while (!cur.empty() && guard++ < 32) {
                chain.push_back(cur);
                const ClassInfo *cc = sem_.find_class(cur);
                if (!cc) break;
                cur = cc->base_class;
            }
            // 头文件可能被多个翻译单元包含：一律用 static const，避免重复定义。
            // _type_name 先定义、并被 _type_chain[0] 引用，避免「未被使用的
            // static const 变量」提示；两者再挂 sinter_maybe_unused。属性必须写在
            // 声明符之后 —— 写在前面对数组不生效（-Wunused-const-variable 仍会报）。
            h_types_ << "static const char* const " << ci.c_name
                     << "_type_name sinter_maybe_unused = \"" << ci.name << "\";\n";
            h_types_ << "static const char* const " << ci.c_name
                     << "_type_chain[] sinter_maybe_unused = {";
            for (size_t i = 0; i < chain.size(); ++i) {
                if (i == 0)
                    h_types_ << ci.c_name << "_type_name, ";
                else
                    h_types_ << "\"" << chain[i] << "\", ";
            }
            h_types_ << "0};\n\n";
        }

        // 构造/析构/拷贝/移动声明
        h_types_ << "struct " << ci.c_name << "* " << ci.c_name << "_new(";
        if (ci.ctor_params.empty()) h_types_ << "void";
        for (size_t i = 0; i < ci.ctor_params.size(); ++i) {
            if (i) h_types_ << ", ";
            h_types_ << Semantic::param_decl(ci.ctor_params[i].type_c, param_c_name(ci.ctor_params[i]));
        }
        h_types_ << ");\n";
        h_types_ << "void " << ci.c_name << "_del(struct " << ci.c_name << "* self);\n";
        h_types_ << "void " << ci.c_name << "_del_inner(struct " << ci.c_name
                 << "* self);   /* destroy embedded sub-objects (does not free memory) */\n";
        h_types_ << "struct " << ci.c_name << "* " << ci.c_name << "_copy(struct " << ci.c_name
                 << "* self);\n";
        h_types_ << "struct " << ci.c_name << "* " << ci.c_name << "_move(struct " << ci.c_name
                 << "* self);\n";
        // 反射：把实例渲染成 `Type(field=value, ...)`。由编译器为每个类/结构体生成。
        h_types_ << "sinter_string " << ci.c_name << "_to_str(struct " << ci.c_name
                 << "* self);\n";
        if (ci.has_destructor) {
            h_types_ << "static inline void " << ci.c_name
                     << "_cleanup(void* p) { struct " << ci.c_name << "** v = (struct "
                     << ci.c_name << "**)p; if (*v) { " << ci.c_name
                     << "_del(*v); *v = 0; } }\n";
        }
        h_types_ << "\n";

        // Generic-container element dispatch for user-defined classes.
        // Containers borrow class references: clone/blank are pointer-preserving,
        // drop/free are no-ops. Hash/compare use pointer identity; text uses _to_str.
        const std::string tok = ci.c_name + "_ptr";
        h_types_ << "static inline struct " << ci.c_name << "* sinter_gen_clone_" << tok
                 << "(struct " << ci.c_name << "* v) { return v; }\n";
        h_types_ << "static inline struct " << ci.c_name << "* sinter_gen_blank_" << tok
                 << "(void) { return NULL; }\n";
        h_types_ << "static inline struct " << ci.c_name << "* sinter_gen_free_" << tok
                 << "(struct " << ci.c_name << "* v) { (void)v; return NULL; }\n";
        h_types_ << "static inline void sinter_gen_drop_" << tok
                 << "(struct " << ci.c_name << "* v) { (void)v; }\n";
        h_types_ << "static inline int64_t sinter_gen_hash_" << tok
                 << "(struct " << ci.c_name << "* v) { return (int64_t)(size_t)v; }\n";
        h_types_ << "static inline int64_t sinter_gen_cmp_" << tok
                 << "(struct " << ci.c_name << "* a, struct " << ci.c_name
                 << "* b) { size_t x=(size_t)a, y=(size_t)b; return x<y?-1:(x>y?1:0); }\n";
        h_types_ << "static inline sinter_string sinter_gen_text_" << tok
                 << "(struct " << ci.c_name << "* v) { return v ? " << ci.c_name
                 << "_to_str(v) : sinter_string_from_literal(\"\", 0); }\n";
        h_types_ << "static inline struct " << ci.c_name << "** sinter_gen_alloc_" << tok
                 << "(int64_t n) { return (struct " << ci.c_name
                 << "**)calloc((size_t)(n > 0 ? n : 1), sizeof(struct " << ci.c_name << "*)); }\n";
        h_types_ << "static inline struct " << ci.c_name << "** sinter_gen_grow_" << tok
                 << "(struct " << ci.c_name << "** p, int64_t n) { return n <= 0 ? NULL : (struct "
                 << ci.c_name << "**)realloc(p, (size_t)n * sizeof(struct " << ci.c_name << "*)); }\n\n";
    }

    void Codegen::emit_static_fields_impl(const ClassDecl *c) {
        for (auto &m: c->members) {
            auto *v = dynamic_cast<VarDeclStmt *>(m.get());
            if (!v || !v->is_static) continue;
            const std::string t = v->type ? c_type(v->type.get()) : "long long";
            const std::string gname = class_c_name(c->name) + "_s_" + Semantic::to_snake_ident(v->name);
            h_types_ << "extern " << t << " " << gname << ";\n";
            c_funs_ << t << " " << gname << " = ";
            if (t == "sinter_string") {
                // 静态字符串字段必须是常量初始化器
                if (auto *lit = v->init ? dynamic_cast<LiteralExpr *>(v->init.get()) : nullptr;
                    lit && lit->lit == LiteralExpr::String) {
                    std::string body = lit->raw;
                    if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
                        body = body.substr(1, body.size() - 2);
                    c_funs_ << "sinter_string_static(\"" << body << "\", "
                            << std::to_string(body.size()) << ")";
                } else {
                    c_funs_ << "sinter_string_static(\"\", 0)";
                }
            } else if (v->init) {
                c_funs_ << expr(v->init.get());
            } else {
                c_funs_ << "0";
            }
            c_funs_ << ";\n";
        }
        if (!c->members.empty()) h_types_ << "\n";
    }

    void Codegen::emit_alias_impl(const AliasStmt *a) {
        if (a->type)
            h_types_ << "typedef " << c_type(a->type.get()) << " " << a->name << ";\n";
    }

    void Codegen::emit_enum(const EnumDecl *e) {
        const size_t mark = (size_t)h_types_.tellp();
        emit_enum_impl(e);
        const std::string text = h_types_.str();
        if (text.size() > mark) {
            h_types_.str(text.substr(0, mark));
            h_types_.seekp(0, std::ios::end);
            type_decls_.push_back({e->is_pub, text.substr(mark)});
        }
    }

    void Codegen::emit_class_decl(const ClassDecl *c) {
        const size_t mark = (size_t)h_types_.tellp();
        emit_class_decl_impl(c);
        const std::string text = h_types_.str();
        if (text.size() > mark) {
            h_types_.str(text.substr(0, mark));
            h_types_.seekp(0, std::ios::end);
            type_decls_.push_back({c->is_pub, text.substr(mark)});
        }
    }

    void Codegen::emit_alias(const AliasStmt *a) {
        const size_t mark = (size_t)h_types_.tellp();
        emit_alias_impl(a);
        const std::string text = h_types_.str();
        if (text.size() > mark) {
            h_types_.str(text.substr(0, mark));
            h_types_.seekp(0, std::ios::end);
            type_decls_.push_back({a->is_pub, text.substr(mark)});
        }
    }

    void Codegen::emit_module_global(const VarDeclStmt *v) {
        const std::string t = v->type ? c_type(v->type.get()) : std::string("long long");
        const std::string gname = module_ident_ + "_g_" + v->name;
        module_globals_.insert(v->name);

        // C++ 风格的强制常量：全局作用域初始化器必须是常量表达式
        std::string init;
        bool runtime_init = false;
        if (v->init) {
            if (auto *lit = dynamic_cast<LiteralExpr *>(v->init.get())) {
                switch (lit->lit) {
                    case LiteralExpr::String: {
                        std::string body = lit->raw;
                        if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
                            body = body.substr(1, body.size() - 2);
                        init = "sinter_string_static(\"" + body + "\", " +
                               std::to_string(body.size()) + ")";
                        break;
                    }
                    case LiteralExpr::Bool:
                        init = (lit->raw == "true") ? "1u" : "0u";
                        break;
                    case LiteralExpr::Null:
                        init = "0";
                        break;
                    default:
                        // 数值字面量交给 expr_literal：它会处理 INT64_MIN 这类
                        // 无法直接写成 C 字面量的边界值（`(-9223372036854775807LL - 1)`）。
                        init = expr_literal(lit);
                        break;
                }
            } else {
                // 非字面量：需要运行期初始化
                runtime_init = true;
            }
        }

        // 声明（头文件只在 pub 时导出）
        if (v->is_pub) h_types_ << "extern " << t << " " << gname << ";\n";
        h_types_ << "\n";

        if (!runtime_init) {
            c_funs_ << "/* module-level " << (v->is_const ? "val" : "var") << " " << v->name << " */\n";
            c_funs_ << t << " " << gname << " = " << (init.empty() ? "0" : init) << ";\n\n";
        }
    }

    void Codegen::emit_static_fields(const ClassDecl *c) {
        const size_t mark = (size_t)h_types_.tellp();
        emit_static_fields_impl(c);
        const std::string text = h_types_.str();
        if (text.size() > mark) {
            h_types_.str(text.substr(0, mark));
            h_types_.seekp(0, std::ios::end);
            type_decls_.push_back({c->is_pub, text.substr(mark)});
        }
    }

    void Codegen::emit_vtables(const ClassDecl *c) {
        // vtable 只被本模块的构造函数/虚调用使用，因此无需导出到头文件。
        const size_t mark = (size_t)h_funs_.tellp();
        emit_vtables_impl(c);
        const std::string text = h_funs_.str();
        if (text.size() > mark) {
            h_funs_.str(text.substr(0, mark));
            h_funs_.seekp(0, std::ios::end);
            vtable_defs_.push_back(text.substr(mark));
        }
    }

    void Codegen::emit_vtables_impl(const ClassDecl *c) {
        auto it = sem_.classes().find(c->name);
        if (it == sem_.classes().end()) return;
        auto &ci = it->second;
        if (ci.vtable_slots.empty()) return;

        // 先算出每个槽位实际指向哪个函数（可为祖先的实现），再统一输出。
        struct Slot {
            std::string impl;
            std::string owner;
            const FunctionDecl *decl = nullptr;
        };
        std::vector<Slot> slots(ci.vtable_slots.size());
        for (size_t i = 0; i < ci.vtable_slots.size(); ++i) {
            const std::string slot_name = ci.vtable_slots[i];
            const auto dot = slot_name.rfind('.');
            const std::string method = dot == std::string::npos ? slot_name : slot_name.substr(dot + 1);

            std::vector<std::string> chain;
            std::string cur = c->name;
            while (!cur.empty() && sem_.classes().contains(cur)) {
                chain.push_back(cur);
                cur = class_by_name(cur).base_class;
            }
            for (auto &cls: chain) {
                auto fit = sem_.functions().find(method);
                if (fit == sem_.functions().end()) continue;
                for (auto &f: fit->second) {
                    if (f.owner_class == cls && !f.is_static) {
                        slots[i].impl = f.c_name;
                        slots[i].owner = cls;
                        slots[i].decl = f.decl;
                        break;
                    }
                }
                if (!slots[i].impl.empty()) break;
            }
        }

        // 槽位回落到「别的模块」里的基类实现时，本模块看不到那个函数符号：
        // 类的成员函数不导出到头文件（跨模块调用一律走 `__vptr`），但 vtable 是
        // 把函数地址直接写进数组，C 在这里必须先有声明。补一条模块私有原型。
        // 泛型类继承 `Any` 曾经就是卡在这一步（`'xxx_any_to_string' undeclared`）。
        for (auto &s: slots) {
            if (s.impl.empty() || !s.decl) continue;
            auto oit = sem_.classes().find(s.owner);
            if (oit == sem_.classes().end()) continue;
            if (oit->second.module_name == ci.module_name) continue;
            emit_function_decl_impl(s.decl, true, s.owner);
        }

        // thunk 必须先于数组定义输出（数组体内不能再写函数定义），
        // 所以这里先累积到 thunks，等数组写完再统一追加。
        std::ostringstream thunks;
        std::ostringstream thunk_decls;
        // thunk 的原型必须出现在数组之前（数组要取它的地址），
        // 定义放数组之后（定义体里要调用实现函数，那些原型已在上文发出）。
        // 这两处都先写好占位，最后再替换 —— 顺序无关，靠标记定位。
        h_funs_ << "@@SINTER_THUNK_DECLS@@";
        h_funs_ << "static sinter_vfn " << ci.c_name << "_vtable[" << ci.vtable_slots.size() << "] = {\n";
        for (size_t i = 0; i < slots.size(); ++i) {
            std::string entry = slots[i].impl.empty() ? "0" : slots[i].impl;
            // 槽位实现在派生类里（`slots[i].owner != c->name`）时，vtable 会被
            // **基类子对象**（`self->__base.__vptr`）共用。此时调用点传进来的
            // 是 `Base*`，而实现要求 `Derived*` —— 直接塞函数地址会让实现
            // 按派生类偏移读字段（实测 `area()` 恒为 0）。
            //
            // 这里为这种情况生成一个薄 thunk：接收 `Base*`，经 `__parent`
            // 找回最外层对象，再按实现类的类型调用。
            if (!slots[i].impl.empty() && slots[i].decl) {
                // thunk 需要：基类子对象（`Shape*`）也能用这张表，
                // 而实现要求 `Rect*`。判据是「实现的声明类 ≠ 本类」或
                // 「本类有基类」（此时 `__base` 可能带着这张表被调用）。
                const std::string impl_c = slots[i].owner;
                const ClassInfo *self_ci = sem_.find_class(c->name);
                const bool has_base = self_ci && !self_ci->base_class.empty();
                const bool need_thunk = has_base && !impl_c.empty();
                if (need_thunk) {
                    const std::string thunk = ci.c_name + "_vfn" + std::to_string(i);
                    // thunk 的形参类型取「本类的直接基类」：基类子对象用的就是它
                    const ClassInfo *bo = sem_.find_class(self_ci->base_class);
                    const ClassInfo *impl_ci = sem_.find_class(impl_c);
                    const TypeAnnotation *rt = slots[i].decl->return_type.get();
                    const bool is_void = !rt || (rt->kind == TypeAnnotation::Named && rt->name == "void");
                    if (bo && impl_ci) {
                        // 声明放 thunk_decls（数组前输出），定义放 thunks（数组后输出）
                        std::string sig_txt = (is_void ? std::string("void") : sem_.type_to_c(rt)) + " " +
                                              thunk + "(struct " + bo->c_name + "* __self";
                        for (auto &p: slots[i].decl->params)
                            sig_txt += ", " + sem_.type_to_c(p.type.get()) + " " +
                                       Semantic::to_snake_ident(p.name);
                        sig_txt += ")";
                        thunk_decls << "static " << sig_txt << ";\n";
                        thunks << "static " << sig_txt << " {\n    ";
                        if (!is_void) thunks << "return ";
                        // `__self` 本身就是基类子对象的地址，已经是合法的 `Base*`。
                        // 只有当实现属于派生类（要按派生类偏移读字段）时，
                        // 才需要用 `__self->__parent` 找回最外层对象；
                        // 实现对基类布局已经够用时直接传 `__self`，
                        // 否则 `Base.describe` 拿到 `Derived*` 会按错误偏移读字段。
                        if (impl_c == self_ci->base_class)
                            thunks << slots[i].impl << "(__self";
                        else
                            thunks << slots[i].impl << "((struct " << impl_ci->c_name
                                   << "*)__self->__parent";
                        for (auto &p: slots[i].decl->params)
                            thunks << ", " << Semantic::to_snake_ident(p.name);
                        thunks << ");\n}\n";
                        entry = thunk;
                    }
                }
            }
            h_funs_ << "    (sinter_vfn)" << entry;
            if (i + 1 < slots.size()) h_funs_ << ",";
            h_funs_ << "  /* " << ci.vtable_slots[i] << " */\n";
        }
        h_funs_ << "};\n\n";
        // 数组之后追加 thunk 定义，并把原型插回数组之前的位置。
        h_funs_ << thunks.str();
        {
            // 把占位标记替换成原型集合（h_funs_ 是流，只能整体重建）
            std::string body = h_funs_.str();
            const std::string marker = "@@SINTER_THUNK_DECLS@@";
            if (const size_t at = body.find(marker); at != std::string::npos)
                body.replace(at, marker.size(), thunk_decls.str());
            h_funs_.str("");
            h_funs_.clear();
            h_funs_ << body;
        }
    }

    std::string Codegen::c_type_short(const TypeAnnotation *t) const {
        if (!t) return "i64";
        switch (t->kind) {
            case TypeAnnotation::Named: {
                if (t->name.empty()) return "i64";
                if (t->name == "string") return "str";
                if (t->name == "int") return "i32";
                if (t->name == "long") return "i64";
                if (t->name == "short") return "i16";
                if (t->name == "byte") return "u8";
                if (t->name == "float" || t->name == "double") return "f64";
                return Semantic::to_snake_ident(t->name);
            }
            case TypeAnnotation::Pointer: return c_type_short(t->inner_type.get()) + "_ptr";
            case TypeAnnotation::Optional: return c_type_short(t->inner_type.get()) + "_opt";
            case TypeAnnotation::Array: return c_type_short(t->inner_type.get()) + "_arr";
            case TypeAnnotation::Generic: {
                std::string r = Semantic::to_snake_ident(t->name);
                for (auto &g: t->generic_args) r += "_" + c_type_short(g.get());
                return r;
            }
            default: return "i64";
        }
    }

    CType Codegen::arg_type_of(Expression *e) {
        const std::string at = c_type_of_expr(e);
        if (at == "double" || at == "float") return CType::make_double();
        if (at == "const char*" || at == "char*") return CType::make_str();
        // 字符串字面量 / 字符串表达式的 C 类型是 sinter_string，不能落到兜底的 i64
        if (at == "sinter_string") return CType::make_str();
        if (at == "int64_t") return CType::make_int(64, true, "i64");
        if (at == "int32_t") return CType::make_int(32, true, "i32");
        if (at == "int16_t") return CType::make_int(16, true, "i16");
        if (at == "int8_t") return CType::make_int(8, true, "i8");
        if (at == "uint32_t") return CType::make_int(32, false, "u32");
        // `bool` 在 C 里就是 `uint8_t`，但 `CType::Bool` 与 `CType::Int` 在
        // 重载评分里走不同分支（Int 不接受 uint8_t 形参）。这里若不还原成
        // Bool，`println(x == y)` 会把比较结果当成整数，输出 `1` 而不是 `true`。
        if (at == "uint8_t") return CType::make_bool();
        // `long long` 是**默认整数类型**，不是 bool。
        // 这里曾经返回 `make_bool()`，于是凡是 `c_type_of_expr` 兜底成
        // `long long` 的表达式（`*p` 解引用、模块级常量等）都被当成布尔值，
        // `println(*p)` 输出 `true` 而不是 42，`println(LIMIT)` 输出 `true` 而不是 12。
        if (at == "long long") return CType::make_int(64, true, "i64");
        if (!at.empty() && at.back() == '*') {
            CType t;
            t.kind = CType::Ptr;
            t.name = at;
            t.sinter_name = at;
            return t;
        }
        return CType::make_int(64, true, "i64");
    }

    std::string Codegen::upcast_if_needed(Expression *obj, const std::string &want_c_type) {
        if (!obj || want_c_type.empty()) return expr(obj);
        // 字符串形参：str -> sinter_string / cstr -> const char*，自动双向适配
        {
            const std::string have = c_type_of_expr(obj);
            const bool want_str = want_c_type == "sinter_string";
            const bool want_cstr = want_c_type == "const char*" || want_c_type == "char*";
            const bool have_str = have == "sinter_string" || have == "const char*" || have == "char*";
            if (have_str && (want_str || want_cstr)) {
                if (want_str) return str_cast(expr(obj), have);
                return cstr_cast(expr(obj), have);
            }
            // `println(obj)` 这类「把结构体/类直接当字符串传」的场景：
            // 形参要 sinter_string 而实参是 `struct X*`，走编译器生成的 to_str（反射）。
            if (want_str && have.rfind("struct ", 0) == 0) return str_cast(expr(obj), have);
            // 形参要可空（`sinter_string*`），实参是值：自动取地址。
            // 没有这条，`greetingFor("ada")` 会把 `sinter_string` 塞进
            // `sinter_string*`，gcc 报 `incompatible type for argument`。
            //
            // 写法上用一个**单元素数组复合字面量**：`(sinter_string[]){v}` 在 C 里会
            // 退化成指向首元素的 `sinter_string*`，类型天然正确。
            //
            // 试过的两种写法都不行：
            //   `&(sinter_string){v}`  —— 少一层括号，`(sinter_string)` 被当成强制转换；
            //   `&((sinter_string){v})` —— 加了括号仍被 GCC 当转换处理，得到 `char*`。
            if (want_c_type == "sinter_string*" && have == "sinter_string") {
                const std::string code = expr(obj);
                return "(sinter_string[]){" + code + "}";
            }
        }
        // null 字面量保持 NULL，不能对它做 ->__base
        if (auto *lit = dynamic_cast<LiteralExpr *>(obj))
            if (lit->lit == LiteralExpr::Null) return "NULL";
        // 三元表达式：逐分支转型（否则 (&(c ? X : NULL)->__base) 会对 NULL 解引用）
        if (auto *te = dynamic_cast<TernaryExpr *>(obj); te && !te->is_elvis) {
            auto is_null_lit = [](Expression *x) {
                if (auto *le = dynamic_cast<LiteralExpr *>(x)) return le->lit == LiteralExpr::Null;
                return false;
            };
            const bool ln = is_null_lit(te->then_b.get());
            const bool rn = is_null_lit(te->else_b.get());
            if (ln || rn) {
                const std::string a = upcast_if_needed(te->then_b.get(), want_c_type);
                const std::string b = upcast_if_needed(te->else_b.get(), want_c_type);
                return "(" + expr(te->cond.get()) + " ? " + a + " : " + b + ")";
            }
        }
        // if 表达式（if (c) A else B）：同样逐分支转型
        if (auto *ie = dynamic_cast<IfExpr *>(obj); ie && ie->else_b) {
            auto is_null_lit = [](Expression *x) {
                if (auto *le = dynamic_cast<LiteralExpr *>(x)) return le->lit == LiteralExpr::Null;
                return false;
            };
            if (is_null_lit(ie->then_b.get()) || is_null_lit(ie->else_b.get())) {
                const std::string a = upcast_if_needed(ie->then_b.get(), want_c_type);
                const std::string b = upcast_if_needed(ie->else_b.get(), want_c_type);
                return "(" + expr(ie->cond.get()) + " ? " + a + " : " + b + ")";
            }
        }
        const std::string got = c_type_of_expr(obj);
        if (got == want_c_type) return expr(obj);
        if (got.empty() || got.back() != '*' || want_c_type.back() != '*') return expr(obj);
        // 找到实参类型对应的 sinter 类
        std::string cur;
        for (auto &[cn, ci]: sem_.classes())
            if ("struct " + ci.c_name + "*" == got) { cur = cn; break; }
        if (cur.empty())
            for (auto &[cn, ci]: sem_.external_classes())
                if ("struct " + ci.c_name + "*" == got) { cur = cn; break; }
        if (cur.empty()) return expr(obj);
        std::string code = expr(obj);
        int guard = 0;
        while (guard++ < 32) {
            const ClassInfo *cc = sem_.find_class(cur);
            if (!cc || cc->base_class.empty()) break;
            std::string up = code + "->__base";
            code = "(&" + up + ")";
            const ClassInfo *bc = sem_.find_class(cc->base_class);
            if (!bc) break;
            if ("struct " + bc->c_name + "*" == want_c_type) return code;
            cur = cc->base_class;
        }
        return expr(obj);
    }

    bool Codegen::is_subclass_of_type(const std::string &got, const std::string &want) {
        std::string cur;
        for (auto &[cn, ci]: sem_.classes())
            if ("struct " + ci.c_name + "*" == got) { cur = cn; break; }
        if (cur.empty())
            for (auto &[cn, ci]: sem_.external_classes())
                if ("struct " + ci.c_name + "*" == got) { cur = cn; break; }
        int guard = 0;
        while (!cur.empty() && guard++ < 32) {
            const ClassInfo *cc = sem_.find_class(cur);
            if (!cc) return false;
            if ("struct " + cc->c_name + "*" == want) return true;
            cur = cc->base_class;
        }
        return false;
    }
} // namespace sinter
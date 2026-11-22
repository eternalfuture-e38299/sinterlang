// sinter 类型检查实现。
//
// 目标不是做完整的类型系统，而是拦住「一定会让 C 编译器报错」的那类程序，
// 并且把错误指向用户自己写的那一行。所有判断都遵循 typeck.hpp 里的保守原则。
#include "sem/typeck.hpp"

#include <algorithm>

namespace sinter {
    namespace {
        bool has_annotation(const Statement *s, const char *name) {
            if (!s) return false;
            for (const auto &a: s->annotations)
                if (a.name == name) return true;
            return false;
        }

        const Annotation *find_annotation(const Statement *s, const char *name) {
            if (!s) return nullptr;
            for (const auto &a: s->annotations)
                if (a.name == name) return &a;
            return nullptr;
        }

        /// Literal string argument of an annotation (quotes stripped).
        std::string annotation_arg(const Annotation *a, size_t index) {
            if (!a || index >= a->args.size()) return {};
            if (auto *lit = dynamic_cast<LiteralExpr *>(a->args[index].get());
                lit && lit->lit == LiteralExpr::String) {
                std::string s = lit->raw;
                if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                    s = s.substr(1, s.size() - 2);
                return s;
            }
            return {};
        }

        /// e.g. `@deprecated("use g", "1.2", "2.0")`
        /// -> "use g (deprecated since 1.2, removal planned for 2.0)"
        std::string deprecation_hint(const Statement *s) {
            const Annotation *a = find_annotation(s, "deprecated");
            if (!a) return "use the replacement API";
            const std::string msg = annotation_arg(a, 0);
            const std::string since = annotation_arg(a, 1);
            const std::string remove = annotation_arg(a, 2);
            std::string h = msg.empty() ? "use the replacement API" : msg;
            if (!since.empty()) h += " (deprecated since " + since;
            if (!remove.empty()) h += (since.empty() ? " (planned removal " : ", removal planned ") + remove;
            if (!since.empty() || !remove.empty()) h += ")";
            return h;
        }

        /// Map named/positional call arguments onto parameter slots.
        /// Missing slots are allowed only when the declaration has a default value.
        static bool map_named_args(const std::vector<Argument> &args,
                                   const std::vector<ParamInfo> &params,
                                   const FunctionDecl *decl,
                                   const std::vector<CType> &arg_types,
                                   std::vector<CType> &out,
                                   std::string &why) {
            out.assign(params.size(), CType::unknown());
            std::vector<char> filled(params.size(), 0);
            size_t next = 0;
            for (size_t i = 0; i < args.size() && i < arg_types.size(); ++i) {
                size_t slot = static_cast<size_t>(-1);
                if (args[i].name.empty()) {
                    while (next < out.size() && filled[next]) ++next;
                    slot = next;
                } else {
                    for (size_t k = 0; k < params.size(); ++k)
                        if (params[k].name == args[i].name) { slot = k; break; }
                    if (slot >= out.size()) {
                        why = "no parameter named '" + args[i].name + "'";
                        return false;
                    }
                }
                if (slot >= out.size()) continue;   // varargs eats the rest
                if (filled[slot]) {
                    why = "parameter '" + params[slot].name + "' is given more than once";
                    return false;
                }
                out[slot] = arg_types[i];
                filled[slot] = 1;
            }
            for (size_t k = 0; k < out.size(); ++k) {
                if (filled[k]) continue;
                if (decl && k < decl->params.size() && decl->params[k].default_value) continue;
                why = "missing argument for parameter '" + params[k].name + "' (it has no default value)";
                return false;
            }
            return true;
        }

        /// `T&` used as a value: drop the reference marker and one pointer layer.
        static CType deref_ref(CType t) {
            if (!t.is_ref) return t;
            t.is_ref = false;
            if (t.kind == CType::Ptr) {
                while (!t.name.empty() && t.name.back() == '*') t.name.pop_back();
                while (!t.name.empty() && t.name.back() == ' ') t.name.pop_back();
                if (!t.sinter_name.empty() && t.sinter_name.back() == '&')
                    t.sinter_name.pop_back();
            }
            return t;
        }

        /// 递归保护：AST 异常时不要爆栈。
        struct DepthGuard {
            int &d;
            explicit DepthGuard(int &dd) : d(dd) { ++d; }
            ~DepthGuard() { --d; }
        };

        /// 运算符的显示字符，用于诊断文案
        char op_char(TokenType t) {
            switch (t) {
                case TokenType::Plus: return '+';
                case TokenType::Minus: return '-';
                case TokenType::Star: return '*';
                case TokenType::Slash: return '/';
                case TokenType::Percent: return '%';
                default: return '?';
            }
        }

        /// 内建类型可以当扩展函数的接收者（`ext fun str.length()` 这类）。
        /// 它们没有 ClassInfo，成员调用的判定必须走单独一条路。
        bool is_builtin_receiver(const CType &t) {
            if (t.kind == CType::Str || t.kind == CType::Char || t.kind == CType::Bool) return true;
            if (t.kind == CType::Int || t.kind == CType::Float) return true;
            if (t.kind != CType::Ptr) return false;
            // 名字可能记在两处：`name` 是 C 侧名（`str*`），`sinter_name` 是
            // Sinter 侧名（`str`）。两者都试，取去星后的基名判断。
            const auto strip = [](std::string s) {
                if (!s.empty() && s.back() == '*') s.pop_back();
                return s;
            };
            const std::string base_c = strip(t.name);
            const std::string base_i = strip(t.sinter_name);
            if (t.sinter_name == "ptr" || t.name == "void*") return false;
            const auto builtin = [](const std::string &b) {
                return b == "str" || b == "char" || b == "bool" ||
                       b == "i64" || b == "i32" || b == "i16" || b == "i8" ||
                       b == "u64" || b == "u32" || b == "u16" || b == "u8" ||
                       b == "f64" || b == "f32";
            };
            // `str?` / `i64?` 解析出来是 `str*` / `int64_t*`。可空包装的底层仍是
            // 内建类型，也要允许挂扩展函数 —— 否则 `maybe?.length()` 会误报
            // `type str* has no member named length`（调用点会自动解一层 `*`）。
            if (builtin(base_i) || builtin(base_c)) return true;
            // `cstr` 这类具名指针别名
            return !t.sinter_name.empty() && t.sinter_name != "ptr";
        }

        /// 用于诊断文案：Sinter 侧类型名，Unknown 时返回「值」
        std::string type_label(const CType &t) {
            if (t.kind == CType::Unknown) return "value";
            if (t.kind == CType::Str) return "str";
            if (!t.sinter_name.empty()) return t.sinter_name;
            return t.to_c();
        }
    }

    const CType *TypeChecker::lookup_local(const std::string &name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    void TypeChecker::require_assignable(const CType &from, const CType &to, const Expression *at,
                                         const std::string &what, const std::string &code) {
        if (from.kind == CType::Unknown || to.kind == CType::Unknown) return;
        if (Semantic::is_assignable(from, to)) return;
        // 子类可以赋给父类：`val base: Animal = Dog(...)`
        if (from.kind == CType::Struct && to.kind == CType::Struct &&
            is_subclass_of(from.sinter_name, to.sinter_name))
            return;
        sem_.report_error_ex(code,
                             what + ": expected " + type_label(to) + ", got " + type_label(from),
                             "check the declared type and the type of the value on the right",
                             at);
    }

    bool TypeChecker::is_subclass_of(const std::string &got, const std::string &want) const {
        if (got.empty() || want.empty() || got == want) return true;
        const ClassInfo *ci = find_class(got);
        // 防御性上限：继承链异常时不要死循环
        for (int guard = 0; ci && guard < 64; ++guard) {
            if (ci->name == want) return true;
            if (ci->base_class.empty()) return false;
            ci = find_class(ci->base_class);
        }
        return false;
    }

    CType TypeChecker::infer(Expression *e) {
        if (!e) return CType::unknown();
        if (depth_ > 200) return CType::unknown();
        DepthGuard guard(depth_);

        if (auto *l = dynamic_cast<LiteralExpr *>(e)) {
            switch (l->lit) {
                case LiteralExpr::Int: {
                    // 带后缀的字面量按后缀定型，否则默认 i64
                    if (!l->suffix.empty()) {
                        auto t = type_of_name(l->suffix);
                        if (t.kind != CType::Unknown) return t;
                    }
                    return CType::make_int(64, true, "i64");
                }
                case LiteralExpr::Float: return CType::make_double();
                case LiteralExpr::String: return CType::make_str();
                case LiteralExpr::Bool: return CType::make_bool();
                // char / null 的类型语义与后端有耦合，这里不做判断
                default: return CType::unknown();
            }
        }

        if (auto *id = dynamic_cast<IdentifierExpr *>(e)) {
            if (const CType *t = lookup_local(id->name)) {
                // 局部变量可能尚未推出来（值为 Unknown），照样返回
                return deref_ref(*t);
            }
            // 字段：方法体里直接写字段名
            if (current_class_) {
                for (auto &f: current_class_->fields)
                    if (f.name == id->name) return type_of_name(f.type_sinter);
            }
            return CType::unknown();
        }

        if (auto *g = dynamic_cast<GroupingExpr *>(e)) return infer(g->expr.get());
        if (auto *n = dynamic_cast<NonNullExpr *>(e)) return infer(n->operand.get());

        if (auto *cst = dynamic_cast<CastExpr *>(e)) {
            return sem_.resolve_type(cst->target.get());
        }

        if (dynamic_cast<StringTemplateExpr *>(e)) return CType::make_str();

        if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
            const CType lt = infer(b->left.get());
            const CType rt = infer(b->right.get());
            const bool lk = lt.kind != CType::Unknown;
            const bool rk = rt.kind != CType::Unknown;
            const bool lstr = lt.kind == CType::Str;
            const bool rstr = rt.kind == CType::Str;

            switch (b->op) {
                case TokenType::Plus:
                    // str + 任意 → 拼接（Sinter 允许把数字当成字符串片段）
                    if (lstr || rstr) return CType::make_str();
                    if (lk && rk && lt.is_numeric() && rt.is_numeric())
                        return Semantic::promote(lt, rt);
                    // 可能是运算符重载（Vec + Vec），交给后端
                    return CType::unknown();

                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Percent:
                    if (lstr || rstr) {
                        sem_.report_error_ex("E0304",
                                             "operator '" + std::string(1, op_char(b->op)) +
                                                 "' cannot be used with str",
                                             "convert the str to a number first, or concatenate "
                                             "with '+'",
                                             e);
                        return CType::unknown();
                    }
                    if (lk && rk && lt.is_numeric() && rt.is_numeric())
                        return Semantic::promote(lt, rt);
                    return CType::unknown();

                case TokenType::EqualEqual:
                case TokenType::NotEqual:
                    if (lk && rk) {
                        const bool lnum = lt.is_numeric(), rnum = rt.is_numeric();
                        if ((lstr && rnum) || (rstr && lnum)) {
                            sem_.report_error_ex("E0305",
                                             "cannot compare str with a number (the comparison "
                                             "would always be false)",
                                             "compare str with str, or convert the number to str first",
                                             e);
                        }
                    }
                    return CType::make_bool();

                case TokenType::Less:
                case TokenType::Greater:
                case TokenType::LessEqual:
                case TokenType::GreaterEqual:
                    if ((lstr && rt.is_numeric()) || (rstr && lt.is_numeric())) {
                        sem_.report_error_ex("E0305", "cannot compare str with a number",
                                             "compare str with str, or convert the number to str first",
                                             e);
                    }
                    return CType::make_bool();

                case TokenType::DoubleAmpersand:
                case TokenType::DoublePipe:
                    return CType::make_bool();

                case TokenType::QuestionQuestion:
                case TokenType::Elvis:
                    return rk ? rt : lt;

                default: return CType::unknown();
            }
        }

        if (auto *u = dynamic_cast<UnaryExpr *>(e)) {
            const CType t = infer(u->operand.get());
            if (u->op == TokenType::Bang) return CType::make_bool();
            if (t.kind == CType::Unknown) return CType::unknown();
            // `&x` 得到 `T*`；`*p` 得到 `T`（去掉一层 *）。指针自身加减/比较交给 C。
            if (u->op == TokenType::Ampersand) {
                if (t.kind == CType::Struct || t.kind == CType::Enum) {
                    CType r = t;
                    r.is_ref = true;
                    r.sinter_name = t.sinter_name + "&";
                    return r;
                }
                CType r;
                r.kind = CType::Ptr;
                r.name = t.to_c() + "*";
                r.sinter_name = t.sinter_name + "&";
                r.is_ref = true;
                return r;
            }
            if (u->op == TokenType::Star) {
                if (auto *oid = dynamic_cast<IdentifierExpr *>(u->operand.get())) {
                    if (const CType *raw = lookup_local(oid->name); raw && raw->is_ref)
                        return deref_ref(*raw);
                }
                if (t.is_ref) {
                    CType r = t;
                    r.is_ref = false;
                    if (r.kind == CType::Ptr) {
                        while (!r.name.empty() && r.name.back() == '*') r.name.pop_back();
                        if (!r.sinter_name.empty() && r.sinter_name.back() == '&')
                            r.sinter_name.pop_back();
                    }
                    return r;
                }
                std::string n = t.to_c();
                if (!n.empty() && n.back() == '*') {
                    n.pop_back();
                    CType r = t;
                    r.name = n;
                    r.sinter_name = t.sinter_name;
                    r.kind = (n == "void") ? CType::Void : CType::Unknown;
                    // 已知标量名字就还给标量类型，便于后续算术检查
                    if (n == "int64_t" || n == "long long") return CType::make_int(64, true, "i64");
                    if (n == "int32_t" || n == "int") return CType::make_int(32, true, "i32");
                    if (n == "double") return CType::make_double();
                    if (n == "float") return CType::make_float();
                    if (n == "uint8_t") return CType::make_bool();
                    if (n == "sinter_string") return CType::make_str();
                    return r;
                }
                sem_.report_error_ex("E0305", "cannot dereference a non-pointer value",
                                     "only `T*` / `ptr` values can be dereferenced", e);
                return CType::unknown();
            }
            if (t.kind == CType::Str) {
                sem_.report_error_ex("E0304", "operator cannot be used with str",
                                     "convert the str to a number first", e);
                return CType::unknown();
            }
            return t;
        }

        if (auto *c = dynamic_cast<CallExpr *>(e)) return infer_call(c);

        if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) {
            const CType obj = infer(m->object.get());
            // 枚举成员 / 静态成员：交给后端
            if (obj.kind == CType::Unknown) return CType::unknown();
            if (const ClassInfo *ci = find_class(obj.sinter_name)) {
                for (auto &f: ci->fields)
                    if (f.name == m->member) return type_of_name(f.type_sinter);
            }
            return CType::unknown();
        }

        if (auto *ix = dynamic_cast<IndexAccessExpr *>(e)) {
            infer(ix->object.get());
            infer(ix->index.get());
            return CType::unknown();   // 数组元素类型不在本轮推断范围
        }

        if (auto *as = dynamic_cast<AssignmentExpr *>(e)) {
            const CType vt = infer(as->value.get());
            if (auto *id = dynamic_cast<IdentifierExpr *>(as->target.get())) {
                if (const CType *lt = lookup_local(id->name)) {
                    if (lt->is_ref) {
                        if (auto *lit = dynamic_cast<LiteralExpr *>(as->value.get());
                            lit && lit->lit == LiteralExpr::Null)
                            sem_.report_error_ex("E0301", "cannot assign null to reference '" + id->name + "'",
                                                 "a T& reference is non-null", as->line, as->col);
                    }
                    require_assignable(vt, deref_ref(*lt), as->value.get(), "assignment type mismatch", "E0302");
                }
            }
            return vt;
        }

        if (auto *t = dynamic_cast<TernaryExpr *>(e)) {
            infer(t->cond.get());
            const CType a = infer(t->then_b.get());
            const CType b2 = infer(t->else_b.get());
            if (a.kind != CType::Unknown && b2.kind != CType::Unknown &&
                a.kind != b2.kind && a.is_numeric() && b2.is_numeric())
                return Semantic::promote(a, b2);
            return a.kind != CType::Unknown ? a : b2;
        }

        if (auto *arr = dynamic_cast<ArrayLiteralExpr *>(e)) {
            for (auto &el: arr->elements) infer(el.get());
            return CType::unknown();
        }

        return CType::unknown();
    }

    CType TypeChecker::infer_call(CallExpr *c) {
        if (!c) return CType::unknown();
        // 实参类型
        std::vector<CType> arg_types;
        arg_types.reserve(c->args.size());
        for (auto &a: c->args) arg_types.push_back(infer(a.value.get()));

        // 1) 直接调用：Class(...) 构造 或 自由函数
        if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get())) {
            if (const ClassInfo *ci = find_class(id->name)) {
                if (ci->decl && has_annotation(ci->decl, "deprecated"))
                    sem_.report_weak_ex("W0302", "construction of deprecated type '" + id->name + "'",
                                        deprecation_hint(ci->decl), c->line, c->col);
                for (auto &a: c->args) (void) a;
                // 一个类可以有多个构造函数。`ctor_params` 只记录其中一组（第一个
                // 登记的），所以「实参个数少于它」不一定是错 —— 先看有没有别的
                // 构造函数变体正好接受这个个数；有就放行，没有才报错。
                bool matched_variant = false;
                if (!ci->ctor_params.empty() && arg_types.size() < ci->ctor_params.size()) {
                    if (auto hit = sem_.functions().find(ci->name); hit != sem_.functions().end()) {
                        for (auto &f: hit->second) {
                            if (!f.is_method || f.owner_class != ci->name) continue;
                            if (f.params.size() == arg_types.size()) {
                                matched_variant = true;
                                break;
                            }
                            // 缺的那几个位置全都有默认值 => 也是一次合法调用
                            if (f.params.size() >= arg_types.size() && f.decl &&
                                f.decl->params.size() == f.params.size()) {
                                bool all_defaulted = true;
                                for (size_t k = arg_types.size(); k < f.params.size(); ++k) {
                                    if (!f.decl->params[k].default_value) { all_defaulted = false; break; }
                                }
                                if (all_defaulted) {
                                    matched_variant = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                bool any_named = false;
                for (auto &a: c->args) if (!a.name.empty()) { any_named = true; break; }
                if (any_named && !ci->ctor_params.empty()) {
                    const FunctionDecl *ctor_decl = nullptr;
                    if (auto hit = sem_.functions().find(ci->name); hit != sem_.functions().end()) {
                        for (auto &f: hit->second) {
                            if (f.is_method && f.owner_class == ci->name && f.decl &&
                                f.params.size() == ci->ctor_params.size()) {
                                ctor_decl = f.decl;
                                break;
                            }
                        }
                    }
                    std::vector<char> filled(ci->ctor_params.size(), 0);
                    size_t next = 0;
                    std::string why;
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        size_t slot = static_cast<size_t>(-1);
                        if (c->args[i].name.empty()) {
                            while (next < filled.size() && filled[next]) ++next;
                            slot = next;
                        } else {
                            for (size_t k = 0; k < ci->ctor_params.size(); ++k)
                                if (ci->ctor_params[k].name == c->args[i].name) { slot = k; break; }
                            if (slot >= filled.size()) {
                                why = "no constructor parameter named '" + c->args[i].name + "'";
                                break;
                            }
                        }
                        if (slot < filled.size()) filled[slot] = 1;
                    }
                    if (why.empty()) {
                        for (size_t k = 0; k < filled.size(); ++k) {
                            if (filled[k]) continue;
                            if (ci->decl && k < ci->decl->ctor_params.size() &&
                                ci->decl->ctor_params[k].default_value) continue;
                            if (ctor_decl && k < ctor_decl->params.size() &&
                                ctor_decl->params[k].default_value) continue;
                            why = "missing constructor argument '" + ci->ctor_params[k].name + "'";
                            break;
                        }
                    }
                    if (!why.empty())
                        sem_.report_error_ex("E0204", id->name + ": " + why,
                                             "check the parameter names and provide every required argument",
                                             c->line, c->col);
                } else if (!matched_variant && !ci->ctor_params.empty() &&
                           arg_types.size() < ci->ctor_params.size()) {
                    sem_.report_error_ex("E0204",
                                             "not enough constructor arguments: " + id->name + " requires " +
                                                 std::to_string(ci->ctor_params.size()) + " argument(s), got " +
                                                 std::to_string(arg_types.size()),
                                             "pass every constructor parameter, or give it a default value",
                                             c);
                }
                CType t;
                t.kind = CType::Struct;
                t.name = "struct " + ci->c_name + "*";
                t.sinter_name = ci->name;
                return t;
            }
            bool any_named = false;
            for (auto &a: c->args) if (!a.name.empty()) { any_named = true; break; }
            if (any_named) {
                const FunctionInfo *named = nullptr;
                std::string why = "no function accepts these named arguments";
                auto it = sem_.functions().find(id->name);
                if (it != sem_.functions().end()) {
                    for (auto &f: it->second) {
                        if (f.is_method) continue;
                        std::vector<CType> mapped;
                        std::string bad;
                        if (!map_named_args(c->args, f.params, f.decl, arg_types, mapped, bad)) {
                            if (why == "no function accepts these named arguments") why = bad;
                            continue;
                        }
                        const FunctionInfo *hit = sem_.resolve_overload(id->name, mapped);
                        if (hit) { named = hit; break; }
                    }
                }
                if (!named) {
                    sem_.report_error_ex("E0204", id->name + ": " + why,
                                         "check the parameter names and provide every required argument",
                                         c->line, c->col);
                    return CType::unknown();
                }
                if (named->decl && has_annotation(named->decl, "deprecated"))
                    sem_.report_weak_ex("W0302", "call to deprecated function '" + id->name + "'",
                                        deprecation_hint(named->decl), c->line, c->col);
                if (!named->return_type_name.empty()) return type_of_name(named->return_type_name);
                return type_of_name("void");
            }
            if (const FunctionInfo *fi = sem_.resolve_overload(id->name, arg_types)) {
                if (fi->decl && has_annotation(fi->decl, "deprecated"))
                    sem_.report_weak_ex("W0302", "call to deprecated function '" + id->name + "'",
                                        deprecation_hint(fi->decl), c->line, c->col);
                if (fi->decl && has_annotation(fi->decl, "experimental"))
                    sem_.report_weak_ex("W0304", "call to experimental function '" + id->name + "'",
                                        "API may change before it is frozen", c->line, c->col);
                if (!fi->return_type_name.empty()) return type_of_name(fi->return_type_name);
                if (!fi->params.empty()) return CType::unknown();
                return type_of_name("void");
            }
            // 注意：这里**故意不检查**「函数是否已声明」。
            // Sinter 大量使用 `bind C` / `embed C` / 跨模块隐式依赖，
            // Semantic::functions() 并不是名字的全集，据此报错会把
            // 合法程序（std 的 str/arr 模块全是这种情况）大面积判死。
            // 真正能可靠判断的是「类型层面确定矛盾」，见上面几条规则。
            return CType::unknown();
        }

        // 2) 方法调用：obj.method(...)
        if (auto *m = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
            const CType obj = infer(m->object.get());
            if (const ClassInfo *ci = find_class(obj.sinter_name)) {
                auto fit = sem_.functions().find(m->member);
                // 沿继承链判断「这个类有没有这个成员」：继承来的方法记在基类的
                // owner_class 下，只看 ci->name 会误判。
                auto owner_is_in_chain = [&](const std::string &owner) {
                    const ClassInfo *cur = ci;
                    int guard = 0;
                    while (cur && guard++ < 64) {
                        if (cur->name == owner) return true;
                        if (cur->base_class.empty()) return false;
                        cur = find_class(cur->base_class);
                    }
                    return false;
                };
                bool member_exists = false;
                const FunctionInfo *best = nullptr;
                bool any_named = false;
                for (auto &a: c->args) if (!a.name.empty()) { any_named = true; break; }
                std::string named_why = "no method accepts these named arguments";
                if (fit != sem_.functions().end()) {
                    for (auto &f: fit->second) {
                        if (owner_is_in_chain(f.owner_class)) {
                            member_exists = true;
                            if (any_named) {
                                std::vector<CType> mapped;
                                std::string why;
                                if (map_named_args(c->args, f.params, f.decl, arg_types, mapped, why)) {
                                    best = &f;
                                    break;
                                }
                                if (named_why == "no method accepts these named arguments") named_why = why;
                                if (!best) best = &f;   // keep a candidate for better diagnostics
                                continue;
                            }
                            if (f.params.size() == arg_types.size()) {
                                best = &f;
                                break;
                            }
                            if (!best) best = &f;
                        }
                    }
                    if (!member_exists) {
                        // 扩展函数 `ext fun T.name()` 也算合法成员
                        for (auto &f: fit->second)
                            if (!f.ext_receiver.empty() && owner_is_in_chain(f.ext_receiver)) {
                                member_exists = true;
                                break;
                            }
                    }
                }
                if (best) {
                    if (!best->is_pub && !best->module_name.empty() &&
                        best->module_name != sem_.current_module()) {
                        sem_.report_error_ex("E0202",
                                              "method '" + m->member + "' of '" + best->owner_class +
                                                  "' is private to module '" + best->module_name + "'",
                                              "mark it `pub fun` (or put it in a `pub { }` block) to call it from another module",
                                              c);
                    }
                    if (best->decl && has_annotation(best->decl, "deprecated"))
                        sem_.report_weak_ex("W0302", "call to deprecated method '" + m->member + "'",
                                            deprecation_hint(best->decl), c->line, c->col);
                    if (best->decl && has_annotation(best->decl, "experimental"))
                        sem_.report_weak_ex("W0304", "call to experimental method '" + m->member + "'",
                                            "API may change before it is frozen", c->line, c->col);
                    bool args_ok = best->params.size() == arg_types.size();
                    if (any_named && best->decl) {
                        std::vector<CType> mapped;
                        std::string why;
                        args_ok = map_named_args(c->args, best->params, best->decl, arg_types, mapped, why);
                        if (!args_ok && why != "no method accepts these named arguments") named_why = why;
                    }
                    if (!args_ok && best->decl && !any_named) {
                        // 缺省实参：后面的形参都有默认值就是合法调用
                        args_ok = true;
                        for (size_t k = arg_types.size(); k < best->params.size(); ++k) {
                            if (k >= best->decl->params.size() || !best->decl->params[k].default_value) {
                                args_ok = false;
                                break;
                            }
                        }
                    }
                    if (!args_ok && !c->args.empty()) {
                        if (any_named)
                            sem_.report_error_ex("E0204", "method " + m->member + ": " + named_why,
                                                  "check the parameter names and provide every required argument", c);
                        else
                            sem_.report_error_ex("E0204",
                                                  "method " + m->member + " requires " +
                                                      std::to_string(best->params.size()) + " argument(s), got " +
                                                      std::to_string(arg_types.size()),
                                                  "check the number of arguments at the call site", c);
                    }
                    if (!best->return_type_name.empty())
                        return type_of_name(best->return_type_name);
                } else if (!member_exists) {
                    // `b.block()`：block 是局部变量，类型为接收者函数（Button.() -> void）。
                    // 编码上它就是 `void (*)(Button*)`，调用时把 b 作为第一个实参。
                    if (const CType *local = lookup_local(m->member);
                        local && local->kind == CType::Fn)
                        return CType::unknown();
                    // 以前这种情况会被原样生成成 `push(x)` —— 丢了接收者的裸调用，
                    // 用户看到的是 gcc 的 `implicit declaration of function 'push'`。
                    // 类类型确定已知、继承链与扩展函数都查过还是没有，就是真错了。
                    sem_.report_error_ex("E0306",
                                          "type `" + ci->name + "` has no member named `" + m->member + "`",
                                          "member calls need a method declared in the class or its bases, "
                                          "or an `ext fun` for that type",
                                          c);
                } else {
                    // 有这个名字，但接收者类型对不上：以前这里静默返回 unknown，
                    // 生成的 C 会把接收者丢掉（`ia.sort()` → `sort()`），
                    // 用户只能在 gcc 的 `implicit declaration` 里迷路。
                    // 现在在语义阶段就说清楚「是哪个类型没有这个方法」。
                    std::string owners_hint;
                    for (auto &f: fit->second) {
                        if (f.owner_class.empty()) continue;
                        if (!owners_hint.empty()) owners_hint += " / ";
                        owners_hint += f.owner_class;
                    }
                    sem_.report_error_ex(
                        "E0306",
                        "type `" + ci->name + "` has no member named `" + m->member +
                            "` (it is declared on `" + owners_hint + "`)",
                        "method calls dispatch on the static type; add the method to this class, "
                        "inherit from the class that declares it, or shadow it with an `ext fun`",
                        c);
                }
            } else if (!obj.sinter_name.empty() && is_builtin_receiver(obj)) {
                // 内建接收者（`str` / `char` / 整数 / 浮点 / `bool`）的扩展方法。
                //
                // 这类接收者没有 ClassInfo，上面的分支整段跳过 —— 于是 `s.length()`
                // 在没 import `std.string` 时会被原样生成成 `length()`（丢了 self），
                // C 阶段才报 `implicit declaration of function 'length'`。这是按需
                // 导入引入后最容易踩的坑，必须在语义分析阶段拦下并告诉用户补哪个 import。
                auto fit = sem_.functions().find(m->member);
                bool member_exists = false;
                const FunctionInfo *best = nullptr;
                // `T?` 解析出来是 `T*`，对可空值调扩展方法要**自动解引用**：
                // `val s: str? = ...; s.length()` 应当等价于 `(*s).length()`。
                //
                // 注意两套名字要分开用：`name` 是 C 侧名（`sinter_string*`），
                // `sinter_name` 是 Sinter 侧名（`str*`）。`ext_receiver` 记的是
                // Sinter 侧名，所以去星后要和 `sinter_name` 对得上，不是 `name`。
                std::string recv_name = obj.sinter_name;
                if (obj.kind == CType::Ptr) {
                    std::string ci_name = obj.sinter_name;
                    while (!ci_name.empty() && ci_name.back() == '*') ci_name.pop_back();
                    if (!ci_name.empty()) recv_name = ci_name;
                }
                if (fit != sem_.functions().end()) {
                    for (auto &f: fit->second) {
                        if (f.ext_receiver.empty()) continue;
                        if (f.ext_receiver != recv_name) continue;
                        member_exists = true;
                        if (f.params.size() == arg_types.size()) {
                            best = &f;
                            break;
                        }
                        if (!best) best = &f;
                    }
                }
                if (best) {
                    if (!best->return_type_name.empty()) return type_of_name(best->return_type_name);
                } else if (!member_exists) {
                    sem_.report_error_ex("E0306",
                                          "type `" + obj.sinter_name + "` has no member named `" + m->member +
                                              "`",
                                          "no `ext fun " + obj.sinter_name + "." + m->member +
                                              "` is in scope; if it comes from the standard library, "
                                              "add the matching import (`std.string` for `str` helpers)",
                                          c);
                }
            }
            return CType::unknown();
        }

        return CType::unknown();
    }

    void TypeChecker::stmt(Statement *s) {
        if (!s) return;
        if (depth_ > 200) return;
        DepthGuard guard(depth_);

        if (auto *b = dynamic_cast<BlockStmt *>(s)) {
            push_scope();
            bool terminated = false;
            for (auto &x: b->statements) {
                if (terminated) {
                    sem_.report_weak_ex("W0301",
                                        "unreachable statement after a control-flow terminator",
                                        "remove it, or move it before return/break/continue",
                                        x->line, x->col);
                    terminated = false;   // 一条就够，别对整段展开刷屏
                }
                stmt(x.get());
                if (dynamic_cast<ReturnStmt *>(x.get()) || dynamic_cast<BreakStmt *>(x.get()) ||
                    dynamic_cast<ContinueStmt *>(x.get()))
                    terminated = true;
            }
            pop_scope();
            return;
        }
        if (auto *v = dynamic_cast<VarDeclStmt *>(s)) {
            if (v->type && v->type->is_ref) {
                bool is_null = false;
                if (auto *lit = dynamic_cast<LiteralExpr *>(v->init.get()))
                    is_null = lit->lit == LiteralExpr::Null;
                if (!v->init || is_null) {
                    sem_.report_error_ex("E0301",
                                         "reference '" + v->name + "' must be initialized with `&value`",
                                         "a T& reference is non-null and cannot be rebound", v->line, v->col);
                }
            }
            CType decl_t = v->type ? sem_.resolve_type(v->type.get()) : CType::unknown();
            CType init_t = v->init ? infer(v->init.get()) : CType::unknown();
            if (v->type && v->init) {
                require_assignable(init_t, decl_t, v->init.get(),
                                   "variable " + v->name + " initializer type mismatch");
            }
            declare(v->name, decl_t.kind != CType::Unknown ? decl_t : init_t);
            return;
        }
        if (auto *d = dynamic_cast<DestructureStmt *>(s)) {
            infer(d->init.get());
            return;
        }
        if (auto *th = dynamic_cast<ThrowStmt *>(s)) {
            CType t = infer(th->value.get());
            if (t.kind != CType::Unknown && t.kind != CType::Str && t.kind != CType::Char)
                sem_.report_error_ex("E0301", "throw expects a string message",
                                     "wrap the value with `${...}` or toText()", th->line, th->col);
            return;
        }
        if (auto *tr = dynamic_cast<TryStmt *>(s)) {
            stmt(tr->body.get());
            push_scope();
            if (!tr->error_name.empty()) declare(tr->error_name, CType::make_str());
            if (tr->catch_body) stmt(tr->catch_body.get());
            pop_scope();
            if (tr->finally_body) stmt(tr->finally_body.get());
            return;
        }
        if (auto *r = dynamic_cast<ReturnStmt *>(s)) {
            CType t = infer(r->value.get());
            if (return_type_.kind != CType::Unknown && t.kind != CType::Unknown &&
                return_type_.kind != CType::Void) {
                require_assignable(t, return_type_, r->value.get(), "return type mismatch", "E0303");
            }
            return;
        }
        if (auto *i = dynamic_cast<IfStmt *>(s)) {
            infer(i->cond.get());
            stmt(i->then_b.get());
            stmt(i->else_b.get());
            return;
        }
        if (auto *w = dynamic_cast<WhileStmt *>(s)) {
            infer(w->cond.get());
            stmt(w->body.get());
            return;
        }
        if (auto *f = dynamic_cast<ForStmt *>(s)) {
            // 降序区间：`for (x in 3..1)` 生成的是 `for (v = 3; v <= 1; ++v)`，
            // 一次都不执行 —— 用户看到的是「循环体凭空消失」，最难查的一类错。
            // Sinter 只支持升序区间，这里对「两端都是整数字面量」的情况直接报错，
            // 而不是静默产出空循环。「下界/上界是变量」无法静态判定，留给运行期。
            if (auto *r = dynamic_cast<RangeExpr *>(f->iterable.get())) {
                const auto *lo = dynamic_cast<LiteralExpr *>(r->start.get());
                const auto *hi = dynamic_cast<LiteralExpr *>(r->end.get());
                if (lo && hi && lo->lit == LiteralExpr::Int && hi->lit == LiteralExpr::Int) {
                    const long long lv = std::get<int64_t>(lo->value);
                    const long long hv = std::get<int64_t>(hi->value);
                    if (lv > hv) {
                        sem_.report_error_ex(
                            "E0307",
                            "descending range `" + std::to_string(lv) + ".." + std::to_string(hv) +
                                "` is not supported; `for (x in a..b)` only counts upward",
                            "range loops are ascending only — rewrite the countdown as "
                            "`for (i in 0..<n) { val x = hi - i ... }`",
                            static_cast<Expression *>(r));
                    }
                }
            }
            infer(f->iterable.get());
            push_scope();
            stmt(f->body.get());
            pop_scope();
            return;
        }
        if (auto *dw = dynamic_cast<DoWhileStmt *>(s)) {
            stmt(dw->body.get());
            infer(dw->cond.get());
            return;
        }
        if (auto *ex = dynamic_cast<ExprStmt *>(s)) {
            if (auto *call = dynamic_cast<CallExpr *>(ex->expr.get())) {
                std::string name;
                if (auto *id = dynamic_cast<IdentifierExpr *>(call->callee.get())) name = id->name;
                else if (auto *m = dynamic_cast<MemberAccessExpr *>(call->callee.get())) name = m->member;
                if (!name.empty()) {
                    if (auto fit = sem_.functions().find(name); fit != sem_.functions().end()) {
                        for (auto &f: fit->second) {
                            if (has_annotation(f.decl, "must_use")) {
                                sem_.report_weak_ex("W0303",
                                                    "return value of @must_use function '" + name +
                                                        "' is ignored",
                                                    "assign it, return it, or wrap it in a statement",
                                                    call->line, call->col);
                                break;
                            }
                        }
                    }
                }
            }
            infer(ex->expr.get());
            return;
        }
    }

    void TypeChecker::function_body(FunctionDecl *f) {
        if (f) sem_.set_scope_function(f->name);   // 诊断带上当前函数名
        return_type_ = f->return_type ? sem_.resolve_type(f->return_type.get()) : CType::unknown();
        push_scope();
        for (auto &p: f->params) {
            CType t = p.type ? sem_.resolve_type(p.type.get()) : CType::unknown();
            declare(p.name, t);
        }
        if (f->is_variadic && !f->variadic_name.empty()) {
            CType t = f->variadic_elem_type ? sem_.resolve_type(f->variadic_elem_type.get())
                                            : CType::unknown();
            declare(f->variadic_name, t);
        }
        // 方法体里的 this
        if (current_class_) {
            CType self;
            self.kind = CType::Struct;
            self.name = "struct " + current_class_->c_name + "*";
            self.sinter_name = current_class_->name;
            declare("this", self);
            declare("self", self);
        }
        stmt(f->body.get());
        pop_scope();
    }

    void TypeChecker::check_program(Program &p) {
        std::function<void(Statement *)> walk = [&](Statement *s) {
            if (!s) return;
            if (auto *f = dynamic_cast<FunctionDecl *>(s)) {
                const ClassInfo *saved = current_class_;
                current_class_ = nullptr;
                function_body(f);
                current_class_ = saved;
                return;
            }
            if (auto *c = dynamic_cast<ClassDecl *>(s)) {
                const ClassInfo *saved = current_class_;
                current_class_ = find_class(c->name);
                for (auto &m: c->members) walk(m.get());
                if (c->destructor) walk(c->destructor.get());
                current_class_ = saved;
                return;
            }
            if (auto *b = dynamic_cast<BlockStmt *>(s)) {
                for (auto &x: b->statements) walk(x.get());
                return;
            }
            if (auto *pb = dynamic_cast<PubBlockStmt *>(s)) {
                for (auto &m: pb->statements) walk(m.get());
                return;
            }
            if (auto *ms = dynamic_cast<ModuleStmt *>(s)) {
                if (ms->is_block) for (auto &m: ms->body) walk(m.get());
                return;
            }
        };
        for (auto &s: p.statements) walk(s.get());
    }
} // namespace sinter
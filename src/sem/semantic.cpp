// sinter semantic implementation
#include "sem/semantic.hpp"
#include <cctype>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <functional>
#include <set>

namespace sinter {
    namespace {
        /// 形参 C 名：统一 snake_case，并在同一形参表内去重
        /// （`someValue` 与 `some_value` 并存时，后者按位次加后缀）。
        void assign_param_c_names(std::vector<ParamInfo> &params) {
            std::set<std::string> used;
            for (size_t i = 0; i < params.size(); ++i) {
                ParamInfo &p = params[i];
                std::string n = Semantic::to_snake_ident(p.name);
                if (used.contains(n)) n = n + "_" + std::to_string(i + 1);
                while (used.contains(n)) n += "_";
                used.insert(n);
                p.c_name = n;
            }
        }
    }
    // ==========================================================================
    // CType
    // ==========================================================================
    std::string CType::to_c() const {
        switch (kind) {
            case Void: return "void";
            case Bool: return "long long";
            case Str: return "sinter_string";
            case Char: return "char";
            case Float: return bits == 32 ? "float" : "double";
            case Int: {
                if (bits == 8) return is_signed ? "int8_t" : "uint8_t";
                if (bits == 16) return is_signed ? "int16_t" : "uint16_t";
                if (bits == 32) return is_signed ? "int32_t" : "uint32_t";
                if (bits == 64) return is_signed ? "int64_t" : "uint64_t";
                return "long long";
            }
            case Ptr:
            case Struct:
            case Enum:
            case Fn:
            default:
                return name.empty() ? "long long" : name;
        }
    }

    CType CType::make_int(int bits, bool sgn, const std::string &sinter) {
        CType t;
        t.kind = Int;
        t.bits = bits;
        t.is_signed = sgn;
        t.sinter_name = sinter;
        return t;
    }
    CType CType::make_void() { CType t; t.kind = Void; t.sinter_name = "void"; return t; }
    CType CType::make_bool() { CType t; t.kind = Bool; t.bits = 8; t.name = "uint8_t"; t.sinter_name = "bool"; return t; }
    CType CType::make_str() { CType t; t.kind = Str; t.sinter_name = "str"; return t; }
    CType CType::make_double() { CType t; t.kind = Float; t.bits = 64; t.sinter_name = "f64"; return t; }
    CType CType::make_float() { CType t; t.kind = Float; t.bits = 32; t.sinter_name = "f32"; return t; }
    CType CType::unknown() { CType t; t.kind = Unknown; return t; }

    std::string FunctionInfo::signature() const {
        // 用 Sinter 侧类型名（i32 / i64 / str …）拼签名，保证 C 标识符简短且稳定
        std::string s;
        // 扩展函数把接收者算进签名：`ext fun i64.twice()` 与 `fun twice(x: i64)`
        // 的 C 形参表都是 `(int64_t)`，但调用形式完全不同（`5.twice()` / `twice(5)`），
        // 属于合法重载。不带接收者前缀时两者签名相同，会被误判成重复定义。
        if (!ext_receiver.empty()) s += Semantic::to_snake_ident(ext_receiver) + ".";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) s += ",";
            const std::string &t = params[i].type_sinter.empty() ? params[i].type_c
                                                                 : params[i].type_sinter;
            s += Semantic::to_snake_ident(t);
        }
        // 可变参数以 `...` 结尾：`fun f(xs: i64...)` 与 `fun f(x: i64)` 是不同重载
        if (is_varargs) s += "...";
        return s;
    }

    // ==========================================================================
    // 构造
    // ==========================================================================
    Semantic::Semantic() { register_builtins(); }

    std::string Semantic::to_snake_ident(const std::string &s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (size_t i = 0; i < s.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '.' || c == ':' || c == '-' || c == ' ') {
                if (!out.empty() && out.back() != '_') out += '_';
                continue;
            }
            if (std::isupper(c)) {
                const bool prev_lower = i > 0 &&
                    (std::islower(static_cast<unsigned char>(s[i - 1])) ||
                     std::isdigit(static_cast<unsigned char>(s[i - 1])));
                // HTTPServer -> http_server：连续大写后跟小写时，在最后一个大写前断开
                const bool acronym = i > 0 && std::isupper(static_cast<unsigned char>(s[i - 1])) &&
                    i + 1 < s.size() && std::islower(static_cast<unsigned char>(s[i + 1]));
                if ((prev_lower || acronym) && !out.empty() && out.back() != '_') out += '_';
                out += static_cast<char>(std::tolower(c));
            } else {
                out += static_cast<char>(c);
            }
        }
        if (out.empty()) out = "anon";
        return out;
    }

    std::string Semantic::param_decl(const std::string &type_c, const std::string &name) {
        // 函数指针类型：`<ret> (*)(<args>)` → `<ret> (*<name>)(<args>)`。
        // 名字必须插在 `(*` 与 `)` 之间，这是 C 声明的固定语法；
        // 直接拼成 `<ret> (*)(<args>) <name>` 会被 gcc 判成语法错误。
        const std::string marker = "(*)(";
        const size_t at = type_c.find(marker);
        if (at != std::string::npos) {
            return type_c.substr(0, at) + "(*" + name + ")" + type_c.substr(at + marker.size() - 1);
        }
        return type_c + " " + name;
    }

    std::string Semantic::module_to_ident(const std::string &m) const {
        std::string r;
        size_t start = 0;
        while (start <= m.size()) {
            const size_t dot = m.find('.', start);
            const std::string part = m.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!part.empty()) {
                if (!r.empty()) r += '_';
                r += to_snake_ident(part);
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (r.empty()) r = "anon";
        return r;
    }

    std::string Semantic::sanitize(const std::string &t) {
        std::string r;
        for (char ch: t) {
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') r += ch;
            else if (ch == '*') r += "_ptr";
            // 可变参数的 `...` 后缀：转成合法标识符片段
            else if (ch == '.') r += "_va";
            // 逗号/空格/括号/尖括号等一律压成一个下划线，保证 C 标识符合法。
            // 函数类型形参 `(i64) -> i64` 以前会把 `(*)` 原样带进构造函数名，
            // 生成 `thread__void_ptr_(_ptr)(void_ptr)_ptr` 这种非法 C 名。
            else r += "_";
        }
        return r;
    }

    // 运算符符号 → C 标识符片段：`+` → `op_add`、`==` → `op_eq` …
    // 未知运算符按字符转义，保证永远是合法 C 标识符。
    static std::string op_c_part(const std::string &name) {
        if (name.rfind("operator", 0) != 0) return name;
        const std::string sym = name.substr(8);
        static const struct { const char *s; const char *to; } tbl[] = {
            {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "mod"},
            {"==", "eq"}, {"!=", "ne"}, {"<", "lt"}, {"<=", "le"}, {">", "gt"}, {">=", "ge"},
            {"[]", "index"}, {"()", "call"}, {"!", "not"}, {"&", "band"}, {"|", "bor"},
        };
        for (auto &e: tbl) if (sym == e.s) return std::string("op_") + e.to;
        std::string r = "op";
        for (char ch: sym) {
            if (std::isalnum(static_cast<unsigned char>(ch))) r += ch;
            else r += "_" + std::to_string(static_cast<int>(ch)) + "_";
        }
        return r;
    }

    /// 生成 C 的函数名：`<模块>_<函数>`，**不再拼参数类型**。
    /// 重名由 `register_function` 在语义层拒绝（要求 `fun f[link]` 别名），
    /// 因此这里不会撞名；签名后缀只是把 C 名字变长、随类型漂移，没有额外价值。
    std::string Semantic::mangled_name(const std::string &name, const std::string &sig) const {
        (void) sig;
        const std::string fn = name.rfind("operator", 0) == 0
                                   ? op_c_part(name)
                                   : to_snake_ident(name);
        return cur_module_ident_ + "_" + fn;
    }

    // ==========================================================================
    // 内建类型
    //  number 是所有整数的统称：number -> i8, i16, i32, i64 ...
    // ==========================================================================
    void Semantic::register_builtins() {
        // 整数宽度
        const struct { const char *n; int bits; bool sgn; } ints[] = {
            {"i8", 8, true}, {"i16", 16, true}, {"i32", 32, true}, {"i64", 64, true}, {"i128", 128, true},
            {"u8", 8, false}, {"u16", 16, false}, {"u32", 32, false}, {"u64", 64, false}, {"u128", 128, false},
        };
        for (auto &it: ints) {
            aliases_[it.n] = CType::make_int(it.bits, it.sgn, it.n).to_c();
            numeric_types_.insert(it.n);
        }
        // 兼容名
        aliases_["int"] = "int32_t";
        aliases_["long"] = "int64_t";
        aliases_["short"] = "int16_t";
        aliases_["byte"] = "uint8_t";
        numeric_types_.insert("int");
        numeric_types_.insert("long");
        numeric_types_.insert("short");
        numeric_types_.insert("byte");

        aliases_["f32"] = "float";
        aliases_["f64"] = "double";
        aliases_["float"] = "double";
        aliases_["double"] = "double";
        numeric_types_.insert("f32");
        numeric_types_.insert("f64");
        numeric_types_.insert("float");
        numeric_types_.insert("double");

        // number 是整数统称，C 侧用最宽整数承载
        aliases_["number"] = "int64_t";
        numeric_types_.insert("number");

        aliases_["str"] = "sinter_string";      // 内部字符串（带长度）
        aliases_["string"] = "sinter_string";
        aliases_["cstr"] = "const char*";      // 兼容 C 的裸字符串
        // `ptr`：无类型指针（void*）。C 头里的 `void*` 参数映射到它，
        // 也比「硬把 FILE* 塞进 cstr」更诚实。
        aliases_["ptr"] = "void*";
        aliases_["bool"] = "uint8_t";
        aliases_["void"] = "void";
        aliases_["char"] = "char";
    }

    std::string Semantic::type_to_c(const CType &t) const { return t.to_c(); }

    std::string Semantic::type_to_c(const TypeAnnotation *t) const {
        if (!t) return "long long";
        if (t->is_ref) {
            if (t->kind == TypeAnnotation::Named) {
                const auto cit = classes_.find(t->name);
                if (cit != classes_.end()) return "struct " + cit->second.c_name + "*";
                const auto xcit = external_classes_.find(t->name);
                if (xcit != external_classes_.end() && !xcit->second.c_name.empty())
                    return xcit->second.c_name + "*";
                const auto eit = enums_.find(t->name);
                if (eit != enums_.end()) return eit->second.c_name + "*";
                return type_to_c_no_ref(t) + "*";
            }
            if (t->kind == TypeAnnotation::Pointer)
                return type_to_c(t->inner_type.get()) + "**";
            return type_to_c_no_ref(t) + "*";
        }
        return type_to_c_no_ref(t);
    }

    std::string Semantic::type_to_c_no_ref(const TypeAnnotation *t) const {
        if (!t) return "long long";
        switch (t->kind) {
            case TypeAnnotation::Named: {
                const auto it = aliases_.find(t->name);
                if (it != aliases_.end()) return it->second;
                const auto cit = classes_.find(t->name);
                if (cit != classes_.end()) return "struct " + cit->second.c_name + "*";
                const auto xcit = external_classes_.find(t->name);
                if (xcit != external_classes_.end() && !xcit->second.c_name.empty()) {
                    // 不透明 C 类型：c_name 已经是 C 侧完整拼写（`struct Foo` / `FILE`），
                    // 不能再加 struct 前缀，否则 `typedef struct _IO_FILE FILE` 会被写成
                    // `struct FILE*`（C 侧直接编译失败）。
                    if (xcit->second.opaque) return xcit->second.c_name + "*";
                    return "struct " + xcit->second.c_name + "*";
                }
                const auto eit = enums_.find(t->name);
                if (eit != enums_.end()) return eit->second.c_name;
                return t->name;
            }
            case TypeAnnotation::Pointer: {
                auto *inner = t->inner_type.get();
                // `T&&` 对类/结构体不加星：类值本身就是 `struct X*`，引用与它同构。
                if (t->is_rvalue_ref && inner && inner->kind == TypeAnnotation::Named &&
                    (classes_.contains(inner->name) || external_classes_.contains(inner->name)))
                    return type_to_c(inner);
                if (inner && inner->kind == TypeAnnotation::Named) {
                    if (inner->name == "void") return "void*";
                    if (inner->name == "char") return "char*";
                }
                return type_to_c(inner) + "*";
            }
            case TypeAnnotation::Array:
                return type_to_c(t->inner_type.get()) + "*";
            case TypeAnnotation::Optional: {
                // `T?` 对已经是指针的类型不额外加星（避免 Animal** 这种错误）
                std::string inner = type_to_c(t->inner_type.get());
                if (!inner.empty() && inner.back() == '*') return inner;
                return inner + "*";
            }
            case TypeAnnotation::Generic:
                return "void*";
            case TypeAnnotation::FunctionType:
                // 函数类型 `(i64) -> i64` → C 函数指针 `int64_t (*)(int64_t)`。
                // 以前这里返回 `void*`（类型擦除），形参会生成 `void* f`，
                // 函数体里的 `f(x)` 到 C 就成了「called object is not a function」。
                return resolve_type(t).to_c();
            default:
                return "void*";
        }
    }

    CType Semantic::resolve_type_name(const std::string &name) const {
        // 整数
        if (name == "i8") return CType::make_int(8, true, "i8");
        if (name == "i16") return CType::make_int(16, true, "i16");
        if (name == "i32" || name == "int") return CType::make_int(32, true, "i32");
        if (name == "i64" || name == "long") return CType::make_int(64, true, "i64");
        if (name == "i128") return CType::make_int(128, true, "i128");
        if (name == "u8" || name == "byte") return CType::make_int(8, false, "u8");
        if (name == "u16" || name == "short") return CType::make_int(16, false, "u16");
        if (name == "u32") return CType::make_int(32, false, "u32");
        if (name == "u64") return CType::make_int(64, false, "u64");
        if (name == "u128") return CType::make_int(128, false, "u128");
        if (name == "number") return CType::make_int(64, true, "number");
        if (name == "f32") return CType::make_float();
        if (name == "f64" || name == "float" || name == "double") return CType::make_double();
        if (name == "bool") return CType::make_bool();
        if (name == "str" || name == "string") return CType::make_str();
        if (name == "cstr") {
            CType t;
            t.kind = CType::Ptr;
            t.name = "const char*";
            t.sinter_name = "cstr";
            return t;
        }
        if (name == "ptr") {
            CType t;
            t.kind = CType::Ptr;
            t.name = "void*";
            t.sinter_name = "ptr";
            return t;
        }
        if (name == "char") { CType t; t.kind = CType::Char; t.sinter_name = "char"; return t; }
        if (name == "void") return CType::make_void();

        if (const auto it = classes_.find(name); it != classes_.end()) {
            CType t;
            t.kind = CType::Struct;
            t.name = "struct " + it->second.c_name + "*";
            t.sinter_name = name;
            return t;
        }
        if (const auto it = enums_.find(name); it != enums_.end()) {
            CType t;
            t.kind = CType::Enum;
            t.name = it->second.c_name;
            t.sinter_name = name;
            return t;
        }
        // 来自 `bind C header` 的不透明 C 结构体：类型本身就是指针
        if (const auto it = external_classes_.find(name); it != external_classes_.end()) {
            CType t;
            t.kind = CType::Struct;
            t.name = it->second.c_name + "*";
            t.sinter_name = name;
            return t;
        }
        if (const auto it = aliases_.find(name); it != aliases_.end()) {
            CType t;
            t.name = it->second;
            t.sinter_name = name;
            t.kind = CType::Ptr;
            return t;
        }
        CType t;
        t.name = name;
        t.sinter_name = name;
        return t;
    }

    CType Semantic::resolve_type(const TypeAnnotation *t) const {
        if (!t) return CType::unknown();
        if (t->is_ref) {
            if (t->kind == TypeAnnotation::Named) {
                CType base = resolve_type_name(t->name);
                if (base.kind == CType::Struct) { base.is_ref = true; return base; }
                CType r;
                r.kind = CType::Ptr;
                r.name = base.to_c() + "*";
                r.sinter_name = base.sinter_name + "&";
                r.is_ref = true;
                return r;
            }
            if (t->kind == TypeAnnotation::Pointer) {
                CType inner = resolve_type(t->inner_type.get());
                CType r;
                r.kind = CType::Ptr;
                r.name = inner.to_c() + "**";
                r.sinter_name = inner.sinter_name + "&&";
                r.is_ref = true;
                return r;
            }
            return CType::unknown();
        }
        switch (t->kind) {
            case TypeAnnotation::Named:
                return resolve_type_name(t->name);
            case TypeAnnotation::FunctionType: {
                // 函数类型 `(i64) -> i64`：生成 C 的函数指针类型 `int64_t (*)(int64_t)`。
                // 以前落到 default 分支擦成 `void*`，形参就变成 `void* f`，
                // 函数体里的 `f(x)` 到了 C 就是「called object is not a function」。
                std::string ret = resolve_type(t->inner_type.get()).to_c();
                if (ret.empty()) ret = "void";
                std::string sig = ret + " (*)(";
                bool any = false;
                if (t->receiver_type) {
                    std::string rt = resolve_type(t->receiver_type.get()).to_c();
                    if (rt.empty()) rt = "void*";
                    sig += rt;
                    any = true;
                }
                for (size_t i = 0; i < t->generic_args.size(); ++i) {
                    if (any) sig += ", ";
                    std::string pt = resolve_type(t->generic_args[i].get()).to_c();
                    if (pt.empty()) pt = "void*";
                    sig += pt;
                    any = true;
                }
                if (!any) sig += "void";
                sig += ")";
                CType r;
                r.kind = CType::Fn;
                r.name = sig;
                r.sinter_name = "fn";
                return r;
            }
            case TypeAnnotation::Pointer:
            case TypeAnnotation::Array:
            case TypeAnnotation::Optional: {
                CType inner = resolve_type(t->inner_type.get());
                // `T&&` / `T&` 对类/结构体不额外加星：Sinter 的类值本身就是 `struct X*`，
                // 引用与它同构。否则 `operator=(other: T&&)` 会拿到 `struct X**`。
                if (t->kind == TypeAnnotation::Pointer && t->is_rvalue_ref && inner.kind == CType::Ptr)
                    return inner;
                CType r;
                r.kind = CType::Ptr;
                r.name = inner.to_c() + "*";
                r.sinter_name = inner.sinter_name + "*";
                r.nullable = (t->kind == TypeAnnotation::Optional);
                return r;
            }
            default: {
                CType r;
                r.kind = CType::Ptr;
                r.name = "void*";
                return r;
            }
        }
    }

    // ==========================================================================
    // 数值提升 / 可赋值性
    // ==========================================================================
    static int rank_of(const CType &t) {
        if (t.kind == CType::Float) return 1000 + t.bits;
        if (t.kind == CType::Int) return t.bits;
        return 0;
    }

    CType Semantic::promote(const CType &a, const CType &b) {
        if (a.kind == CType::Float || b.kind == CType::Float) {
            return (a.bits >= 64 || b.bits >= 64) ? CType::make_double() : CType::make_float();
        }
        if (a.kind == CType::Int && b.kind == CType::Int) {
            const int bits = std::max(a.bits, b.bits);
            const bool sgn = a.is_signed && b.is_signed;
            return CType::make_int(bits, sgn, "i" + std::to_string(bits));
        }
        return a;
    }

    bool Semantic::is_assignable(const CType &from, const CType &to, bool strict) {
        if (to.kind == CType::Unknown || from.kind == CType::Unknown) return true;
        if (to.kind == CType::Void) return true;
        if (from.kind == to.kind && from.name == to.name) return true;
        // 数字之间：完全信任用户操作，仅宽度不足时告警，不拦截
        if (from.is_numeric() && to.is_numeric()) {
            if (strict && rank_of(from) > rank_of(to)) return false;
            return true;
        }
        // bool ← 数字
        if (to.kind == CType::Bool && from.is_numeric()) return true;
        // 任何指针可赋给 void*
        if (to.kind == CType::Ptr && to.name == "void*") return true;
        // 可空
        if (to.nullable) return true;
        // struct 相同 sinter 名字
        if (!to.sinter_name.empty() && to.sinter_name == from.sinter_name) return true;
        return from.to_c() == to.to_c();
    }

    bool Semantic::check_constraint(const std::string &constraint, const CType &arg) const {
        if (constraint == "number") {
            // number 只能接受整数（用户要求：number 是所有整数的统称）
            return arg.kind == CType::Int;
        }
        if (constraint == "any") return true;
        const CType want = resolve_type_name(constraint);
        if (want.kind == CType::Int && arg.kind == CType::Int) {
            return want.bits == arg.bits && want.is_signed == arg.is_signed;
        }
        if (want.kind == CType::Float && arg.kind == CType::Float) return true;
        return is_assignable(arg, want);
    }

    // ==========================================================================
    // 诊断
    // ==========================================================================
    std::string Semantic::scope_text() const {
        std::string s = "module " + (current_module_.empty() ? cur_module_ident_ : current_module_);
        if (!scope_type_.empty()) {
            s += " \u203a " + (scope_kind_.empty() ? std::string("type") : scope_kind_) + " " + scope_type_;
        }
        if (!scope_fn_.empty()) s += " \u203a fun " + scope_fn_;
        return s;
    }

    void Semantic::report_error(const std::string &m) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.message = m;
        d.file = current_file_.empty() ? cur_module_ident_ : current_file_;
        d.scope = scope_text();
        diags_.push_back(std::move(d));
    }
    void Semantic::report_warn(const std::string &m) {
        diags_.push_back({Severity::Warning, m,
                          current_file_.empty() ? cur_module_ident_ : current_file_, 0, 0, 0});
    }

    void Semantic::report_weak_ex(const std::string &code, const std::string &m,
                                  const std::string &hint, int line, int col) {
        Diagnostic d;
        d.severity = Severity::WeakWarning;
        d.code = code;
        d.message = m;
        d.hint = hint;
        d.file = current_file_.empty() ? cur_module_ident_ : current_file_;
        d.line = line;
        d.col = col;
        diags_.push_back(std::move(d));
    }
    bool Semantic::has_errors() const {
        for (auto &d: diags_) if (d.severity == Severity::Error) return true;
        return false;
    }

    // ==========================================================================
    // 收集声明
    // ==========================================================================
    void Semantic::register_function(FunctionDecl *f, const std::string &owner_class, bool is_method) {
        scope_fn_ = f->name;   // 诊断里带上函数/方法名
        // extern fun：实现 C 端已声明的函数。命名（alias_name，不带模块前缀）与
        // 「不导出头文件」（is_pub=false）已经在 parser 里定好；这里只做
        // 「这个签名能不能上 C ABI」的检查，避免生成 C 之后才报 implicit declaration。
        if (f->is_extern_c) {
            if (!f->body) {
                report_error_ex("E0701", "extern function '" + f->name +
                                             "' needs a body: this form implements a C declaration, "
                                             "it does not re-declare it",
                                 "add a body, or declare a C function with `bind C` instead",
                                 f->line, f->col);
            }
            if (!f->tparams.empty()) {
                report_error_ex("E0702",
                                 "extern function '" + f->name +
                                     "' cannot be generic: C has no template instantiation",
                                 "write a concrete function, or wrap the generic one in Sinter code",
                                 f->line, f->col);
            }
            if (f->is_variadic) {
                report_error_ex("E0703", "extern function '" + f->name +
                                             "' cannot use Sinter variadics",
                                 "declare a true C variadic function with `bind C`, and implement "
                                 "it in C",
                                 f->line, f->col);
            }
            for (auto &prm: f->params) {
                if (prm.type && prm.type->kind == TypeAnnotation::Named && prm.type->name == "str") {
                    report_error_ex("E0704", "extern function parameter '" + prm.name +
                                                 "' cannot be 'str': C has no sinter_string ABI",
                                     "use 'cstr' (const char*) and convert inside the body",
                                     f->line, f->col);
                }
            }
            if (f->return_type && f->return_type->kind == TypeAnnotation::Named &&
                f->return_type->name == "str") {
                report_error_ex("E0704", "extern function '" + f->name +
                                             "' cannot return 'str': C has no sinter_string ABI",
                                 "return 'cstr', or fill a caller-provided buffer",
                                 f->line, f->col);
            }
        }
        // 函数别名：fun f2 = f1 — 直接登记为目标函数的别名项
        FunctionInfo fi;
        fi.name = f->name;
        fi.module_name = current_module_;
        fi.decl = f;
        fi.is_pub = f->is_pub;
        fi.is_inline = f->is_inline;
        fi.is_static = f->is_static;
        fi.is_method = is_method;
        fi.owner_class = owner_class;
        fi.is_virtual = f->is_override || (is_method && !f->is_static && !owner_class.empty());
        fi.ext_receiver = f->ext_receiver;

        for (auto &prm: f->params) {
            ParamInfo pi;
            pi.name = prm.name;
            // 类型参数包在语义层还没有具体类型（要等调用点代入），
            // 若原样保留名字会生成非法 C 标识符（如 `Args xs`）。
            // 这里统一退化为 `long long`，代码生成侧再按包内第一个实参类型细化。
            bool is_pack_type = false;
            if (prm.type && prm.type->pack_index >= 0) is_pack_type = true;
            if (prm.type && prm.type->kind == TypeAnnotation::Named && !f->tparams.empty()) {
                for (auto &tp: f->tparams)
                    if (tp.is_pack && tp.name == prm.type->name) is_pack_type = true;
            }
            pi.type_c = is_pack_type ? std::string("long long")
                                     : (prm.type ? type_to_c(prm.type.get()) : "long long");
            pi.type_sinter = prm.type ? (prm.type->name.empty() ? pi.type_c : prm.type->name) : "i64";
            if (is_pack_type) pi.type_sinter = "i64";
            fi.params.push_back(std::move(pi));
        }
        assign_param_c_names(fi.params);
        // 可变参数：把末位形参的元素类型记为 `T*`（调用点传入的是元素数组）。
        // 签名（signature()）会额外带上 `...`，用于和定参重载区分；type_sinter
        // 保持原样，不能污染 C 标识符。
        if (f->is_variadic && !fi.params.empty()) {
            ParamInfo &last = fi.params.back();
            last.type_c = (last.type_c.empty() ? std::string("long long") : last.type_c) + "*";
        }
        fi.return_type_name = f->return_type ? type_to_c(f->return_type.get()) : "void";
        // 返回类型是类型参数包索引（`-> Args[0]`）时语义层还不知道具体类型，
        // 先退化为 long long，代码生成侧按 subst 细化。
        if (f->return_type && f->return_type->pack_index >= 0)
            fi.return_type_name = "long long";
        // 可变参数：末位形参接收全部剩余实参，签名用 `...` 后缀区分于定参重载。
        fi.is_varargs = f->is_variadic;

        // 无参数列表的同名分支（fun square : i64 = ...）：
        // 沿用同模块同名首个重载的形参表，使其成为同签名、仅返回类型不同的重载。
        if (f->params.empty() && !is_method) {
            if (const auto hit = functions_.find(f->name); hit != functions_.end()) {
                for (auto &prev: hit->second) {
                    if (prev.module_name != current_module_) continue;
                    if (prev.params.empty()) continue;
                    if (!prev.template_params.empty()) continue;
                    fi.params = prev.params;
                    break;
                }
            }
        }

        for (auto &tp: f->tparams) {
            fi.template_params.push_back(tp.name);
            fi.template_constraints.push_back(tp.constraint ? tp.constraint->name : "");
        }

        const std::string sig = fi.signature();
        // 方法命名：<模块>_<类>_<方法>；普通函数：<模块>_<函数>。
        // `fun name[alias]` 的 `[]` 是 **Sinter 别名**，不是 C 名：C 符号仍由编译器
        // 生成（带签名与别名片段），保证唯一且不把 C 布局暴露给用户。
        const std::string method_part = f->name.rfind("operator", 0) == 0
                                            ? op_c_part(f->name)
                                            : to_snake_ident(f->name);
        std::string base_name;
        if (is_method) {
            base_name = cur_module_ident_ + "_" + to_snake_ident(owner_class) + "_" + method_part;
        } else {
            base_name = mangled_name(f->name, sig);
        }
        // 构造函数是唯一允许「同名不同参」的成员（`String(str)` / `String(cstr)` 都得叫
        // `String`），因此只有它们保留签名后缀；其余函数/方法同名一律由下面的唯一性
        // 检查拒绝，不会产生第二个签名变体。
        // 还有一类必须带签名：类名与函数名撞车。`class IntArray` 会生成
        // `typedef struct {...} std_arrays_int_array;`，若再有一个同名函数
        // （构造入口 `IntArray(size, init)` 就是），C 的普通标识符命名空间里
        // 就重复了。加签名后缀既保留可读性，也保证符号唯一。
        bool collides_with_type = false;
        for (const auto &kv: classes_) {
            if (kv.second.c_name == base_name) {
                collides_with_type = true;   // 生成的符号与某个类的 typedef 名相同
                break;
            }
        }
        const bool ctor_needs_sig = is_method && f->name == owner_class;
        // 带别名的函数通常是重载：签名 + 别名片段一起进 C 名，避免两个别名
        // 生成同一个符号。别名的「可调用名」在下面单独登记。
        const bool alias_needs_sig = !f->alias_name.empty() && !sig.empty();
        const bool needs_sig = !sig.empty() && (ctor_needs_sig || collides_with_type || alias_needs_sig);
        if (f->is_extern_c) {
            // extern fun：实现 C 头里的函数，符号名必须就是 C 名字。
            fi.c_name = f->name;
        } else {
            fi.c_name = needs_sig ? base_name + "__" + sanitize(sig) : base_name;
            if (!f->alias_name.empty()) fi.c_name += "_a_" + to_snake_ident(f->alias_name);
        }

        auto &bucket = functions_[f->name];

        // `extern fun` 实现的是 C 头里已经声明过的函数：逐参数比对 ABI。
        // 比的是 type_c（已经过同一套 type_to_c 归一化），一旦不一致就是
        // 「C 侧调用约定对得上、宽度/符号性对不上」的静默错误，必须编译期拒绝。
        if (f->is_extern_c) {
            for (auto &exist: bucket) {
                if (!exist.is_extern || exist.owner_class != owner_class) continue;
                if (exist.params.size() != fi.params.size()) {
                    report_error_ex("E0803",
                                    "extern function '" + f->name + "' takes " +
                                        std::to_string(fi.params.size()) +
                                        " parameter(s), but the C declaration takes " +
                                        std::to_string(exist.params.size()),
                                    "match the prototype from the C header", f->line, f->col);
                    ++conflict_count_;
                    return;
                }
                for (size_t i = 0; i < fi.params.size(); ++i) {
                    if (fi.params[i].type_c == exist.params[i].type_c) continue;
                    report_error_ex("E0803",
                                    "extern function '" + f->name + "' parameter " +
                                        std::to_string(i + 1) + " is '" + fi.params[i].type_c +
                                        "', but the C declaration says '" +
                                        exist.params[i].type_c + "'",
                                    "match the prototype from the C header", f->line, f->col);
                    ++conflict_count_;
                    return;
                }
                if (fi.return_type_name != exist.return_type_name) {
                    report_error_ex("E0803",
                                    "extern function '" + f->name + "' returns '" +
                                        fi.return_type_name + "', but the C declaration returns '" +
                                        exist.return_type_name + "'",
                                    "match the prototype from the C header", f->line, f->col);
                    ++conflict_count_;
                    return;
                }
                // 签名完全一致：头里的声明就是这条实现的声明，不要重复登记，
                // 否则重载解析会看到两条一模一样的候选。改成把实现挂到这条
                // 声明上：codegen 靠 `fi->decl == f` 找定义，不挂就等于没有实现。
                //
                // 形参名必须换成 Sinter 侧的名字：函数体引用的是它，若沿用它头里的
                // 名字（`int32_t c_scale(int32_t x)` 但体里写 v），生成的 C 编不过。
                for (size_t i = 0; i < exist.params.size() && i < fi.params.size(); ++i) {
                    exist.params[i].name = fi.params[i].name;
                    exist.params[i].c_name.clear();
                }
                exist.decl = f;
                exist.module_name = current_module_;
                // 有了实现就不再是「只声明」：is_extern 为真会让 codegen 跳过定义。
                exist.is_extern = false;
                return;
            }
        }

        // 名字唯一性：同一个模块（同一类的同类方法）里，同名函数**允许共存**，
        // 前提是每个同名函数都用了 `fun name[link](...)` 指定链接名 —— 编译器按签名
        // 推导该调哪一个，生成时用各自的链接名，C 侧自然是两个不同符号。
        //
        // 真正要拦的是「同模块内两个同名函数有同样的签名」，那种情况 C 侧必然撞符号；
        // 以及「后一个同名函数没写链接名」—— 它会被拼成 `<模块>_<名字>`，和先来的那个
        // 撞车（先来的默认也是这个名字），同样在链接期才炸。
        //
        // 构造/析构是编译器合成的一族（`_copy` / `_move` / `_new`），靠显式 alias_name
        // 区分，不参与这条规则。
        const bool is_ctor_or_dtor = is_method &&
                                     (f->name == owner_class || (!f->name.empty() && f->name[0] == '~'));
        if (!is_ctor_or_dtor) {
            for (auto &exist: bucket) {
                if (exist.owner_class != owner_class) continue;
                if (is_method && exist.module_name != current_module_) continue;
                const bool mine_has_alias = !f->alias_name.empty();
                // 真正要防的是 C 符号撞车：只要两条函数/方法最终生成的 C 名字不同，
                // 就算参数签名相同也可以共存（例如 std 里 `hashCode()` 与带 `[]`
                // 的 32 位变体）。没有 `[]` 的同签名重载仍然会算出同一个 c_name，
                // 依旧被下面拒绝。
                if (fi.c_name != exist.c_name) continue;
                report_error_ex("E0204",
                                "function '" + f->name + "' is already defined in this " +
                                    (is_method ? "class" : "module") +
                                    (mine_has_alias ? " with the same parameter signature"
                                                   : "; Sinter picks overloads by signature, so every "
                                                     "overload needs its own link name, e.g. fun " +
                                                         f->name + "[" + to_snake_ident(f->name) +
                                                         "_alt](...)"),
                                mine_has_alias
                                    ? "two overloads with identical parameters cannot share a link name"
                                    : "add a link name in [] to distinguish this overload",
                                f->line, f->col);
                ++conflict_count_;
                return;
            }
        }
        // 同签名但返回类型不同，用户已用别名分支写法 => 追加
        const bool has_alias = !f->alias_name.empty() && f->alias_name != f->name;
        FunctionInfo alias_fi;
        if (has_alias) alias_fi = fi;   // 共享同一份符号/参数信息，只是多一个可调用名
        bucket.push_back(std::move(fi));
        user_functions_.insert(f->name);
        if (has_alias) {
            functions_[f->alias_name].push_back(std::move(alias_fi));
            user_functions_.insert(f->alias_name);
        }

        // 显式别名：fun f2 = f1 —— 复制目标函数的签名与返回类型
        if (!f->aliases.empty()) {
            const std::string &alias = f->aliases.front();
            std::string target = alias;
            if (const auto dot = target.rfind('.'); dot != std::string::npos)
                target = target.substr(dot + 1);
            if (const auto hit = functions_.find(target); hit != functions_.end()) {
                for (auto &tf: hit->second) {
                    if (tf.is_method) continue;
                    auto &self = bucket.back();
                    self.params = tf.params;
                    self.return_type_name = tf.return_type_name;
                    self.template_params = tf.template_params;
                    self.template_constraints = tf.template_constraints;
                    self.aliases_via.clear();
                    self.aliases_via.push_back(tf.c_name);   // 记住目标 C 名
                    const std::string nsig = self.signature();
                    self.c_name = mangled_name(self.name, nsig);
                    break;
                }
            }
        }

    }

    // 辅助：把写在类体里的同名构造函数 `T(val a: i32, b: str) { ... }`
    // 提取成构造参数表。
    //
    // 这里必须把默认构造函数排除在外：每个类都会生成一个与类同名的默认构造
    // `List() { self.data = null ... }`，它的形参不是构造参数。旧实现会在
    // `class T(...)` 未声明构造参数时把默认构造的形参当构造参数，导致
    // `List<str>()` 多出一个 `str v` 形参、`Box_<str>` 甚至多出一个字段。
    static bool looks_like_default_ctor(const FunctionDecl *f) {
        if (f->params.empty()) return true;
        // 形参全部是 self 的写法也算默认构造
        for (auto &p: f->params)
            if (!p.is_self) return false;
        return true;
    }

    static void fill_ctor_params_from_members(ClassDecl *c) {
        if (!c || !c->ctor_params.empty()) return;
        for (auto &m: c->members) {
            auto *f = dynamic_cast<FunctionDecl *>(m.get());
            if (!f) continue;
            if (f->name != c->name) continue;
            if (looks_like_default_ctor(f)) continue;
            c->ctor_params = f->params;
            return;
        }
    }

    void Semantic::collect_class(ClassDecl *c) {
        // 诊断上下文：进入类型体后，错误都带上「类/结构体 + 名字」。
        scope_kind_ = c->is_struct ? "struct" : "class";
        scope_type_ = c->name;
        scope_fn_.clear();
        // 允许把构造参数写成同名构造函数 `T(val a: i32, b: str) { ... }`
        fill_ctor_params_from_members(c);
        if (std::getenv("SINTER_DEBUG_CTOR") && c->name.find("Box") != std::string::npos) {
            std::cerr << "[ctor] " << c->name << " ctor_params=" << c->ctor_params.size();
            for (auto &p: c->ctor_params) std::cerr << " " << p.name;
            std::cerr << "\n";
        }
        ClassInfo ci;
        ci.name = c->name;
        std::vector<FieldInfo> ctor_param_fields;
        ci.c_name = cur_module_ident_ + "_" + to_snake_ident(c->name);
        ci.module_name = current_module_;
        ci.is_struct = c->is_struct;
        ci.is_abstract = c->is_abstract;
        ci.is_pub = c->is_pub;
        ci.is_annotation = c->is_annotation;
        ci.decl = c;
        for (auto &prm: c->ctor_params) ci.annotation_defaults.push_back(prm.default_value != nullptr);
        ci.has_destructor = (c->destructor != nullptr);

        for (auto &prm: c->ctor_params) {
            ParamInfo pi;
            pi.name = prm.name;
            pi.type_c = prm.type ? type_to_c(prm.type.get()) : "long long";
            pi.type_sinter = prm.type ? (prm.type->name.empty() ? pi.type_c : prm.type->name) : "i64";
            const std::string pi_type_c = pi.type_c;
            const std::string pi_type_sinter = pi.type_sinter;
            ci.ctor_params.push_back(std::move(pi));

            if (std::getenv("SINTER_DEBUG_CTOR2")) {
                std::cerr << "[param-field] " << c->name << " prm=" << prm.name
                          << " is_self=" << prm.is_self << "\n";
            }

            FieldInfo fld;
            fld.name = prm.name;
            fld.c_name = to_snake_ident(prm.name);
            fld.type_c = pi_type_c;
            fld.type_sinter = pi_type_sinter;
            fld.is_mutable = !prm.is_val; // val 只读
            fld.is_public = true;
            ci.fields.push_back(std::move(fld));
        }
        assign_param_c_names(ci.ctor_params);

        // 基类 / 接口
        // 基类可以写成泛型形式（`class IntArray : Array<i64>`）。单态化阶段的
        // Rewriter 只改写「使用处」的类型标注，`bases` 是纯名字列表，带 `<...>`
        // 的写法不会被替换 —— 那样 `Array<i64>` 既不是已登记的类型也不是外部
        // 类型，继承链直接断掉（子类拿不到 `sum` / `sort` 这些方法）。
        //
        // 这里在登记基类之前先做两件事：
        //   1) 去掉类型实参，只留模板短名（`Array<i64>` → `Array`）；
        //   2) 把实参记到 `generic_base_args_`，由单态化阶段补一条实例化需求，
        //      于是 `Array<i64>` 真的会被生成为 `Array_i64` 并被内嵌。
        for (size_t i = 0; i < c->bases.size(); ++i) {
            std::string base = c->bases[i];
            std::vector<std::string> base_args;
            const auto lt = base.find('<');
            if (lt != std::string::npos) {
                const auto gt = base.rfind('>');
                const std::string inner = base.substr(lt + 1,
                                                     gt == std::string::npos ? std::string::npos
                                                                            : gt - lt - 1);
                std::string cur;
                for (char ch: inner) {
                    if (ch == ',') {
                        if (!cur.empty()) base_args.push_back(cur);
                        cur.clear();
                    } else if (ch != ' ') {
                        cur += ch;
                    }
                }
                if (!cur.empty()) base_args.push_back(cur);
                base = base.substr(0, lt);
            }
            c->bases[i] = base;
            if (i == 0 && !base_args.empty()) {
                // 把实参编码进 `bases[0]`（`Array\x01i64`）：单态化阶段据此
                // 补一条 `Array<i64>` 的需求，写完再把名字收回纯模板短名。
                std::string encoded = base;
                encoded += '\x01';
                for (size_t k = 0; k < base_args.size(); ++k) {
                    if (k) encoded += ',';
                    encoded += base_args[k];
                }
                c->bases[i] = encoded;
            }
            if (i == 0) ci.base_class = base;
            else ci.interfaces.push_back(base);
            if (classes_.contains(base)) {
                // 同模块单继承：基类整体以 __base 内嵌，字段不展开
                ci.base_ctor_params = classes_[base].ctor_params;
            } else if (external_classes_.contains(base)) {
                // 跨模块继承：基类整体以 __base 内嵌，无需展开字段，
                // 只登记构造参数供默认 super 调用使用。
                ci.base_ctor_params = external_classes_[base].ctor_params;
            }
        }
        classes_[c->name] = ci;

        // 成员：字段 / method / 静态
        for (auto &m: c->members) {
            if (auto *v = dynamic_cast<VarDeclStmt *>(m.get())) {
                FieldInfo fld;
                fld.name = v->name;
                fld.c_name = to_snake_ident(v->name);
                fld.type_c = v->type ? type_to_c(v->type.get()) : "long long";
                fld.type_sinter = v->type ? v->type->name : "i64";
                fld.is_mutable = v->is_mutable;
                fld.is_public = true;
                // 静态字段单独登记为全局变量
                if (v->is_static) {
                    fld.is_static = true;
                    fld.decl = v;
                    static_fields_[c->name + "." + v->name] = fld;
                } else {
                    classes_[c->name].fields.push_back(std::move(fld));
                }
                if (std::getenv("SINTER_DEBUG_CTOR2")) {
                    std::cerr << "[body-field] " << c->name << " " << v->name
                              << " total=" << classes_[c->name].fields.size() << "\n";
                }
            } else if (auto *f = dynamic_cast<FunctionDecl *>(m.get())) {
                register_function(f, c->name, true);
            }
        }

        if (c->destructor) {
            if (auto *df = dynamic_cast<FunctionDecl *>(c->destructor.get())) {
                df->name = "destructor_" + c->name;   // 合法 C 标识符
                register_function(df, c->name, true);
            }
        }
        // 复制/移动构造
        if (c->copy_ctor) {
            c->copy_ctor->name = c->name + "_copy";
            register_function(c->copy_ctor.get(), c->name, true);
        }
        if (c->move_ctor) {
            c->move_ctor->name = c->name + "_move";
            register_function(c->move_ctor.get(), c->name, true);
        }
    }

    void Semantic::prescan(Program &p) {
        for (auto &s: p.statements) {
            if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                predeclare_class(c);
            } else if (auto *it = dynamic_cast<InterfaceDecl *>(s.get())) {
                predeclare_interface(it);
                // 接口方法按「抽象虚拟方法」登记：进 vtable 槽位表，
                // 实现方（`struct Circle : Shape`）用 `override` 填同一个槽。
                for (auto &m: it->methods) {
                    if (auto *f = dynamic_cast<FunctionDecl *>(m.get())) {
                        f->is_virtual = true;
                        f->is_abstract = true;
                        register_function(f, it->name, true);
                    }
                }
            } else if (auto *e = dynamic_cast<EnumDecl *>(s.get())) {
                EnumInfo ei;
                ei.name = e->name;
                ei.c_name = cur_module_ident_ + "_" + to_snake_ident(e->name);
                ei.module_name = current_module_;
                ei.variants = e->variants;
                ei.is_pub = e->is_pub;
                enums_[e->name] = std::move(ei);
            } else if (auto *pb = dynamic_cast<PubBlockStmt *>(s.get())) {
                for (auto &inner: pb->statements) {
                    if (auto *c = dynamic_cast<ClassDecl *>(inner.get())) predeclare_class(c);
                }
            } else if (auto *ms = dynamic_cast<ModuleStmt *>(s.get())) {
                if (ms->is_block) {
                    for (auto &inner: ms->body) {
                        if (auto *c = dynamic_cast<ClassDecl *>(inner.get())) predeclare_class(c);
                    }
                }
            }
        }
    }

    void Semantic::predeclare_class(ClassDecl *c) {
        ClassInfo ci;
        ci.name = c->name;
        ci.c_name = cur_module_ident_ + "_" + to_snake_ident(c->name);
        ci.module_name = current_module_;
        ci.is_struct = c->is_struct;
        ci.is_abstract = c->is_abstract;
        ci.is_pub = c->is_pub;
        ci.is_annotation = c->is_annotation;
        ci.decl = c;
        for (auto &prm: c->ctor_params) ci.annotation_defaults.push_back(prm.default_value != nullptr);
        ci.has_destructor = (c->destructor != nullptr);
        for (auto &prm: c->ctor_params) {
            ParamInfo pi;
            pi.name = prm.name;
            pi.type_c = prm.type ? type_to_c(prm.type.get()) : "long long";
            pi.type_sinter = prm.type ? (prm.type->name.empty() ? pi.type_c : prm.type->name) : "i64";
            ci.ctor_params.push_back(std::move(pi));
        }
        assign_param_c_names(ci.ctor_params);
        if (!classes_.contains(c->name)) classes_[c->name] = std::move(ci);
    }

    /// 接口登记成**抽象类**。
    ///
    /// `interface Shape { fun area(): f64 }` 以前只是方法声明的容器，完全没有
    /// 对应的 C 类型 —— 于是 `fun describe(s: Shape)` 会生成 `struct Shape*`
    /// 这种不存在的类型，gcc 报 `unknown type name 'Shape'`。
    ///
    /// 这里给它一个和 class 同构的布局（`__vptr` / `__parent` / `__type_chain`），
    /// 只剩对象头、没有字段。实现方（`struct Circle : Shape`）按普通继承处理，
    /// 接口方法进 vtable，调用点通过接口指针查表分派。
    void Semantic::predeclare_interface(InterfaceDecl *it) {
        ClassInfo ci;
        ci.name = it->name;
        ci.c_name = cur_module_ident_ + "_" + to_snake_ident(it->name);
        ci.module_name = current_module_;
        ci.is_struct = true;      // 按 struct 布局：只有对象头，没有堆分配构造
        ci.is_abstract = true;    // 不能直接实例化
        ci.is_pub = it->is_pub;
        ci.interfaces.push_back(it->name);
        if (!it->bases.empty()) ci.base_class = it->bases.front();
        if (!classes_.contains(it->name)) classes_[it->name] = std::move(ci);
    }

    void Semantic::collect(Program &p) {
        prescan(p);
        for (auto &s: p.statements) {
            if (auto *imp = dynamic_cast<ImportStmt *>(s.get())) {
                if (imp->path == "std" || imp->path == "stdlib" || imp->path == "std.core")
                    stdlib_imported_ = true;
                continue;
            }
            if (auto *ms = dynamic_cast<ModuleStmt *>(s.get())) {
                if (ms->is_block) {
                    for (auto &inner: ms->body) {
                        if (auto *f = dynamic_cast<FunctionDecl *>(inner.get())) f->is_pub = true;
                        if (auto *c = dynamic_cast<ClassDecl *>(inner.get())) c->is_pub = true;
                    }
                    Program sub;
                    for (auto &inner: ms->body) sub.statements.push_back(std::move(inner));
                    collect(sub);
                    ms->body.clear();
                    for (auto &inner: sub.statements) ms->body.push_back(std::move(inner));
                    continue;
                }
                continue;
            }

            if (auto *a = dynamic_cast<AliasStmt *>(s.get())) {
                if (a->type) aliases_[a->name] = type_to_c(a->type.get());
                continue;
            }
            // pub { ... } 展开
            if (auto *pb = dynamic_cast<PubBlockStmt *>(s.get())) {
                for (auto &inner: pb->statements) {
                    if (auto *f = dynamic_cast<FunctionDecl *>(inner.get())) f->is_pub = true;
                    if (auto *c = dynamic_cast<ClassDecl *>(inner.get())) c->is_pub = true;
                }
                // 递归处理。注意：sub 持有语句所有权，collect 期间不能清空 pb->statements，
                // 否则 codegen 看不到类/函数定义（字段与构造参数会全部丢失）。
                Program sub;
                for (auto &inner: pb->statements) sub.statements.push_back(std::move(inner));
                collect(sub);
                pb->statements.clear();
                for (auto &inner: sub.statements) pb->statements.push_back(std::move(inner));
                continue;
            }
            if (auto *f = dynamic_cast<FunctionDecl *>(s.get())) {
                register_function(f, "", false);
            } else if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                collect_class(c);
            } else if (auto *e = dynamic_cast<EnumDecl *>(s.get())) {
                EnumInfo ei;
                ei.name = e->name;
                ei.c_name = cur_module_ident_ + "_" + to_snake_ident(e->name);
                ei.module_name = current_module_;
                ei.variants = e->variants;
                ei.is_pub = e->is_pub;
                enums_[e->name] = std::move(ei);
                for (auto &v: e->variants) var_types_[v] = e->name;
            } else if (auto *cs = dynamic_cast<CStructDeclStmt *>(s.get())) {
                // 不透明 C 结构体：只登记名字与 C 拼写，不产生任何布局。
                // 放进 external_classes_ 是因为它和「别的模块的类」在语义上同类：
                // 类型拼写由对方决定，本模块既不定义也不必知道字段。
                ClassInfo ci;
                ci.name = cs->name;
                ci.c_name = cs->c_name.empty() ? ("struct " + cs->name) : cs->c_name;
                ci.module_name = current_module_;
                ci.is_struct = true;
                ci.opaque = true;
                external_classes_[cs->name] = std::move(ci);
            } else if (auto *b = dynamic_cast<BindCFunctionStmt *>(s.get())) {
                FunctionInfo fi;
                fi.name = b->c_name;
                fi.c_name = b->c_name;
                fi.module_name = current_module_;
                fi.is_extern = true;
                fi.variadic = b->variadic;
                for (auto &prm: b->params) {
                    ParamInfo pi;
                    pi.name = prm.name;
                    pi.type_c = prm.type ? type_to_c(prm.type.get()) : "long long";
                    fi.params.push_back(std::move(pi));
                }
                assign_param_c_names(fi.params);
                fi.return_type_name = b->return_type ? type_to_c(b->return_type.get()) : "void";
                functions_[b->c_name].push_back(std::move(fi));
            }
        }
    }

    // ==========================================================================
    // vtable
    // ==========================================================================
    void Semantic::build_vtables() {
        // 关键：必须先按继承深度排好序再继承槽位，否则子类可能先于基类被处理，
        // 拿到的是一张空表 —— 于是它把自己的 override 从 0 开始编号，
        // vtable 里 `__vptr[3]` 落到基类的实现上（表现为「调子类方法却执行了基类版本」）。
        // 以前 classes_ 是无序 map，顺序碰运气，多层继承时就会随机出错。
        std::vector<std::string> order;
        order.reserve(classes_.size());
        for (auto &[name, ci]: classes_) order.push_back(name);
        const auto depth_of = [&](const std::string &n) {
            int d = 0;
            std::string cur = n;
            int guard = 0;
            while (!cur.empty() && guard++ < 64) {
                const ClassInfo *c = find_class(cur);
                if (!c || c->base_class.empty()) break;
                cur = c->base_class;
                ++d;
            }
            return d;
        };
        std::stable_sort(order.begin(), order.end(),
                         [&](const std::string &a, const std::string &b) { return depth_of(a) < depth_of(b); });

        // 第一趟：按「基类 → 子类」继承槽位
        for (const auto &name: order) {
            auto &ci = classes_[name];
            if (ci.base_class.empty()) continue;
            if (classes_.contains(ci.base_class)) {
                ci.vtable = classes_[ci.base_class].vtable;
                ci.vtable_slots = classes_[ci.base_class].vtable_slots;
            } else if (external_classes_.contains(ci.base_class)) {
                const auto &xb = external_classes_[ci.base_class];
                ci.vtable = xb.vtable;
                ci.vtable_slots = xb.vtable_slots;
            }
        }
        // 第二趟：同样按深度顺序把 override 落到槽位上（同名的直接复用基类槽位）
        //
        // 注意「复用」依赖基类的方法已经分配过槽位。上面按继承深度排序保证了这一点：
        // 处理 `Rect` 时 `Shape` 的方法早已全部落位，`area` 能找到属于自己的旧槽位。
        // 若顺序颠倒，`Rect.area` 会新占一个槽位，而调用点仍按基类槽位下标去取 ——
        // 取到的是空槽／别的方法，表现为崩溃或调用错函数。
        for (const auto &name: order) {
            auto &ci = classes_[name];
            // 把「本类 + 全部祖先」的方法按「祖先优先」排一遍，保证复用槽位稳定
            std::vector<std::string> chain;
            {
                std::string cur = name;
                int guard = 0;
                while (!cur.empty() && guard++ < 64) {
                    chain.push_back(cur);
                    const ClassInfo *c = find_class(cur);
                    if (!c || c->base_class.empty()) break;
                    cur = c->base_class;
                }
                std::reverse(chain.begin(), chain.end());   // 祖先在前
            }
            for (const auto &owner: chain) {
                for (auto &[fname, bucket]: functions_) {
                    for (auto &f: bucket) {
                        if (f.owner_class != owner) continue;
                        if (f.is_static) continue;
                        // 泛型方法在调用点单态化成普通函数，不参与虚表布局
                        if (!f.template_params.empty()) continue;
                        if (f.decl && !f.decl->tparams.empty()) continue;
                        if (f.name == "destructor_" + owner) continue;          // 析构器不入 vtable
                        if (!f.name.empty() && f.name[0] == '~') continue;
                        if (f.name == owner + "_copy" || f.name == owner + "_move") continue;
                        if (f.name == owner) continue;                          // 构造函数不入 vtable
                        // 泛型类单态化后构造函数的名字可能跟着实例改名（`Array_str`），
                        // 此时 `f.name != owner`。用「该方法是否登记成某类的类名」来补判：
                        // 构造函数一定与某个类的名字完全一致，普通方法几乎不会。
                        if (classes_.contains(f.name) || external_classes_.contains(f.name)) continue;
                        int slot = -1;
                        for (auto &[vn, vs]: ci.vtable)
                            if (vn == f.name) slot = vs;
                        if (slot < 0) {
                            slot = static_cast<int>(ci.vtable_slots.size());
                            ci.vtable.emplace_back(f.name, slot);
                            ci.vtable_slots.push_back(owner + "." + f.name);
                        } else if (owner == name) {
                            // 本类的实现覆盖基类槽位：只更新标签，槽位号不动
                            if (slot < static_cast<int>(ci.vtable_slots.size()))
                                ci.vtable_slots[slot] = name + "." + f.name;
                        }
                        // 只有「属于本类」的方法需要记 slot；祖先的 slot 已在自己的
                        // ClassInfo 里定好了，这里重写会把子类的方法挂到父类方法名下。
                        if (owner == name) f.vtable_slot = slot;
                    }
                }
            }
        }
    }

    int Semantic::vtable_slot_of(const std::string &class_name, const std::string &method) const {
        auto it = classes_.find(class_name);
        if (it == classes_.end()) return -1;
        for (auto &[m, s]: it->second.vtable) if (m == method) return s;
        return -1;
    }

    // ==========================================================================
    // 重载解析
    // ==========================================================================
    // 可变参数的元素类型见 FunctionInfo::varargs_elem_type()

    static int score_match(const CType &arg, const std::string &param_c) {
        switch (arg.kind) {
            case CType::Int: {
                // 注意 `uint8_t` 不能算「更精确的整数」：`bool` 在 C 里也是
                // `uint8_t`，如果按 5 分匹配，「窄整数优先」的老逻辑会让
                // `println(3)` 抢到 `println(bool)` 上（实测输出 `true` 而不是 3）。
                // 整数往 bool 的隐式转换在 Sinter 里本来就不该发生，直接给 0。
                if (param_c == "int32_t" || param_c == "int16_t" || param_c == "int8_t") return 5;
                // `uint8_t` 形参只能接 `u8` 整数字面量（`parm 5u8`）。
                // 无符号窄整数按 5 分算，但**无后缀整数字面量**（i64）不该跳到这里 ——
                // 否则会和 `bool` 形参撞车（`bool` 在 C 里就是 `uint8_t`），
                // `println(3)` 曾因此输出 `true`。
                if (param_c == "uint8_t")
                    return (arg.bits == 8 && !arg.is_signed) ? 5 : 0;
                if (param_c == "int64_t" || param_c == "uint32_t") return 3;
                if (param_c == "long long") return 2;
                if (param_c == "double") return 1;
                return 0;
            }
            case CType::Float: {
                if (param_c == "double") return 5;
                if (param_c == "float") return 3;
                return 0;
            }
            case CType::Str:
                // 字面量默认是内部字符串，但可以直接喂给 `cstr` 形参
                if (param_c == "const char*" || param_c == "char*") return 5;
                if (param_c == "sinter_string") return 5;
                return 0;
            case CType::Bool:
                if (param_c == "long long" || param_c == "int32_t") return 4;
                if (param_c == "uint8_t") return 5;   // bool 自己的 C 名
                // 其余形参一律不接受 bool：以前这里是 `return 1`，
                // 等于「bool 能塞进任何形参」，`println(true)` 会撞上各种重载。
                return 0;
            case CType::Struct:
                if (param_c == arg.name) return 5;
                return 0;
            default: return 1;
        }
    }

    const FunctionInfo *Semantic::resolve_overload(const std::string &name,
                                                   const std::vector<CType> &arg_types,
                                                   std::string *err,
                                                   const std::string &module_ident) const {
        const auto it = functions_.find(name);
        if (it == functions_.end()) {
            if (err) *err = "undeclared function '" + name + "'";
            return nullptr;
        }
        const FunctionInfo *best = nullptr;
        int best_score = -1;
        for (auto &f: it->second) {
            if (f.is_method) continue;
            if (module_to_ident(f.module_name) != module_ident) continue;
            // 可变参数：末位形参吸收全部剩余实参，个数只要求不少于定参个数
            if (f.is_varargs ? arg_types.size() + 1 < f.params.size()
                             : arg_types.size() > f.params.size())
                continue;
            bool ok = true;
            int score = 0;
            const size_t fixed = f.is_varargs ? f.params.size() - 1 : f.params.size();
            for (size_t i = 0; i < arg_types.size(); ++i) {
                // 超出定参个数的实参按可变参数的元素类型匹配
                const std::string want = (f.is_varargs && i >= fixed)
                                              ? f.varargs_elem_type()
                                              : f.params[i].type_c;
                // A bare template parameter (`T`) accepts any argument type;
                // monomorphization substitutes the real type later.
                bool is_tparam = false;
                for (auto &tp: f.template_params)
                    if (tp == want) { is_tparam = true; break; }
                if (!is_tparam && f.decl)
                    for (auto &tp: f.decl->tparams)
                        if (tp.name == want) { is_tparam = true; break; }
                const int sc = is_tparam ? 1 : score_match(arg_types[i], want);
                if (sc == 0) { ok = false; break; }
                score += sc;
            }
            if (!ok) continue;
            for (auto &c: f.template_constraints) {
                if (c.empty()) continue;
                for (auto &a: arg_types) {
                    if (!check_constraint(c, a)) { ok = false; break; }
                }
                if (!ok) break;
            }
            if (!ok) continue;
            if (score > best_score) { best_score = score; best = &f; }
        }
        if (!best && err) *err = "cannot resolve overload '" + name + "' (module " + module_ident + "）";
        return best;
    }

    const FunctionInfo *Semantic::resolve_overload(const std::string &name,
                                                  const std::vector<CType> &arg_types,
                                                  std::string *err) {
        const FunctionInfo *hit = resolve_overload_exact(name, arg_types);
        if (hit) return hit;
        // 实例方法体里按 T 分派的多态调用（`sinter_gen_clone(v)` 之类）：
        // 单态化后名字里没有 T，靠实例名尾部的类型实参与「函数名的末段」对应，
        // 归约成 `sinter_gen_clone_str` 再解析一次。
        if (const std::string resolved = dispatch_by_instance_param(name); !resolved.empty()) {
            hit = resolve_overload_exact(resolved, arg_types);
            if (hit) return hit;
        }
        if (err) *err = "undeclared function '" + name + "'";
        return nullptr;
    }

    /// 实例方法体里的多态调用归约：函数名末段 → 实例的第 idx 个类型实参。
    /// 只对 `sinter_gen_*` 这一族白名单生效，避免误改普通函数名。
    std::string Semantic::dispatch_by_instance_param(const std::string &name) const {
        static const std::vector<std::string> kFamilies = {
            "sinter_gen_clone", "sinter_gen_free", "sinter_gen_grow", "sinter_gen_hash",
        };
        bool known = false;
        for (auto &fam: kFamilies)
            if (name == fam || (name.size() > fam.size() &&
                                name.compare(0, fam.size(), fam) == 0 &&
                                name[fam.size()] == '_'))
                known = true;
        if (!known) return {};

        // 所属类：从当前函数反查（`is_method` 的 owner_class）
        std::string owner;
        if (cur_fn_) {
            for (auto &kv: functions_) {
                for (auto &fi: kv.second) {
                    if (fi.decl != cur_fn_ || !fi.is_method) continue;
                    owner = fi.owner_class;
                    break;
                }
                if (!owner.empty()) break;
            }
        }
        if (owner.empty()) return {};

        const std::vector<std::string> *args = generic_args_of(owner);
        if (!args || args->empty()) return {};
        std::string tok = args->front();
        if (tok.empty() || tok == "void") return {};
        // User class arguments use the class's C name + pointer token, matching
        // Codegen::generic_type_token("struct x_y*") == "x_y_ptr".
        if (const auto cit = classes_.find(tok); cit != classes_.end()) {
            tok = cit->second.c_name + "_ptr";
        } else if (const auto xit = external_classes_.find(tok); xit != external_classes_.end()) {
            std::string cn = xit->second.c_name;
            if (cn.rfind("struct ", 0) == 0) cn = cn.substr(7);
            while (!cn.empty() && cn.back() == '*') cn.pop_back();
            tok = cn + "_ptr";
        }

        static const std::vector<std::string> kSuffixes = {
            "_i64", "_i32", "_i16", "_i8", "_u64", "_u32", "_u16", "_u8",
            "_f64", "_f32", "_str", "_cstr", "_bool",
        };
        for (auto &s: kSuffixes)
            if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0)
                return {};
        return name + "_" + tok;
    }

    const FunctionInfo *Semantic::resolve_overload_exact(const std::string &name,
                                                         const std::vector<CType> &arg_types,
                                                         std::string *err) const {
        const auto it = functions_.find(name);
        if (it == functions_.end()) return nullptr;
        const FunctionInfo *best = nullptr;
        int best_score = -1;
        for (auto &f: it->second) {
            if (f.is_method) continue;
            // 参数个数：允许默认参数
            // 可变参数：末位形参吸收全部剩余实参，个数只要求不少于定参个数
            if (f.is_varargs ? arg_types.size() + 1 < f.params.size()
                             : arg_types.size() > f.params.size())
                continue;
            bool ok = true;
            int score = 0;
            const size_t fixed = f.is_varargs ? f.params.size() - 1 : f.params.size();
            for (size_t i = 0; i < arg_types.size(); ++i) {
                // 超出定参个数的实参按可变参数的元素类型匹配
                const std::string want = (f.is_varargs && i >= fixed)
                                              ? f.varargs_elem_type()
                                              : f.params[i].type_c;
                // A bare template parameter (`T`) accepts any argument type;
                // monomorphization substitutes the real type later.
                bool is_tparam = false;
                for (auto &tp: f.template_params)
                    if (tp == want) { is_tparam = true; break; }
                if (!is_tparam && f.decl)
                    for (auto &tp: f.decl->tparams)
                        if (tp.name == want) { is_tparam = true; break; }
                const int s = is_tparam ? 1 : score_match(arg_types[i], want);
                if (s == 0) { ok = false; break; }
                score += s;
            }
            if (!ok) continue;
            // 泛型 must 约束
            for (auto &c: f.template_constraints) {
                if (c.empty()) continue;
                for (auto &a: arg_types) {
                    if (!check_constraint(c, a)) { ok = false; break; }
                }
                if (!ok) break;
            }
            if (!ok) continue;
            if (score > best_score) { best_score = score; best = &f; }
        }
        if (!best) {
            if (err) *err = "cannot resolve overload '" + name + "' (no matching argument types)";
            return nullptr;
        }
        return best;
    }

    // ==========================================================================
    // 检查
    // ==========================================================================
    void Semantic::check_expr(Expression *e) {
        if (!e) return;
        if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
            check_expr(b->left.get());
            check_expr(b->right.get());
        } else if (auto *u = dynamic_cast<UnaryExpr *>(e)) {
            check_expr(u->operand.get());
        } else if (auto *c = dynamic_cast<CallExpr *>(e)) {
            check_expr(c->callee.get());
            for (auto &a: c->args) check_expr(a.value.get());
        } else if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) {
            check_expr(m->object.get());
        } else if (auto *a = dynamic_cast<AssignmentExpr *>(e)) {
            check_expr(a->target.get());
            check_expr(a->value.get());
        } else if (auto *t = dynamic_cast<TernaryExpr *>(e)) {
            check_expr(t->cond.get());
            check_expr(t->then_b.get());
            check_expr(t->else_b.get());
        } else if (auto *p = dynamic_cast<PipelineExpr *>(e)) {
            check_expr(p->left.get());
            check_expr(p->right.get());
        } else if (auto *s = dynamic_cast<StringTemplateExpr *>(e)) {
            for (auto &pt: s->parts) check_expr(pt.expr.get());
        } else if (auto *w = dynamic_cast<WhenExpr *>(e)) {
            check_expr(w->subject.get());
            for (auto &br: w->branches) {
                for (auto &pt: br.patterns) {
                    check_expr(pt.value.get());
                    check_expr(pt.lo.get());
                    check_expr(pt.hi.get());
                }
                check_expr(br.body.get());
            }
            check_expr(w->else_branch.get());
        }
    }

    void Semantic::check_function(FunctionDecl *f) {
        // 记下当前所在函数：实例方法体里按 T 分派的多态调用要靠它定位类。
        auto *saved_fn = cur_fn_;
        cur_fn_ = f;
        check_function_body(f);
        cur_fn_ = saved_fn;
    }

    void Semantic::check_function_body(FunctionDecl *f) {
        // val/var 检查 + 变量不可不初始化
        std::function<void(Statement *)> walk = [&](Statement *s) {
            if (!s) return;
            if (auto *b = dynamic_cast<BlockStmt *>(s)) {
                for (auto &x: b->statements) walk(x.get());
            } else if (auto *v = dynamic_cast<VarDeclStmt *>(s)) {
                if (v->is_const && !v->init) report_error("const variable requires an initializer: " + v->name);
                if (!v->init && !v->is_static && !v->type) {
                    report_error("variable requires an initializer: " + v->name);
                }
                if (v->type) var_types_[v->name] = v->type->name;
                check_expr(v->init.get());
            } else if (auto *d = dynamic_cast<DestructureStmt *>(s)) {
                check_expr(d->init.get());
            } else if (auto *r = dynamic_cast<ReturnStmt *>(s)) {
                check_expr(r->value.get());
            } else if (auto *i = dynamic_cast<IfStmt *>(s)) {
                check_expr(i->cond.get());
                walk(i->then_b.get());
                walk(i->else_b.get());
            } else if (auto *w = dynamic_cast<WhileStmt *>(s)) {
                check_expr(w->cond.get());
                walk(w->body.get());
            } else if (auto *fs = dynamic_cast<ForStmt *>(s)) {
                check_expr(fs->iterable.get());
                walk(fs->body.get());
            } else if (auto *dw = dynamic_cast<DoWhileStmt *>(s)) {
                walk(dw->body.get());
                check_expr(dw->cond.get());
            } else if (auto *ex = dynamic_cast<ExprStmt *>(s)) {
                check_expr(ex->expr.get());
            }
        };
        walk(f->body.get());
    }

    void Semantic::check_stmt(Statement *s) {
        if (!s) return;
        if (auto *b = dynamic_cast<BlockStmt *>(s)) {
            for (auto &x: b->statements) check_stmt(x.get());
        } else if (auto *f = dynamic_cast<FunctionDecl *>(s)) {
            check_function(f);
        } else if (auto *c = dynamic_cast<ClassDecl *>(s)) {
            for (auto &m: c->members) check_stmt(m.get());
            if (c->destructor) check_stmt(c->destructor.get());
        } else if (auto *pb = dynamic_cast<PubBlockStmt *>(s)) {
            for (auto &m: pb->statements) check_stmt(m.get());
        } else if (auto *ms = dynamic_cast<ModuleStmt *>(s)) {
            if (ms->is_block) for (auto &m: ms->body) check_stmt(m.get());
        }
    }

    void Semantic::analyze(Program &p, const std::string &module_name) {
        current_module_ = module_name;
        cur_module_ident_ = module_to_ident(module_name);
        collect(p);
        // 第二遍：建立 vtable
        build_vtables();
        for (auto &s: p.statements) check_stmt(s.get());
        // 第三遍（纯诊断）：when 的穷尽性。放在 check_stmt 之后，这样
        // var_types_ / enums_ 都已就绪；它不修改 AST，也不影响后续 codegen。
        check_when_exhaustive(p);
        validate_annotations(p);
    }

    namespace {
        /// 找到所有 `when` 并检查枚举分支是否覆盖全部变体。
        ///
        /// 独立于 check_stmt 的理由：穷尽性是「语法 + 枚举表」的纯诊断，不需要类型
        /// 上下文，也不修改 AST；单独走动不会给主检查逻辑添任何风险。
        ///
        /// 保守原则（和 typeck 一致）：只要 subject 类型推不出来、分支里出现非枚举
        /// 值、或已有 `else`，就一律不报——宁可漏报也不误判合法程序。
        struct WhenExhaustiveWalker {
            const std::unordered_map<std::string, EnumInfo> &enums;
            const std::unordered_map<std::string, std::string> &var_types;
            std::vector<Diagnostic> &diags;
            std::string file;

            [[nodiscard]] const std::string *enum_of_variant(const std::string &v) const {
                for (const auto &kv: enums)
                    for (const auto &x: kv.second.variants)
                        if (x == v) return &kv.first;
                return nullptr;
            }

            void check(WhenExpr &w) {
                if (w.else_branch || w.else_block) return;   // 有 else 视为穷尽
                std::string target;
                if (auto *id = dynamic_cast<IdentifierExpr *>(w.subject.get())) {
                    const auto it = var_types.find(id->name);
                    if (it != var_types.end() && enums.contains(it->second)) target = it->second;
                    else if (const std::string *e = enum_of_variant(id->name)) target = *e;
                }
                if (target.empty()) {
                    // 类型推不出来时用分支反推：只有所有 Value 模式都指向**同一个**
                    // 枚举才检查（否则多半是整数 / 字符串分支，不该报穷尽）。
                    std::set<std::string> cand;
                    for (auto &br: w.branches)
                        for (auto &pt: br.patterns) {
                            if (pt.mode != WhenExpr::Pattern::Value) continue;
                            auto *pid = dynamic_cast<IdentifierExpr *>(pt.value.get());
                            if (!pid) continue;
                            if (const std::string *e = enum_of_variant(pid->name)) cand.insert(*e);
                        }
                    if (cand.size() == 1) target = *cand.begin();
                }
                if (target.empty()) return;
                const auto eit = enums.find(target);
                if (eit == enums.end() || eit->second.variants.empty()) return;

                std::set<std::string> covered;
                for (auto &br: w.branches)
                    for (auto &pt: br.patterns) {
                        if (pt.mode != WhenExpr::Pattern::Value) continue;
                        if (auto *pid = dynamic_cast<IdentifierExpr *>(pt.value.get()))
                            covered.insert(pid->name);
                    }
                std::string missing;
                int n = 0;
                for (const auto &v: eit->second.variants) {
                    if (covered.contains(v)) continue;
                    if (n++) missing += "、";
                    missing += v;
                }
                if (n == 0) return;
                diags.push_back({Severity::Error,
                                 "when is not exhaustive: `" + target + "` is missing branch(es): " +
                                     missing,
                                 file, w.line, w.col, 0,
                                 "E0203",
                                 "list every variant, or add an 'else' branch to opt out of the check"});
            }

            void expr(ExprPtr &e) {
                if (!e) return;
                if (auto *w = dynamic_cast<WhenExpr *>(e.get())) {
                    check(*w);
                    expr(w->subject);
                    for (auto &br: w->branches) {
                        for (auto &pt: br.patterns) {
                            expr(pt.value);
                            expr(pt.lo);
                            expr(pt.hi);
                        }
                        expr(br.body);
                        stmt(br.body_block);
                    }
                    expr(w->else_branch);
                    stmt(w->else_block);
                    return;
                }
                if (auto *b = dynamic_cast<BinaryExpr *>(e.get())) { expr(b->left); expr(b->right); return; }
                if (auto *u = dynamic_cast<UnaryExpr *>(e.get())) { expr(u->operand); return; }
                if (auto *g = dynamic_cast<GroupingExpr *>(e.get())) { expr(g->expr); return; }
                if (auto *c = dynamic_cast<CallExpr *>(e.get())) {
                    expr(c->callee);
                    for (auto &a: c->arguments) expr(a);
                    for (auto &a: c->args) expr(a.value);
                    return;
                }
                if (auto *m = dynamic_cast<MemberAccessExpr *>(e.get())) { expr(m->object); return; }
                if (auto *i = dynamic_cast<IndexAccessExpr *>(e.get())) { expr(i->object); expr(i->index); return; }
                if (auto *a = dynamic_cast<AssignmentExpr *>(e.get())) { expr(a->target); expr(a->value); return; }
                if (auto *t = dynamic_cast<TernaryExpr *>(e.get())) { expr(t->cond); expr(t->then_b); expr(t->else_b); return; }
                if (auto *i = dynamic_cast<IfExpr *>(e.get())) { expr(i->cond); expr(i->then_b); expr(i->else_b); return; }
                if (auto *p = dynamic_cast<PipelineExpr *>(e.get())) { expr(p->left); expr(p->right); return; }
                if (auto *a = dynamic_cast<ArrayLiteralExpr *>(e.get())) { for (auto &x: a->elements) expr(x); return; }
                if (auto *c = dynamic_cast<CastExpr *>(e.get())) { expr(c->expr); return; }
                if (auto *s = dynamic_cast<StringTemplateExpr *>(e.get())) {
                    for (auto &part: s->parts) if (part.expr) expr(part.expr);
                    return;
                }
                if (auto *l = dynamic_cast<LambdaExpr *>(e.get())) {
                    for (auto &p: l->params) expr(p.default_value);
                    if (auto *body = std::get_if<ExprPtr>(&l->body)) expr(*body);
                    else if (auto *body = std::get_if<StmtPtr>(&l->body)) stmt(*body);
                    return;
                }
                if (auto *r = dynamic_cast<RangeExpr *>(e.get())) { expr(r->start); expr(r->end); return; }
                if (auto *s = dynamic_cast<ScopeCallExpr *>(e.get())) {
                    expr(s->receiver);
                    expr(s->body_expr);
                    stmt(s->body);
                    return;
                }
                if (auto *b = dynamic_cast<BlockExpr *>(e.get())) { expr(b->result); stmt(b->block); return; }
            }

            void stmt(StmtPtr &s) {
                if (!s) return;
                if (auto *b = dynamic_cast<BlockStmt *>(s.get())) { for (auto &x: b->statements) stmt(x); return; }
                if (auto *v = dynamic_cast<VarDeclStmt *>(s.get())) { expr(v->init); return; }
                if (auto *d = dynamic_cast<DestructureStmt *>(s.get())) { expr(d->init); return; }
                if (auto *e = dynamic_cast<ExprStmt *>(s.get())) { expr(e->expr); return; }
                if (auto *r = dynamic_cast<ReturnStmt *>(s.get())) { expr(r->value); return; }
                if (auto *i = dynamic_cast<IfStmt *>(s.get())) { expr(i->cond); stmt(i->then_b); stmt(i->else_b); return; }
                if (auto *w = dynamic_cast<WhileStmt *>(s.get())) { expr(w->cond); stmt(w->body); return; }
                if (auto *f = dynamic_cast<ForStmt *>(s.get())) { expr(f->iterable); stmt(f->body); return; }
                if (auto *d = dynamic_cast<DoWhileStmt *>(s.get())) { stmt(d->body); expr(d->cond); return; }
                if (auto *f = dynamic_cast<FunctionDecl *>(s.get())) {
                    for (auto &p: f->params) expr(p.default_value);
                    stmt(f->body);
                    return;
                }
                if (auto *c = dynamic_cast<ClassDecl *>(s.get())) {
                    for (auto &p: c->ctor_params) expr(p.default_value);
                    for (auto &m: c->members) stmt(m);
                    stmt(c->destructor);
                    return;
                }
                if (auto *st = dynamic_cast<StructDecl *>(s.get())) { for (auto &m: st->methods) stmt(m); return; }
                if (auto *it = dynamic_cast<InterfaceDecl *>(s.get())) { for (auto &m: it->methods) stmt(m); return; }
                if (auto *m = dynamic_cast<ModuleStmt *>(s.get())) {
                    if (m->is_block) for (auto &x: m->body) stmt(x);
                    return;
                }
                if (auto *c = dynamic_cast<ComptimeStmt *>(s.get())) { stmt(c->body); return; }
                if (auto *p = dynamic_cast<PubBlockStmt *>(s.get())) { for (auto &x: p->statements) stmt(x); return; }
                if (auto *d = dynamic_cast<DelStmt *>(s.get())) { expr(d->target); return; }
            }
        };
    } // namespace

    void Semantic::validate_annotations(Program &p) {
        // `annotation class` declarations are compile-time specs: every use of a
        // declared annotation is checked for argument count, missing required
        // parameters and literal argument types. Unknown @names stay allowed so
        // built-in annotations (@deprecated, @must_use, ...) and external tools work.
        auto find_spec = [&](const std::string &name) -> const ClassInfo * {
            for (auto &kv: classes_) {
                if (kv.second.is_annotation && kv.second.name == name) return &kv.second;
            }
            return nullptr;
        };
        auto check_list = [&](const std::vector<Annotation> &anns, int line, int col) {
            for (auto &a: anns) {
                const ClassInfo *spec = find_spec(a.name);
                if (!spec) continue;
                const size_t given = a.args.size();
                const size_t total = spec->ctor_params.size();
                if (given > total) {
                    report_error_ex("E0204",
                                    "annotation '@" + a.name + "' takes " + std::to_string(total) +
                                        " argument(s), got " + std::to_string(given),
                                    "remove the extra annotation arguments", line, col);
                    continue;
                }
                for (size_t i = given; i < total; ++i) {
                    const bool has_default =
                        i < spec->annotation_defaults.size() && spec->annotation_defaults[i];
                    if (!has_default) {
                        report_error_ex("E0204",
                                        "annotation '@" + a.name + "' is missing required argument '" +
                                            spec->ctor_params[i].name + "'",
                                        "pass all non-default annotation parameters", line, col);
                    }
                }
                for (size_t i = 0; i < given; ++i) {
                    auto *lit = dynamic_cast<LiteralExpr *>(a.args[i].get());
                    if (!lit) continue;
                    const std::string &want = spec->ctor_params[i].type_sinter;
                    bool ok = true;
                    switch (lit->lit) {
                        case LiteralExpr::Int:
                            ok = want == "i8" || want == "i16" || want == "i32" || want == "i64" ||
                                 want == "u8" || want == "u16" || want == "u32" || want == "u64" ||
                                 want == "int" || want == "long";
                            break;
                        case LiteralExpr::Float: ok = want == "f32" || want == "f64" || want == "float" ||
                                                          want == "double";
                            break;
                        case LiteralExpr::String: ok = want == "str" || want == "cstr"; break;
                        case LiteralExpr::Bool: ok = want == "bool"; break;
                        default: break;
                    }
                    if (!ok) {
                        report_error_ex("E0301",
                                        "annotation argument '" + spec->ctor_params[i].name +
                                            "' expects " + want,
                                        "use a literal of the declared parameter type", line, col);
                    }
                }
            }
        };
        std::function<void(Statement *)> walk = [&](Statement *s) {
            if (!s) return;
            if (auto *f = dynamic_cast<FunctionDecl *>(s)) {
                check_list(f->annotations, f->line, f->col);
                if (auto *b = dynamic_cast<BlockStmt *>(f->body.get())) {
                    for (auto &inner: b->statements) walk(inner.get());
                }
                return;
            }
            if (auto *c = dynamic_cast<ClassDecl *>(s)) {
                check_list(c->annotations, c->line, c->col);
                for (auto &m: c->members) walk(m.get());
                return;
            }
            if (auto *i = dynamic_cast<InterfaceDecl *>(s)) {
                check_list(i->annotations, i->line, i->col);
                for (auto &m: i->methods) walk(m.get());
                return;
            }
            if (auto *e = dynamic_cast<EnumDecl *>(s)) check_list(e->annotations, e->line, e->col);
        };
        for (auto &s: p.statements) walk(s.get());
    }

    void Semantic::check_when_exhaustive(Program &p) {
        WhenExhaustiveWalker w{enums_, var_types_, diags_,
                               current_file_.empty() ? cur_module_ident_ : current_file_};
        for (auto &s: p.statements) w.stmt(s);
    }
} // namespace sinter
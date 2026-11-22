// sinter backend: codegen_exprs.cpp
#include "backend/codegen.hpp"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>

namespace sinter {
    namespace {
        // 类型标签用的短名（`sinter_gen_grow(vals, n, i64)` 末尾那个 `i64`）。
        // 这些名字在 Sinter 里是类型关键字，正常不会当变量用，
        // 因此「分派族调用的末位实参 = 其中的一个」就判定为编译期标签。
        bool looks_like_type_token(const std::string &n) {
            static const std::vector<std::string> kTokens = {
                "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
                "f32", "f64", "str", "cstr", "bool",
            };
            for (auto &t: kTokens)
                if (n == t) return true;
            return false;
        }

        /// Named call arguments -> positional parameter types for overload
        /// resolution. Missing slots are allowed only when the declaration has
        /// a default value; the returned types are Unknown for those slots.
        bool map_named_types(const std::vector<Argument> &args,
                             const std::vector<ParamInfo> &params,
                             const FunctionDecl *decl,
                             const std::vector<CType> &arg_types,
                             std::vector<CType> &out) {
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
                    if (slot >= out.size()) return false;
                }
                if (slot >= out.size()) continue;
                if (filled[slot]) return false;
                out[slot] = arg_types[i];
                filled[slot] = 1;
            }
            for (size_t k = 0; k < out.size(); ++k) {
                if (filled[k]) continue;
                if (decl && k < decl->params.size() && decl->params[k].default_value) continue;
                return false;
            }
            return true;
        }

        // ---- 字符串字面量：先解码成真实字节，再按字节重新编码 ----------------
        //
        // 以前是把源码里的字面量体原样抄进生成的 C，长度按「源码字符数」传给
        // sinter_string_from_literal。对 `"a\"b"` 来说源码体是 4 个字符
        // （a \ " b），但 C 侧解码后只有 3 个字节，于是 len=4 会多读一个字节，
        // 把结尾的 '\0' 也算进内容里（`od -c` 能看到 `a " b \0 \n`）。
        //
        // 现在改成：解码 → 得到真正要放进 .rodata 的字节 → 按字节重新转义 →
        // 长度取字节数。这样 `\n`、`\"`、`\\`、`\x41` 都只算一个字节。
        std::string decode_string_body(const std::string &src) {
            std::string out;
            out.reserve(src.size());
            for (size_t i = 0; i < src.size(); ++i) {
                if (src[i] != '\\' || i + 1 >= src.size()) {
                    out += src[i];
                    continue;
                }
                const char e = src[++i];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case 'a': out += '\a'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'v': out += '\v'; break;
                    case 'e': out += '\x1b'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    case 'x': {
                        std::string hex;
                        while (hex.size() < 2 && i + 1 < src.size() &&
                               std::isxdigit(static_cast<unsigned char>(src[i + 1])))
                            hex += src[++i];
                        if (hex.empty()) out += 'x';
                        else out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
                        break;
                    }
                    default: out += e; break; // 未知转义：保留字符本身，别吞掉
                }
            }
            return out;
        }

        /// 字节 -> C 字面量内容（不含两侧引号）。非可打印字节一律三位八进制，
        /// 因为 `\xHH` 后面紧跟十六进制字符时会被 C 解析成更长的数。
        std::string encode_c_body(const std::string &bytes) {
            std::string out;
            out.reserve(bytes.size() + 8);
            for (const unsigned char c: bytes) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    case '\r': out += "\\r"; break;
                    default:
                        if (c >= 0x20 && c < 0x7f) {
                            out += static_cast<char>(c);
                        } else {
                            char buf[8];
                            std::snprintf(buf, sizeof buf, "\\%03o", c);
                            out += buf;
                        }
                }
            }
            return out;
        }

        /// 源码字面量体 -> `sinter_string_from_literal("...", 字节数)`
        std::string literal_from_source_body(const std::string &src_body) {
            const std::string bytes = decode_string_body(src_body);
            return "sinter_string_from_literal(\"" + encode_c_body(bytes) + "\", " +
                   std::to_string(bytes.size()) + ")";
        }
    } // namespace

    std::string Codegen::expr(Expression *e) {
        if (!e) return "0";
        // 语句级临时字符串已经落到局部变量：直接引用变量名，
        // 由语句末尾统一释放（不要重复求值原表达式，否则会再分配一次）。
        if (!hoist_names_.empty()) {
            auto it = hoist_names_.find(e);
            if (it != hoist_names_.end()) return it->second;
        }
        if (auto *l = dynamic_cast<LiteralExpr *>(e)) return expr_literal(l);
        if (auto *i = dynamic_cast<IdentifierExpr *>(e)) return expr_ident(i);
        if (auto *b = dynamic_cast<BinaryExpr *>(e)) return expr_binary(b);
        if (auto *u = dynamic_cast<UnaryExpr *>(e)) return expr_unary(u);
        if (auto *c = dynamic_cast<CallExpr *>(e)) return expr_call(c);
        if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) return expr_member(m);
        if (auto *i = dynamic_cast<IndexAccessExpr *>(e)) return expr_index(i);
        if (auto *a = dynamic_cast<AssignmentExpr *>(e)) return expr_assign(a);
        if (auto *n = dynamic_cast<NonNullExpr *>(e)) {
            const std::string vt = c_type_of_expr(n->operand.get());
            std::string v = expr(n->operand.get());
            if (has_side_effect(n->operand.get()) && !vt.empty()) {
                const std::string tmp = fresh("__nn");
                emit(vt + " " + tmp + " = " + v + ";");
                v = tmp;
            }
            // 非空断言：空指针直接 abort（不静默继续）。
            return "((" + v + ") ? (" + v + ") : (abort(), (" + v + ")))";
        }
        if (auto *l = dynamic_cast<LambdaExpr *>(e)) return expr_lambda(l);
        if (auto *t = dynamic_cast<TernaryExpr *>(e)) return expr_ternary(t);
        if (auto *i = dynamic_cast<IfExpr *>(e)) return expr_if(i);
        if (auto *w = dynamic_cast<WhenExpr *>(e)) return expr_when(w);
        if (auto *a = dynamic_cast<ArrayLiteralExpr *>(e)) return expr_array(a);
        if (auto *g = dynamic_cast<GroupingExpr *>(e)) return expr_group(g);
        if (auto *p = dynamic_cast<PipelineExpr *>(e)) return expr_pipeline(p);
        if (auto *s = dynamic_cast<StringTemplateExpr *>(e)) return expr_template(s);
        if (auto *s = dynamic_cast<ScopeCallExpr *>(e)) return expr_scope_call(s);
        if (auto *b = dynamic_cast<BlockExpr *>(e)) {
            if (b->is_block && b->block) {
                if (auto *blk = dynamic_cast<BlockStmt *>(b->block.get()); blk && !blk->statements.empty()) {
                    auto &last = blk->statements.back();
                    if (auto *ex = dynamic_cast<ExprStmt *>(last.get())) return expr(ex->expr.get());
                    if (auto *rt = dynamic_cast<ReturnStmt *>(last.get())) return expr(rt->value.get());
                }
            }
            return "0";
        }
        if (auto *c = dynamic_cast<CastExpr *>(e))
            return "((" + c_type(c->target.get()) + ")" + expr(c->expr.get()) + ")";
        if (auto *b = dynamic_cast<BuiltinExpr *>(e)) {
            // 正常路径上 comptime pass 已经把 __FILE__ 等折成字面量。
            // 走到这里说明折叠漏了位置（compiler bug）——宁可明确报错，
            // 也不能像兜底分支那样静默生成 0，让用户以为代码跑了。
            throw std::runtime_error(
                std::string("internal error: compile-time keyword ") + BuiltinExpr::spelling(b->which) +
                " was not folded (line " + std::to_string(b->line) + ")");
        }
        return "0";
    }

    std::string Codegen::expr_literal(LiteralExpr *l) {
        switch (l->lit) {
            case LiteralExpr::Int: {
                const long long v = std::get<int64_t>(l->value);
                // INT64_MIN 没法写成字面量：`-9223372036854775808LL` 里的
                // 数值部分本身就超出 int64，ISO C 报“integer constant is so
                // large that it is unsigned”（-pedantic-errors 下是 error）。
                // 唯一合法的写法是 `(-9223372036854775807LL - 1)`。
                if (v == std::numeric_limits<long long>::min() && l->suffix.empty())
                    return "(-9223372036854775807LL - 1)";
                const std::string s = std::to_string(v);
                if (l->suffix == "i8") return "(int8_t)" + s + "LL";
                if (l->suffix == "i16") return "(int16_t)" + s + "LL";
                if (l->suffix == "i32") return "(int32_t)" + s + "LL";
                if (l->suffix == "i64") return s + "LL";
                if (l->suffix == "u8") return "(uint8_t)" + s + "ULL";
                if (l->suffix == "u32") return "(uint32_t)" + s + "ULL";
                if (l->suffix == "u64") return s + "ULL";
                return s + "LL";
            }
            case LiteralExpr::Float: {
                std::ostringstream o;
                o.precision(17);
                o << std::get<double>(l->value);
                std::string s = o.str();
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
                if (l->suffix == "f32") return s + "f";
                return s;
            }
            case LiteralExpr::String: {
                // 字符串字面量 -> 内部字符串。长度是**解码后**的字节数：
                // 源码里的 `\"` 是两字符、一个字节，按字符数算会多读一个字节。
                std::string body = l->raw;
                if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
                    body = body.substr(1, body.size() - 2);
                return literal_from_source_body(body);
            }
            case LiteralExpr::Char: return l->raw;
            case LiteralExpr::Bool: return std::get<bool>(l->value) ? "1u" : "0u";
            case LiteralExpr::Null: return "NULL";
        }
        return "0";
    }

    std::string Codegen::expr_ident(IdentifierExpr *i) {
        if (const auto it = locals_.find(i->name); it != locals_.end()) {
            if (ref_vars_.count(i->name) && !ref_class_vars_.count(i->name))
                return "(*" + it->second + ")";
            return it->second;
        }
        if (i->name == "this") return "this";
        // 模块级常量 / 变量：`comptime { val LIMIT = 12 }` 展开成模块级 `val`，
        // 真正的 C 符号带模块前缀（`ct_g_LIMIT`）。这里不兜底会生成裸 `LIMIT`，
        // gcc 报 `'LIMIT' undeclared` —— 看起来像编译器没展开 comptime 块。
        if (module_globals_.contains(i->name))
            return module_ident_ + "_g_" + i->name;
        // lambda / 作用域函数块中：裸字段名解析到接收者
        if (!lambda_receiver_.empty() && locals_.contains(lambda_receiver_)) {
            const std::string rt = local_types_.count(lambda_receiver_)
                                       ? local_types_[lambda_receiver_]
                                       : std::string();
            if (!rt.empty() && rt.back() == '*') {
                std::string cur;
                if (const ClassInfo *rc = class_by_c_type(rt)) cur = rc->name;
                int guard = 0;
                while (!cur.empty() && guard++ < 32) {
                    const ClassInfo *cc = sem_.find_class(cur);
                    if (!cc) break;
                    for (auto &f: cc->fields)
                        if (f.name == i->name)
                            return locals_[lambda_receiver_] + "->" + f.c_name;
                    cur = cc->base_class;
                }
            }
        }
        for (auto &[en, ei]: sem_.enums())
            for (auto &v: ei.variants)
                if (v == i->name) return ei.c_name + "_" + Semantic::to_snake_ident(v);
        // 函数名作为值（形参是函数类型、赋给函数指针变量）：
        // 生成它在 C 里的链接名，而不是源码里的裸名。以前这里落到最后
        // `return i->name`，`apply(square, 5)` 会生成 `apply(square, 5)`，
        // gcc 报 'square' undeclared（真实符号是 `t_square`）。
        if (const auto fit = sem_.functions().find(i->name); fit != sem_.functions().end()) {
            for (const auto &f: fit->second) {
                if (!f.is_method && !f.c_name.empty()) return f.c_name;
            }
        }
        return i->name;
    }

    /// `Color.Red` 这种带限定名的枚举变体：解析成 `<枚举C名>_<变体>`。
    /// 不带这一支，生成的 C 会原样写出 `Color.Red`（typedef 名是 `control_color`，
    /// 变体常量是 `control_color_red`，两者都不叫 `Color`），gcc 报
    /// `'Color' undeclared`。裸 `Red` 走 IdentifierExpr 那一支，一直是好的，
    /// 所以这个缺陷只在写限定名时暴露。
    std::string Codegen::enum_variant_c_name(const std::string &enum_name,
                                             const std::string &variant) const {
        const auto it = sem_.enums().find(enum_name);
        if (it == sem_.enums().end()) return std::string();
        for (auto &v: it->second.variants)
            if (v == variant) return it->second.c_name + "_" + Semantic::to_snake_ident(v);
        return std::string();
    }

    std::string Codegen::op_symbol(TokenType t) {
        switch (t) {
            case TokenType::Plus: return "+";
            case TokenType::Minus: return "-";
            case TokenType::Star: return "*";
            case TokenType::Slash: return "/";
            case TokenType::Percent: return "%";
            case TokenType::EqualEqual: return "==";
            case TokenType::NotEqual: return "!=";
            case TokenType::Less: return "<";
            case TokenType::LessEqual: return "<=";
            case TokenType::Greater: return ">";
            case TokenType::GreaterEqual: return ">=";
            default: return std::string();
        }
    }

    std::string Codegen::find_binary_operator(const TokenType op,
                                             const std::string &lt,
                                             const std::string &rt) const {
        const std::string sym = op_symbol(op);
        if (sym.empty()) return {};
        // 只有涉及结构体指针的运算才需要改派；纯数值运算保持原生 C 运算符。
        const bool lobj = !lt.empty() && lt.back() == '*';
        const bool robj = !rt.empty() && rt.back() == '*';
        if (!lobj && !robj) return {};

        const auto hit = sem_.functions().find("operator" + sym);
        if (hit == sem_.functions().end()) return {};
        // 成员运算符：函数名由语义层编织成 <模块>_<类>_operator<符号>__<签名>，
        // 因此按 owner_class 反查类型再拼 C 名。
        for (auto &f: hit->second) {
            if (!f.is_method) continue;
            const ClassInfo *ci = sem_.find_class(f.owner_class);
            if (!ci) continue;
            const std::string want = "struct " + ci->c_name + "*";
            // 左操作数是该类型，或右操作数是（`i64 + Vec` 时右操作数决定）
            if (lt == want || rt == want) return f.c_name;
        }
        // 自由运算符：第一个形参是左操作数
        for (auto &f: hit->second) {
            if (f.is_method) continue;
            if (f.params.size() != 2) continue;
            const bool l_ok = f.params[0].type_c == lt;
            const bool r_ok = f.params[1].type_c == rt;
            if (l_ok && r_ok) return f.c_name;
        }
        return {};
    }

    std::string Codegen::expr_binary(const BinaryExpr *b) {
        const auto lt = c_type_of_expr(b->left.get());
        const auto rt = c_type_of_expr(b->right.get());
        const auto is_str_t = [](const std::string &t) {
            return t == "sinter_string" || t == "const char*" || t == "char*";
        };
        const bool lstr = is_str_t(lt);
        const bool rstr = is_str_t(rt);

        // 运算符重载：结构体/类上的二元运算改派到 `operator<符号>` 成员函数。
        // 左操作数或右操作数任一是有析构/有方法的结构体就走这条路径。
        const std::string opfn = find_binary_operator(b->op, lt, rt);
        if (!opfn.empty()) {
            return opfn + "(" + expr(b->left.get()) + ", " + expr(b->right.get()) + ")";
        }
if (b->op == TokenType::Plus && (lstr || rstr)) {
            // 两侧都升级为内部字符串后拼接，避免每次 strlen 与二次分配。
            // 所有权：只有「variable / 字段 / 下标」是被借用的，必须保留；
            // 字面量、运算结果、函数调用返回值都能在拼完后顺手释放。
            const std::string l = str_cast(expr(b->left.get()), lt);
            const std::string r = str_cast(expr(b->right.get()), rt);
            // 左右两侧都是临时值 → 两个都能顺手释放。
            // 只有「本来就是字符串的variable / 字段 / 下标」必须保留：
            // 它们是被借用后还要继续用的；非字符串转过来的字符串一定是新建的。
            auto is_borrowed = [](Expression *e, const std::string &t) {
                const bool is_str = t == "sinter_string" || t == "const char*" || t == "char*";
                if (!is_str) return false;   // 数值转字符串 → 新建，可释放
                return is_borrowed_expr(e);
            };
            const bool l_borrowed = is_borrowed(b->left.get(), lt);
            const bool r_borrowed = is_borrowed(b->right.get(), rt);
            if (!l_borrowed && !r_borrowed)
                return "sinter_string_concat_own(" + l + ", " + r + ")";
            if (!r_borrowed)
                return "sinter_string_concat_take_right(" + l + ", " + r + ")";
            if (!l_borrowed)
                return "sinter_string_concat_take_left(" + l + ", " + r + ")";
            return "sinter_string_concat(" + l + ", " + r + ")";
        }
        if ((b->op == TokenType::EqualEqual || b->op == TokenType::NotEqual) && lstr && rstr) {
            const std::string cmp = "(sinter_string_equals(" + str_cast(expr(b->left.get()), lt) + ", " +
                                    str_cast(expr(b->right.get()), rt) + ") != 0)";
            return b->op == TokenType::EqualEqual ? cmp : "(!(" + cmp + "))";
        }

        std::string op;
        switch (b->op) {
            case TokenType::Plus: op = "+"; break;
            case TokenType::Minus: op = "-"; break;
            case TokenType::Star: op = "*"; break;
            case TokenType::Slash: op = "/"; break;
            case TokenType::Percent: op = "%"; break;
            case TokenType::EqualEqual: op = "=="; break;
            case TokenType::NotEqual: op = "!="; break;
            case TokenType::Less: op = "<"; break;
            case TokenType::LessEqual: op = "<="; break;
            case TokenType::Greater: op = ">"; break;
            case TokenType::GreaterEqual: op = ">="; break;
            case TokenType::DoubleAmpersand: op = "&&"; break;
            case TokenType::DoublePipe: op = "||"; break;
            default: op = "+";
        }
        return "(" + expr(b->left.get()) + " " + op + " " + expr(b->right.get()) + ")";
    }

    std::string Codegen::expr_unary(const UnaryExpr *u) {
        if (u->op == TokenType::Star) {
            if (auto *id = dynamic_cast<IdentifierExpr *>(u->operand.get());
                id && ref_vars_.count(id->name)) {
                // Explicit deref of a reference: the reference is already an
                // lvalue pointer, so reuse the auto-deref form instead of `*(*r)`.
                return expr_ident(id);
            }
        }
        const std::string op = (u->op == TokenType::Minus)   ? "-"
                               : (u->op == TokenType::Plus)  ? "+"
                               : (u->op == TokenType::Star)  ? "*"
                               : (u->op == TokenType::Ampersand) ? "&"
                                                             : "!";
        return "(" + op + expr(u->operand.get()) + ")";
    }

    std::string Codegen::expr_call(CallExpr *c) {
        // obj.method(args)
        if (auto *m = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
            // 静态method Class.method()
            if (auto *oid = dynamic_cast<IdentifierExpr *>(m->object.get());
                oid && sem_.classes().contains(oid->name) && !locals_.contains(oid->name)) {
                auto fit = sem_.functions().find(m->member);
                if (fit != sem_.functions().end()) {
                    for (auto &f: fit->second) {
                        if (f.is_static && f.owner_class == oid->name) {
                            std::string a;
                            for (size_t i = 0; i < c->args.size(); ++i) {
                                if (i) a += ", ";
                                a += expr(c->args[i].value.get());
                            }
                            if (!f.template_params.empty() && f.decl)
                                return generic_fn_c_name(f, generic_subst_for_call(f.decl, c)) + "(" + a + ")";
                            return f.c_name + "(" + a + ")";
                        }
                    }
                }
            }

            // 作用域函数（调用形式）：recv.let(lambda) / run / apply / also / with
            if (m->member == "let" || m->member == "run" || m->member == "apply"
                || m->member == "also" || m->member == "with") {
                if (!c->args.empty() && dynamic_cast<LambdaExpr *>(c->args[0].value.get())) {
                    auto *lam = dynamic_cast<LambdaExpr *>(c->args[0].value.get());
                    const std::string recv = expr(m->object.get());
                    // 接收者类型提示：让 lambda 的无标注形参拿到真实类型
                    const std::string recv_t = c_type_of_expr(m->object.get());
                    lambda_param_hints_pending_types_.assign(lam->params.size(), recv_t);
                    std::string fn = expr_lambda(lam);
                    std::string call = fn + "(" + recv;
                    for (size_t i = 1; i < c->args.size(); ++i)
                        call += ", " + expr(c->args[i].value.get());
                    call += ")";
                    if (m->member == "apply" || m->member == "also")
                        // 逗号表达式返回最后的接收者本身（类型同接收者）
                        return "(" + call + ", " + recv + ")";
                    return call;
                }
            }

            // 模块限定调用：math.square(5)
            // 对象是裸标识符，既不是本地变量、也不是已知类名，则按模块名解析。
            if (const std::string mod_path = module_path_of(m->object.get()); !mod_path.empty()) {
                const std::string mod = sem_.module_to_ident(mod_path);
                std::vector<CType> qa;
                for (auto &arg: c->args) qa.push_back(arg_type_of(arg.value.get()));
                std::string qerr;
                const FunctionInfo *qfi = sem_.resolve_overload(m->member, qa, &qerr, mod);
                std::string args;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i) args += ", ";
                    if (qfi && i < qfi->params.size())
                        args += upcast_if_needed(c->args[i].value.get(), qfi->params[i].type_c);
                    else
                        args += expr(c->args[i].value.get());
                }
                if (qfi) {
                    for (size_t i = c->args.size(); i < qfi->params.size(); ++i) {
                        if (!args.empty()) args += ", ";
                        if (qfi->decl && i < qfi->decl->params.size() && qfi->decl->params[i].default_value)
                            args += expr(qfi->decl->params[i].default_value.get());
                        else
                            args += "0";
                    }
                    std::string qcname = qfi->c_name;
                    if (!qfi->template_params.empty() && qfi->decl) {
                        // 单态化名字与定义侧共用同一套规则
                        qcname = generic_fn_c_name(*qfi, generic_subst_for_call(qfi->decl, c));
                    }
                    return qcname + "(" + args + ")";
                }
                return mod + "_" + m->member + "(" + args + ")";
            }

            const std::string obj_type = c_type_of_expr(m->object.get());
            std::string obj = expr(m->object.get());
            // 链式调用 `sb.append(a).append(b)` 的接收者是一个有副作用的表达式。
            // C 代码里虚表查找要写两次接收者（取 __vptr 一次、传参一次），
            // 不先落到临时变量就会把 append(a) 执行两遍。
            if (has_side_effect(m->object.get()) && !obj_type.empty()) {
                const std::string tmp = fresh("__recv");
                emit(obj_type + " " + tmp + " = " + obj + ";");
                obj = tmp;
            }

            // 空安全调用 obj?.method(...)：先判空，再走普通成员调用
            if (m->null_safe && null_safe_depth_ == 0) {
                null_safe_depth_++;
                const std::string plain = expr_call(c);   // 此时 null_safe 已被计数屏蔽
                null_safe_depth_--;
                // ?: 两侧类型必须一致：字符串用空值，其它用 0
                const std::string ct = c_type_of_expr(c);
                const std::string zero = (ct == "sinter_string")
                                             ? "sinter_string_from_literal(\"\", 0)"
                                             : "0";
                return "((" + obj + ") ? " + plain + " : " + zero + ")";
            }

            std::string owner_class;
            for (auto &[cn, ci]: sem_.classes())
                if ("struct " + ci.c_name + "*" == obj_type) { owner_class = cn; break; }

            // 编译器生成的复制构造 / 移动构造：obj.copy() / obj.move()
            // 只在没有同名用户方法时生效（data class 自己生成 copy 方法后，
            // `p.copy()` 应走用户方法而不是拷贝构造）。
            if ((m->member == "copy" || m->member == "move") && c->args.empty()) {
                bool user_method = false;
                if (const auto fit5 = sem_.functions().find(m->member); fit5 != sem_.functions().end()) {
                    for (auto &f: fit5->second)
                        if (f.is_method && f.owner_class == owner_class) { user_method = true; break; }
                }
                const ClassInfo *oc = owner_class.empty() ? nullptr
                                                          : sem_.find_class(owner_class);
                if (oc && !user_method)
                    return oc->c_name + "_" + m->member + "(" + expr(m->object.get()) + ")";
            }

            // 显式析构：obj.del() -> <类>_del(obj)
            if (m->member == "del" && c->args.empty() && !owner_class.empty()) {
                const ClassInfo *oc = sem_.find_class(owner_class);
                if (oc) return oc->c_name + "_del(" + expr(m->object.get()) + ")";
            }

            // 类名当命名空间用：`Int.MAX_VALUE()` / `I64.MAX_VALUE()`。
            // 语言没有 companion object，常量就写成 struct 上的无参方法；
            // 这里把 `Class.method()` 派发成静态调用（不带隐式接收者）。
            // 函数名按语义层登记的实际符号取（含模块前缀），不自己拼。
            if (auto *oid = dynamic_cast<IdentifierExpr *>(m->object.get());
                oid && sem_.classes().contains(oid->name) && !locals_.contains(oid->name)) {
                auto sfit = sem_.functions().find(m->member);
                if (sfit != sem_.functions().end()) {
                    for (auto &f: sfit->second) {
                        if (f.owner_class != oid->name) continue;
                        if (f.is_static) {
                            std::string sargs;
                            for (size_t i = 0; i < c->args.size(); ++i) {
                                if (i) sargs += ", ";
                                sargs += expr(c->args[i].value.get());
                            }
                            return f.c_name + "(" + sargs + ")";
                        }
                        // 非静态：`Class.method()` 等价于对「类实参」的调用。
                        // 常量用法的唯一形态是 `Type.zero()`（无接收者），
                        // 语义层把它当静态处理；这里生成 `Type_method(NULL, ...)`
                        // 会解引用空指针，所以直接报错更诚实。
                        sem_.report_error("`" + oid->name + "." + m->member +
                                              "()` needs a receiver; declare it `static fun` to call it on the type",
                                          c->line, c->col);
                        return "0";
                    }
                }
            }

            std::string a;
            for (size_t i = 0; i < c->args.size(); ++i) {
                if (i) a += ", ";
                a += expr(c->args[i].value.get());
            }

            auto fit = sem_.functions().find(m->member);
            // 扩展函数：x.twice() -> <模块>_twice(x)
            if (fit != sem_.functions().end()) {
                // `T?` 的接收者是 `T*`，调扩展方法要自动解一层 `*`：
                // 语义层已经按去星后的名字放行，这里必须生成 `f(*x)`，
                // 否则把 `sinter_string*` 当 `sinter_string` 传进 C 函数。
                //
                // 这里只有 C 侧名（`obj_type` 是 `sinter_string*`），所以比较时
                // 把 `ext_receiver`（Sinter 名）也翻成 C 名，两边都在 C 域上对齐。
                bool deref_recv = false;
                std::string recv_c = obj_type;
                if (!recv_c.empty() && recv_c.back() == '*') {
                    recv_c.pop_back();
                    deref_recv = true;
                }
                for (auto &f: fit->second) {
                    if (f.ext_receiver.empty()) continue;
                    const std::string want = sem_.resolve_type_name(f.ext_receiver).to_c();
                    if (f.ext_receiver != owner_class && !owner_class.empty()) {
                        // 接收者类型按 C 类型名匹配（ext_receiver 是 sinter 类型名）。
                        // 先按原样的 C 类型比，再按去星后的比（可空接收者 `T?`）。
                        if (want != obj_type && !(deref_recv && want == recv_c)) continue;
                    }
                    const std::string recv_expr =
                        (deref_recv && want == recv_c && want != obj_type) ? "(*" + obj + ")" : obj;
                    std::string ea = recv_expr;
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        ea += ", ";
                        ea += expr(c->args[i].value.get());
                    }
                    return f.c_name + "(" + ea + ")";
                }
            }
            if (fit != sem_.functions().end()) {
                for (auto &f: fit->second) {
                    if (!f.is_method) continue;
                    bool reachable = false;
                    std::string cur = owner_class;
                    int guard = 0;
                    // 记录从 owner_class 走到 `cur` 时穿过了几层 `__base` 内嵌，
                    // 以及每层的 C 结构体名（用来在实参上逐层取 `__base`）。
                    std::vector<std::string> chain;
                    while (!cur.empty() && guard++ < 32) {
                        if (f.owner_class == cur) { reachable = true; break; }
                        const ClassInfo *cc = sem_.find_class(cur);
                        if (!cc) break;
                        // 继承是「以 __base 内嵌」而不是 C 的指针转换：调用基类
                        // 方法时必须把 `struct Derived*` 换成 `struct Derived*` 里
                        // 内嵌的 `struct Base` 的地址（`&obj->__base`）。否则
                        // 基类方法按自己的字段偏移读派生类的对象头，直接越界
                        // （子类 `IntArray : Array<i64>` 上实测：`sum`/`sort`
                        // 全部编译不过或被当成无接收者的裸调用）。
                        chain.push_back(cc->c_name);
                        cur = cc->base_class;
                    }
                    if (!reachable && !f.ext_receiver.empty() && f.ext_receiver == owner_class) reachable = true;
                    if (!reachable) continue;

                    // 接收者表达式：两级形式
                    //   recv    —— 作为实参传给方法（基类方法要「内嵌子对象」的地址）
                    //   recv_v  —— 用来访问 `__vptr`（基类方法要用内嵌子对象本身）
                    // 继承是「以 __base 内嵌」而不是 C 的指针转换：直接传 `Derived*`
                    // 给 `Base*` 形参是类型不匹配（实测 gcc 直接报错）。
                    std::string recv = obj;
                    std::string recv_v = obj;
                    if (f.owner_class != owner_class) {
                        std::string acc = obj;
                        for (size_t k = 0; k + 1 < chain.size(); ++k)
                            acc += "->__base";
                        // `__base` 是内嵌的**值**，`X->__base` 拿到的是结构体本身：
                        //   - 访问 vtable 槽要写成 `(&(X->__base))->__vptr[i]`
                        //   - 作为实参传给基类方法同样要取地址 `(&(X->__base))`
                        // 两者一模一样，所以只需要一个表达式。
                        recv = "(&" + acc + "->__base)";
                        recv_v = recv;
                    }

                    // 方法默认参数：缺省实参在**接收者上下文**里求值，
                    // 所以 `copy(x = self.x)` 里的 self/this 要指向 obj。
                    std::string aa;
                    {
                        const auto saved_locals = locals_;
                        const auto saved_types = local_types_;
                        if (!f.is_static) {
                            locals_["this"] = obj;
                            local_types_["this"] = obj_type;
                            locals_["self"] = obj;
                            local_types_["self"] = obj_type;
                        }
                        // 命名实参：按 f.params 的名字重排；否则保持位置参数。
                        std::vector<Expression *> ordered(f.params.size(), nullptr);
                        bool any_named = false;
                        size_t next_pos = 0;
                        for (auto &arg: c->args) {
                            if (!arg.name.empty()) any_named = true;
                            size_t slot = static_cast<size_t>(-1);
                            if (arg.name.empty()) {
                                while (next_pos < ordered.size() && ordered[next_pos]) ++next_pos;
                                slot = next_pos;
                            } else {
                                for (size_t k = 0; k < f.params.size(); ++k)
                                    if (f.params[k].name == arg.name) { slot = k; break; }
                            }
                            if (slot < ordered.size()) ordered[slot] = arg.value.get();
                        }
                        if (any_named) {
                            for (size_t i = 0; i < ordered.size(); ++i) {
                                std::string piece;
                                if (ordered[i]) {
                                    piece = upcast_if_needed(ordered[i], f.params[i].type_c);
                                } else if (f.decl && i < f.decl->params.size() &&
                                           f.decl->params[i].default_value) {
                                    piece = expr(f.decl->params[i].default_value.get());
                                } else {
                                    piece = "0";
                                }
                                if (!aa.empty()) aa += ", ";
                                aa += piece;
                            }
                        } else {
                            aa = a;
                            for (size_t i = c->args.size(); i < f.params.size(); ++i) {
                                std::string piece = "0";
                                if (f.decl && i < f.decl->params.size() && f.decl->params[i].default_value)
                                    piece = expr(f.decl->params[i].default_value.get());
                                if (!aa.empty()) aa += ", ";
                                aa += piece;
                            }
                        }
                        locals_ = saved_locals;
                        local_types_ = saved_types;
                    }

                    // 泛型方法：单态化后是普通函数，接收者作为第一个实参
                    if (!f.template_params.empty() && f.decl) {
                        const std::string gname = generic_fn_c_name(f, generic_subst_for_call(f.decl, c));
                        if (f.is_static) return gname + "(" + aa + ")";
                        std::string callargs = recv;
                        if (!aa.empty()) callargs += ", " + aa;
                        return gname + "(" + callargs + ")";
                    }

                    const int slot = f.vtable_slot;
                    // `self.xxx()` 在自己类的方法体里要**静态派发**：直接调用本类的实现，不经虚表。
                    // 否则 `Shape.describe()` 里的 `self.area()` 会按接收者的真实类型
                    // 分派回自己（`__parent` 指向最外层对象，其 vtable 里 describe
                    // 仍是 `Shape.describe`），形成无限递归 —— 实测就是栈溢出崩溃。
                    // 判定只看接收者是不是 `self`：方法体里对自身的调用一律直接编址，
                    // 与 Kotlin/Java 的「非虚调用」一致（子类覆盖只影响外部调用）。
                    bool self_static_call = false;
                    if (auto *rid = dynamic_cast<IdentifierExpr *>(m->object.get())) {
                        // 只有「调用自己正在生成的这个方法」才需要直接编址 ——
                        // 否则 `Shape.describe()` 里的 `self.describe()` 会经虚表
                        // 再回到自己，无限递归。调用**别的**方法（如 describe 里
                        // 调 area）必须走虚派发，这才是多态的正确语义：
                        // 子类覆写 area 后，基类 describe 里看到的也该是子类实现。
                        if ((rid->name == "self" || rid->name == "this") &&
                            f.owner_class == current_class_ && f.name == current_method_)
                            self_static_call = true;
                    }
                    if (slot >= 0 && f.is_virtual && !f.is_static && !self_static_call) {
                        std::string sig = f.return_type_name + "(*)(void*";
                        for (auto &p: f.params) sig += ", " + p.type_c;
                        sig += ")";
                        // 虚调用传**接收者自身**：`obj` 的静态类型就是虚表
                        // 槽位号的来源（`slot` 由静态类型解析），所以 `obj`
                        // 必然就是那张表对应的基类子对象指针，也正是 thunk
                        // 形参 `struct Base* __self` 期待的东西。
                        //
                        // 传 `obj->__parent` 是错的：`Shape* s` 会收到 `Rect*`，
                        // 上线后按 `Shape` 偏移读字段就是越界（实测崩溃）。
                        // 「找回最外层对象」的职责在 thunk 内部，不在调用点。
                        std::string callargs = recv;
                        if (!aa.empty()) callargs += ", " + aa;
                        // 虚表槽是 void*，取出来再 memcpy 到函数指针变量，
                        // 避免 ISO C 禁止的对象指针 -> 函数指针强转。
                        return "((" + sig + ")" + recv_v + "->__vptr[" + std::to_string(slot) + "])(" + callargs + ")";
                    }
                    std::string callargs = recv;
                    if (!aa.empty()) callargs += ", " + aa;
                    return f.c_name + "(" + callargs + ")";
                }
            }
            // `b.block()`：block 是接收者函数类型的局部变量（编码为
            // `void (*)(Button*)`），把接收者作为第一个实参：`block(b)`。
            if (locals_.contains(m->member)) {
                const auto lt = local_types_.find(m->member);
                if (lt != local_types_.end() && lt->second.find("(*)") != std::string::npos) {
                    std::string callargs = obj;
                    if (!a.empty()) callargs += ", " + a;
                    return locals_[m->member] + "(" + callargs + ")";
                }
            }
            return m->member + "(" + a + ")";
        }

        if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get())) {
            const std::string name = id->name;
            // 局部函数变量优先于同名全局函数（参数名 `f` 不能被子模块里的 `f` 抢走）。
            if (locals_.contains(name)) {
                const auto lt = local_types_.find(name);
                if (lt != local_types_.end() && lt->second.find("(*)") != std::string::npos) {
                    std::string args;
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        if (i) args += ", ";
                        args += expr(c->args[i].value.get());
                    }
                    return locals_[name] + "(" + args + ")";
                }
            }
            // 类型参数分派：`sinter_gen_clone(v)` 在 `List<str>` 的实例里落到
            // `sinter_gen_clone_str(v)`。归约不出来就按原名继续走常规解析。
            if (is_generic_dispatch_call(name)) {
                // 末位若是类型短名，就是编译期标签：不生成实参，只用于选函数名。
                size_t argc = c->args.size();
                std::string tag;
                size_t tag_idx = static_cast<size_t>(-1);
                if (argc > 0) {
                    if (auto *lid = dynamic_cast<IdentifierExpr *>(c->args[argc - 1].value.get())) {
                        if (looks_like_type_token(lid->name)) {
                            tag = lid->name;
                            --argc;
                        } else {
                            // 标签写成模板形参名（`..., B`）：按该形参的位置取实例的类型实参。
                            // 形参表来自单态化时登记的 GenericTable（current_class_ 的实例），
                            // 不能再依赖 cur_type_params_ —— 实例方法生成阶段它可能是空的。
                            std::vector<TemplateParam> tps = cur_type_params_;
                            if (generic_table_ && !current_class_.empty())
                                generic_table_->params_of(current_class_, tps);
                            for (size_t k = 0; k < tps.size(); ++k) {
                                if (tps[k].name == lid->name) {
                                    tag_idx = k;
                                    --argc;
                                    break;
                                }
                            }
                        }
                    }
                }
                const std::string saved_tag = tag_token_;
                const size_t saved_idx = tag_param_idx_;
                tag_token_ = tag;
                tag_param_idx_ = tag_idx;
                const std::string dn = generic_dispatch_name_at(name, tag_idx == static_cast<size_t>(-1) ? 0 : tag_idx);
                tag_token_ = saved_tag;
                tag_param_idx_ = saved_idx;
                if (!dn.empty() && dn != name) {
                    if (getenv("SINTER_DEBUG_DISPATCH"))
                        std::cerr << "[call] " << name << " -> " << dn
                                  << " tag=" << tag << " cur_class=" << current_class_ << "\n";
                    std::string dargs;
                    for (size_t i = 0; i < argc; ++i) {
                        if (i) dargs += ", ";
                        dargs += expr(c->args[i].value.get());
                    }
                    return dn + "(" + dargs + ")";
                }
            }
            // 作用域函数 lambda 中：裸方法名 name(...) 解析到接收者
            if (getenv("SINTER_DEBUG_CALL2"))
                std::cerr << "[bare] name=" << name << " recv='" << lambda_receiver_
                          << "' hasLocal=" << (locals_.contains(lambda_receiver_) ? 1 : 0)
                          << " type='" << (local_types_.count(lambda_receiver_)
                                               ? local_types_[lambda_receiver_]
                                               : "?") << "' funcs="
                          << (sem_.functions().count(name) ? (int) sem_.functions().at(name).size() : -1)
                          << "\n";
            if (!lambda_receiver_.empty() && locals_.contains(lambda_receiver_)) {
                const std::string recv_type = local_types_.count(lambda_receiver_)
                                                  ? local_types_[lambda_receiver_]
                                                  : std::string();
                if (!recv_type.empty() && recv_type.back() == '*') {
                    auto fit4 = sem_.functions().find(name);
                    if (fit4 != sem_.functions().end()) {
                        for (auto &f: fit4->second) {
                            if (!f.is_method) continue;
                            const ClassInfo *fc = sem_.find_class(f.owner_class);
                            if (!fc) continue;
                            // 方法可能定义在基类：recv_type 必须是 f.owner_class 或其派生类
                            if (!is_subclass_of_type(recv_type,
                                                     "struct " + fc->c_name + "*"))
                                continue;
                            std::string a4;
                            for (size_t i = 0; i < c->args.size(); ++i) {
                                if (i) a4 += ", ";
                                a4 += expr(c->args[i].value.get());
                            }
                            const std::string recv = locals_[lambda_receiver_];
                            if (f.vtable_slot >= 0 && f.is_virtual) {
                                std::string sig4 = f.return_type_name + "(*)(void*";
                                for (auto &pp: f.params) sig4 += ", " + pp.type_c;
                                sig4 += ")";
                                return "((" + sig4 + ")" + recv + "->__vptr[" +
                                       std::to_string(f.vtable_slot) + "])(" + recv +
                                       (a4.empty() ? "" : ", " + a4) + ")";
                            }
                            return f.c_name + "(" + recv + (a4.empty() ? "" : ", " + a4) + ")";
                        }
                    }
                }
            }
            // 模块限定调用：math.square(x) -> 解析到 math 模块的同名函数
            const auto last_dot = name.rfind('.');
            if (last_dot != std::string::npos && !sem_.classes().contains(name)) {
                const std::string mod = sem_.module_to_ident(name.substr(0, last_dot));
                const std::string base = name.substr(last_dot + 1);
                std::vector<CType> qa;
                for (auto &arg: c->args) qa.push_back(arg_type_of(arg.value.get()));
                std::string qerr;
                const FunctionInfo *qfi = sem_.resolve_overload(base, qa, &qerr, mod);
                std::string args;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i) args += ", ";
                    args += expr(c->args[i].value.get());
                }
                if (qfi) {
                    for (size_t i = c->args.size(); i < qfi->params.size(); ++i) {
                        if (!args.empty()) args += ", ";
                        if (qfi->decl && i < qfi->decl->params.size() && qfi->decl->params[i].default_value)
                            args += expr(qfi->decl->params[i].default_value.get());
                        else
                            args += "0";
                    }
                    std::string qcname = qfi->c_name;
                    if (!qfi->template_params.empty() && qfi->decl) {
                        // 单态化名字与定义侧共用同一套规则（模块前缀 + snake 方法名 + 类型后缀）
                        qcname = generic_fn_c_name(*qfi, generic_subst_for_call(qfi->decl, c));
                    }
                    return qcname + "(" + args + ")";
                }
                // 未在目标模块找到：回退为模块前缀 + 原名，交给 C 编译器报错
                return mod + "_" + base + "(" + args + ")";
            }

            // lambda 接收者：无接收者语法糖的 `name()` 视作 `recv->name()`
            if (!lambda_receiver_.empty() && locals_.contains(lambda_receiver_)) {
                const std::string recv_type = local_types_.count(lambda_receiver_)
                                                  ? local_types_[lambda_receiver_]
                                                  : std::string();
                if (!recv_type.empty() && recv_type.back() == '*') {
                    auto fit3 = sem_.functions().find(name);
                    if (fit3 != sem_.functions().end()) {
                        for (auto &f: fit3->second) {
                            if (!f.is_method) continue;
                            const ClassInfo *fc = sem_.find_class(f.owner_class);
                            if (!fc) continue;
                            if ("struct " + fc->c_name + "*" != recv_type) continue;
                            std::string a3;
                            for (size_t i = 0; i < c->args.size(); ++i) {
                                if (i) a3 += ", ";
                                a3 += expr(c->args[i].value.get());
                            }
                            const std::string recv = locals_[lambda_receiver_];
                            if (f.vtable_slot >= 0 && f.is_virtual) {
                                std::string sig3 = f.return_type_name + "(*)(void*";
                                for (auto &pp: f.params) sig3 += ", " + pp.type_c;
                                sig3 += ")";
                                return "((" + sig3 + ")" + recv + "->__vptr[" +
                                       std::to_string(f.vtable_slot) + "])(" + recv +
                                       (a3.empty() ? "" : ", " + a3) + ")";
                            }
                            return f.c_name + "(" + recv + (a3.empty() ? "" : ", " + a3) + ")";
                        }
                    }
                }
                // 字段为可调用值（函数指针）时直接调用
                if (!recv_type.empty() && recv_type.back() == '*') {
                    // 落在成员字段：recv->name()
                    for (auto &[cn, ci]: sem_.classes()) {
                        if ("struct " + ci.c_name + "*" != recv_type) continue;
                        for (auto &fld: ci.fields)
                            if (fld.name == name) return locals_[lambda_receiver_] + "->" + fld.c_name + "()";
                    }
                }
            }

            if (getenv("SINTER_DEBUG_CALL2"))
                std::cerr << "[proto] name='" << name << "' recv='" << lambda_receiver_
                          << "'\n";
            // 类构造
            if (sem_.classes().contains(name)) {
                const auto &ci = class_by_name(name);
                // 构造函数的声明（取默认值用）：ctor_params 只有类型，默认值在 decl 上
                const FunctionDecl *ctor_decl = nullptr;
                if (auto hit = sem_.functions().find(name); hit != sem_.functions().end()) {
                    for (auto &f: hit->second) {
                        if (f.is_method && f.owner_class == ci.name && f.decl &&
                            f.params.size() == ci.ctor_params.size()) {
                            ctor_decl = f.decl;
                            break;
                        }
                    }
                }
                // 1) 先按形参名把实参摆到位置上（`Counter(name = "x")` 只给了第 2 个）
                std::vector<Expression *> slot(ci.ctor_params.size(), nullptr);
                size_t next_pos = 0;
                for (auto &arg: c->args) {
                    size_t k = static_cast<size_t>(-1);
                    if (arg.name.empty()) {
                        while (next_pos < slot.size() && slot[next_pos]) ++next_pos;
                        k = next_pos;
                    } else {
                        for (size_t j = 0; j < ci.ctor_params.size(); ++j)
                            if (ci.ctor_params[j].name == arg.name) { k = j; break; }
                    }
                    if (k < slot.size()) slot[k] = arg.value.get();
                }
                // 2) 逐位生成：有实参用实参，没有就用声明的默认值，再没有就 0
                std::string a;
                for (size_t i = 0; i < slot.size(); ++i) {
                    if (i) a += ", ";
                    if (slot[i]) {
                        a += upcast_if_needed(slot[i], ci.ctor_params[i].type_c);
                    } else if (ci.decl && i < ci.decl->ctor_params.size() &&
                               ci.decl->ctor_params[i].default_value) {
                        a += expr(ci.decl->ctor_params[i].default_value.get());
                    } else if (ctor_decl && i < ctor_decl->params.size() &&
                               ctor_decl->params[i].default_value) {
                        a += expr(ctor_decl->params[i].default_value.get());
                    } else {
                        a += "0";
                    }
                }
                return ci.c_name + "_new(" + a + ")";
            }

            // 实参类型（用于重载解析）
            std::vector<CType> arg_types;
            for (auto &arg: c->args) arg_types.push_back(arg_type_of(arg.value.get()));

            std::string err;
            // 先按本模块解析，再按任意模块解析（跨模块/隐式依赖）
            const FunctionInfo *fi = nullptr;
            bool any_named = false;
            for (auto &arg: c->args) if (!arg.name.empty()) { any_named = true; break; }
            if (any_named) {
                auto try_named = [&](bool local_only) -> const FunctionInfo * {
                    for (auto &[fname, bucket]: sem_.functions()) {
                        if (fname != name) continue;
                        for (auto &f: bucket) {
                            if (f.is_method || f.is_varargs) continue;
                            if (local_only && sem_.module_to_ident(f.module_name) != module_ident_) continue;
                            std::vector<CType> mapped;
                            if (!map_named_types(c->args, f.params, f.decl, arg_types, mapped)) continue;
                            const FunctionInfo *hit = sem_.resolve_overload(name, mapped);
                            if (hit && hit->params.size() == f.params.size()) return hit;
                        }
                    }
                    return nullptr;
                };
                fi = try_named(true);
                if (!fi) fi = try_named(false);
            }
            if (!fi) fi = sem_.resolve_overload(name, arg_types, &err, module_ident_);
            if (!fi) fi = sem_.resolve_overload(name, arg_types, &err);

            std::string a;
            // 可变参数：末位形参接收全部剩余实参 —— 用 C11 复合字面量
            // 打包成临时数组，再追加元素个数。整个展开是单个表达式，
            // 不需要往语句流里插入任何临时变量。
            if (fi && fi->is_varargs) {
                const size_t fixed = fi->params.empty() ? 0 : fi->params.size() - 1;
                // 泛型类型参数包：逐参数展开（与定义侧/原型侧一致），
                // 不打包数组、不传个数，因此混型包也能正确传递。
                bool gen_pack = false;
                if (fi->decl) {
                    for (auto &tp: fi->decl->tparams) {
                        if (!tp.is_pack) continue;
                        if (fixed < fi->decl->params.size() && fi->decl->params[fixed].type &&
                            fi->decl->params[fixed].type->name == tp.name)
                            gen_pack = true;
                    }
                }
                if (gen_pack) {
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        if (i) a += ", ";
                        a += expr(c->args[i].value.get());
                    }
                    const std::string vn = generic_fn_c_name(*fi, generic_subst_for_call(fi->decl, c));
                    return vn + "(" + a + ")";
                }
                std::string elem = fi->params.empty() ? std::string("long long")
                                                      : fi->varargs_elem_type();
                // Generic variadic `e: T...`: substitute the element type so the
                // compound literal uses the concrete C type.
                if (!fi->template_params.empty() && fi->decl) {
                    const auto gs = generic_subst_for_call(fi->decl, c);
                    if (const auto hit = gs.find(elem); hit != gs.end()) elem = hit->second;
                }
                for (size_t i = 0; i < c->args.size() && i < fixed; ++i) {
                    if (i) a += ", ";
                    a += upcast_if_needed(c->args[i].value.get(), fi->params[i].type_c);
                }
                const size_t n = c->args.size() > fixed ? c->args.size() - fixed : 0;
                if (fixed) a += ", ";
                if (n == 0) {
                    a += "0, 0";
                } else {
                    a += "(" + elem + "[]){";
                    for (size_t i = 0; i < n; ++i) {
                        if (i) a += ", ";
                        a += upcast_if_needed(c->args[fixed + i].value.get(), elem);
                    }
                    a += "}, " + std::to_string(n);
                }
                std::string vname = fi->c_name;
                if (!fi->template_params.empty() && fi->decl)
                    vname = generic_fn_c_name(*fi, generic_subst_for_call(fi->decl, c));
                return vname + "(" + a + ")";
            }
            bool any_lambda_arg = false;
            for (auto &arg: c->args)
                if (dynamic_cast<LambdaExpr *>(arg.value.get())) { any_lambda_arg = true; break; }
            // 重载解析失败且实参里有 lambda：先别生成（否则拿不到接收者/返回类型提示），
            // 等下面 fallback 找到函数后统一按形参重建。
            const bool defer_args = !fi && any_lambda_arg;
            for (size_t i = 0; !defer_args && i < c->args.size(); ++i) {
                if (i) a += ", ";
                if (fi && i < fi->params.size()) {
                    const std::string saved_hint = lambda_receiver_type_hint_;
                    if (fi->decl && i < fi->decl->params.size()) {
                        const auto *pt = fi->decl->params[i].type.get();
                        if (pt && pt->kind == TypeAnnotation::FunctionType &&
                            dynamic_cast<LambdaExpr *>(c->args[i].value.get())) {
                            if (pt->receiver_type) lambda_receiver_type_hint_ = c_type(pt->receiver_type.get());
                            lambda_return_type_hint_ = pt->inner_type ? c_type(pt->inner_type.get()) : "void";
                        }
                    }
                    a += upcast_if_needed(c->args[i].value.get(), fi->params[i].type_c);
                    lambda_receiver_type_hint_ = saved_hint;
                } else {
                    // 重载解析失败时，按唯一同名函数的形参类型做继承向上转型
                    std::string want;
                    auto fit2 = sem_.functions().find(name);
                    if (fit2 != sem_.functions().end() && fit2->second.size() == 1 &&
                        i < fit2->second.front().params.size())
                        want = fit2->second.front().params[i].type_c;
                    a += upcast_if_needed(c->args[i].value.get(), want);
                }
            }

            if (!fi) {
                auto fit = sem_.functions().find(name);
                if (fit != sem_.functions().end()) {
                    // 兜底只挑自由函数：同名方法不能在没有接收者的情况下被调用，
                    // 否则 `add(3,4)` 会误撞上 `List.add`，生成丢接收者的裸调用。
                    for (auto &cand: fit->second) {
                        if (!cand.is_method) { fi = &cand; break; }
                    }
                }
            }
            // 未在本模块声明、但其它模块已有同名函数：按用户示例语义直接解析到该模块
            if (!fi) {
                std::string oerr;
                const FunctionInfo *cross = nullptr;
                for (auto &[cn, ci]: sem_.classes()) {
                    (void) cn; (void) ci;
                }
                // 逐个模块尝试解析
                for (auto &[fname, bucket]: sem_.functions()) {
                    if (fname != name) continue;
                    for (auto &cand: bucket) {
                        if (cand.is_method) continue;
                        const auto mid = sem_.module_to_ident(cand.module_name);
                        const FunctionInfo *hit = sem_.resolve_overload(name, arg_types, &oerr, mid);
                        if (hit) { cross = hit; break; }
                    }
                    if (cross) break;
                }
                if (cross) fi = cross;
            }
            // 首次 resolve 失败（例如接收者 lambda 的实参类型是 fn）时，
            // 上面的参数循环被跳过；拿到 fallback 的 fi 后按真实形参重建 a，
            // 这样接收者 lambda 的 hint 才能生效。
            if (fi && c->args.size() > 0 && a.empty()) {
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i) a += ", ";
                    if (i < fi->params.size()) {
                        const std::string saved_hint = lambda_receiver_type_hint_;
                        const std::string saved_ret = lambda_return_type_hint_;
                        if (fi->decl && i < fi->decl->params.size()) {
                            const auto *pt = fi->decl->params[i].type.get();
                            if (pt && pt->kind == TypeAnnotation::FunctionType &&
                                dynamic_cast<LambdaExpr *>(c->args[i].value.get())) {
                                if (pt->receiver_type) lambda_receiver_type_hint_ = c_type(pt->receiver_type.get());
                                lambda_return_type_hint_ = pt->inner_type ? c_type(pt->inner_type.get()) : "void";
                            }
                        }
                        a += upcast_if_needed(c->args[i].value.get(), fi->params[i].type_c);
                        lambda_receiver_type_hint_ = saved_hint;
                        lambda_return_type_hint_ = saved_ret;
                    } else {
                        a += expr(c->args[i].value.get());
                    }
                }
            }
            if (!fi) {
                if (getenv("SINTER_DEBUG_CALL2"))
                    std::cerr << "[unresolved] name='" << name << "' recv='"
                              << lambda_receiver_ << "' args=" << c->args.size() << "\n";                // 未声明：方法调用可能来自作用域函数的 lambda（裸方法名）
                if (getenv("SINTER_DEBUG_CLASS"))
                    std::cerr << "[call] unresolved " << name << " recv='"
                              << lambda_receiver_ << "' hasLocal="
                              << (locals_.contains(lambda_receiver_) ? 1 : 0) << "\n";
                if (a.empty()) {
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        if (i) a += ", ";
                        a += expr(c->args[i].value.get());
                    }
                }
                if (!lambda_receiver_.empty() && locals_.contains(lambda_receiver_))
                    return locals_[lambda_receiver_] + "->" + name + "(" + a + ")";
                // 其余情况原样输出，交给 C 编译器报错
                return name + "(" + a + ")";
            }

            std::string cname = fi->c_name;
            // 命名参数：`intro("grace", city = "nyc")` 调用方按名字给了实参，
            // 但 `a` 是按位置拼出来的 —— 直接补默认值会错位（以前生成
            // `functions_intro("grace", "nyc", "unknown")`，把 city 塞进 age）。
            // 这里按形参名把实参重排成位置顺序，顺带算出哪些位置真的缺省。
            if (!fi->params.empty()) {
                bool any_named = false;
                for (auto &arg: c->args)
                    if (!arg.name.empty()) { any_named = true; break; }
                if (any_named) {
                    std::vector<Expression *> ordered(fi->params.size(), nullptr);
                    std::vector<char> used(fi->params.size(), 0);
                    size_t next_pos = 0;
                    for (auto &arg: c->args) {
                        size_t slot = static_cast<size_t>(-1);
                        if (arg.name.empty()) {
                            while (next_pos < ordered.size() && ordered[next_pos]) ++next_pos;
                            slot = next_pos;
                        } else {
                            for (size_t k = 0; k < fi->params.size(); ++k)
                                if (fi->params[k].name == arg.name) { slot = k; break; }
                        }
                        if (slot >= ordered.size()) {
                            // 名字对不上任何形参：保留原顺序拼接，让后面的诊断接手
                            any_named = false;
                            break;
                        }
                        ordered[slot] = arg.value.get();
                        used[slot] = 1;
                    }
                    if (any_named) {
                        std::string na;
                        for (size_t k = 0; k < ordered.size(); ++k) {
                            std::string piece;
                            if (ordered[k]) {
                                piece = upcast_if_needed(ordered[k], fi->params[k].type_c);
                            } else if (fi->decl && k < fi->decl->params.size() &&
                                       fi->decl->params[k].default_value) {
                                piece = expr(fi->decl->params[k].default_value.get());
                            } else {
                                piece = "0";
                            }
                            if (!na.empty()) na += ", ";
                            na += piece;
                        }
                        if (!fi->template_params.empty() && fi->decl)
                            cname = generic_fn_c_name(*fi, generic_subst_for_call(fi->decl, c));
                        return cname + "(" + na + ")";
                    }
                }
            }
            // 默认参数补齐（实参都按位置给出时）
            for (size_t i = c->args.size(); i < fi->params.size(); ++i) {
                if (!a.empty()) a += ", ";
                if (fi->decl && i < fi->decl->params.size() && fi->decl->params[i].default_value)
                    a += expr(fi->decl->params[i].default_value.get());
                else
                    a += "0";
            }
            // 泛型单态化：显式类型实参优先，其次按形参位置从实参推断
            if (!fi->template_params.empty() && fi->decl)
                cname = generic_fn_c_name(*fi, generic_subst_for_call(fi->decl, c));
            return cname + "(" + a + ")";
        }

        const std::string callee = expr(c->callee.get());
        std::string a;
        for (size_t i = 0; i < c->args.size(); ++i) {
            if (i) a += ", ";
            a += expr(c->args[i].value.get());
        }
        return callee + "(" + a + ")";
    }

    std::string Codegen::expr_member(const MemberAccessExpr *m) {
        if (auto *id = dynamic_cast<IdentifierExpr *>(m->object.get())) {
            if (id->name == "this") return expr_ident(id) + "->" + m->member;
            // `Color.Red`：限定名枚举变体。必须在类/静态字段分支之前处理 ——
            // 它既不是类也不是静态字段，落到后面会被拼成 `Color.Red` 这种不存在的 C。
            if (const std::string ev = enum_variant_c_name(id->name, m->member); !ev.empty()) return ev;
            if (sem_.classes().contains(id->name) && !locals_.contains(id->name)) {
                auto &ci = class_by_name(id->name);
                return ci.c_name + "_s_" + Semantic::to_snake_ident(m->member);
            }
        }
        const std::string obj = expr(m->object.get());
        const std::string ot = c_type_of_expr(m->object.get());
        if (ot.empty() || ot.back() != '*') return obj + "." + m->member;
        if (m->null_safe && null_safe_depth_ == 0) {
            null_safe_depth_++;
            const std::string plain = expr_member(m);
            null_safe_depth_--;
            const std::string ft = c_type_of_expr(const_cast<MemberAccessExpr *>(m));
            const std::string zero = (ft == "sinter_string")
                                         ? "sinter_string_from_literal(\"\", 0)"
                                         : "0";
            return "((" + obj + ") ? " + plain + " : " + zero + ")";
        }
        // 字段可能定义在基类：生成 obj->__base...(->__base)->field
        std::string cur;
        for (auto &[cn, ci]: sem_.classes())
            if ("struct " + ci.c_name + "*" == ot) { cur = cn; break; }
        if (cur.empty())
            for (auto &[cn, ci]: sem_.external_classes())
                if ("struct " + ci.c_name + "*" == ot) { cur = cn; break; }
        std::string access = obj;
        int guard = 0;
        while (!cur.empty() && guard++ < 32) {
            const ClassInfo *cc = sem_.find_class(cur);
            if (!cc) break;
            for (auto &f: cc->fields)
                if (f.name == m->member) return access + "->" + f.c_name;
            if (cc->base_class.empty()) break;
            access += "->__base";
            cur = cc->base_class;
        }
        return obj + "->" + m->member;
    }

    std::string Codegen::expr_index(const IndexAccessExpr *i) {
        // 类型参数包逐参数展开之后，`xs[0]` 不再是数组下标，
        // 而是指向一个独立形参 `xs0`（定义侧已在 locals_ 里登记）。
        if (auto *id = dynamic_cast<IdentifierExpr *>(i->object.get())) {
            if (auto *lit = dynamic_cast<LiteralExpr *>(i->index.get());
                lit && lit->lit == LiteralExpr::Int) {
                const auto k = std::to_string(std::get<int64_t>(lit->value));
                if (auto it = locals_.find(id->name + "[" + k + "]"); it != locals_.end())
                    return it->second;
            }
        }
        return expr(i->object.get()) + "[" + expr(i->index.get()) + "]";
    }

    std::string Codegen::expr_assign(const AssignmentExpr *a) {
        // 复制/移动赋值重载：类里定义了 `operator=`（`fun operator=(other: T)` /
        // `(other: T&&)`）就调用它，否则保持默认的 C 赋值（结构体逐字节拷贝）。
        //
        // 方法调用约定是「self 在前」，所以生成 `X_assign(lhs, rhs)`；
        // 只有确实存在该重载时才改写，避免给普通赋值引入一次多余调用。
        {
            const std::string tt = c_type_of_expr(a->target.get());
            if (tt.rfind("struct ", 0) == 0) {
                std::string cn = tt.substr(7);
                while (!cn.empty() && cn.back() == '*') cn.pop_back();
                const ClassInfo *ci = nullptr;
                for (const auto &kv: sem_.classes()) {
                    if (kv.second.c_name == cn) {
                        ci = &kv.second;
                        break;
                    }
                }
                if (ci) {
                    for (const auto &kv: sem_.functions()) {
                        if (kv.first != "operator=") continue;
                        for (const auto &fi: kv.second) {
                            if (!fi.is_method || fi.owner_class != ci->name) continue;
                            return fi.c_name + "(" + expr(a->target.get()) + ", " +
                                   expr(a->value.get()) + ")";
                        }
                    }
                }
            }
        }
        // 字符串目标：先释放目标持有的旧缓冲，再把新值搬进去。
        // 走辅助函数而不是逗号表达式 —— C11 的逗号表达式里不能声明变量。
        if (auto *id = dynamic_cast<IdentifierExpr *>(a->target.get())) {
            const std::string tn = expr_ident(id);
            const auto it = local_types_.find(id->name);
            if (it != local_types_.end() && it->second == "sinter_string") {
                return "sinter_string_assign(&" + tn + ", " + expr(a->value.get()) + ")";
            }
            if (it != local_types_.end() && it->second == "sinter_string*") {
                // `str&`: write through the reference with the string-aware assignment
                // helper so the old buffer is released instead of overwritten.
                return "sinter_string_assign(" + locals_[id->name] + ", " + expr(a->value.get()) + ")";
            }
        }
        if (auto *m = dynamic_cast<MemberAccessExpr *>(a->target.get()))
            return expr_member(m) + " = " + expr(a->value.get());
        if (auto *id = dynamic_cast<IdentifierExpr *>(a->target.get()))
            return expr_ident(id) + " = " + expr(a->value.get());
        return expr(a->target.get()) + " = " + expr(a->value.get());
    }

    std::string Codegen::expr_lambda(LambdaExpr *l) {
        const std::string fname = fresh("__lambda_" + module_ident_);
        auto saved_locals = locals_;
        auto saved_types = local_types_;

        const std::string recv_type = lambda_receiver_type_hint_;
        // 形参类型：显式标注优先；否则按调用点推断（scope call 会预填）
        std::vector<std::string> ptypes;
        for (size_t i = 0; i < l->params.size(); ++i) {
            auto &p = l->params[i];
            std::string pt = p.type ? c_type(p.type.get()) : std::string();
            if (pt.empty() && i < lambda_param_hints_pending_types_.size())
                pt = lambda_param_hints_pending_types_[i];   // 调用点给出的接收者类型
            if (pt.empty()) pt = "long long";
            ptypes.push_back(pt);
            // C 侧形参名统一 snake_case（与函数/方法形参同一规则）
            const std::string cn = Semantic::to_snake_ident(p.name);
            locals_[p.name] = cn;
            local_types_[p.name] = pt;
            // 作用域函数的接收者即第 1 个形参：让它同时可按 this/self 访问
            if (i == 0) {
                locals_["this"] = cn;
                local_types_["this"] = pt;
                locals_["self"] = cn;
                local_types_["self"] = pt;
            }
        }

        // 接收者必须先就位：下面推断返回类型 / 生成函数体都要用它
        const std::string saved_receiver = lambda_receiver_;
        lambda_receiver_ = l->params.empty() ? std::string() : l->params[0].name;
        if (!recv_type.empty()) {
            locals_["this"] = "__recv";
            local_types_["this"] = recv_type;
            locals_["self"] = "__recv";
            local_types_["self"] = recv_type;
            lambda_receiver_ = "this";
        }

        const std::string saved_ret_hint = lambda_return_type_hint_;
        std::string ret = lambda_return_type_hint_.empty() ? "long long" : lambda_return_type_hint_;
        if (!lambda_return_type_hint_.empty()) {
            // 调用点形参给出了返回类型：块 lambda 必须以它为准
        } else if (!l->is_block) {
            if (auto *e = std::get_if<ExprPtr>(&l->body)) ret = c_type_of_expr(e->get());
        } else {
            // 块 lambda：有 return 用返回值类型，最后一条是表达式用它的类型，
            // 否则是 void。尾随 lambda 的典型体是 `println(...)`，必须是 void，
            // 否则会生成 `long long (*)(void)`，传给 `() -> void` 形参时类型不兼容。
            ret = "void";
            if (auto *st = std::get_if<StmtPtr>(&l->body)) {
                if (auto *blk = dynamic_cast<BlockStmt *>(st->get()); blk && !blk->statements.empty()) {
                    if (auto *r = dynamic_cast<ReturnStmt *>(blk->statements.back().get()); r && r->value)
                        ret = c_type_of_expr(r->value.get());
                    else if (auto *ex = dynamic_cast<ExprStmt *>(blk->statements.back().get()))
                        ret = c_type_of_expr(ex->expr.get());
                }
            }
        }

        std::ostringstream decl;
        decl << "static inline " << ret << " " << fname << "(";
        bool first_param = true;
        if (!recv_type.empty()) {
            decl << recv_type << " __recv";
            first_param = false;
        }
        if (l->params.empty() && first_param) decl << "void";
        for (size_t i = 0; i < l->params.size(); ++i) {
            if (!first_param) decl << ", ";
            first_param = false;
            decl << ptypes[i] << " " << Semantic::to_snake_ident(l->params[i].name);
        }
        decl << ") {\n";

        std::ostringstream saved_os = std::move(c_funs_);
        c_funs_ = std::ostringstream{};
        c_funs_ << decl.str();
        const int saved_indent = indent_;
        indent_ = 1;
        if (l->is_block) {
            if (auto *s = std::get_if<StmtPtr>(&l->body)) {
                enter_scope();
                stmt(s->get());
                leave_scope(false);
            }
        } else {
            if (auto *e = std::get_if<ExprPtr>(&l->body))
                emit("return " + expr(e->get()) + ";");
        }
        indent_ = saved_indent;
        c_funs_ << "}\n\n";
        const std::string code = c_funs_.str();
        c_funs_ = std::move(saved_os);
        pending_ << code;

        locals_ = saved_locals;
        local_types_ = saved_types;
        lambda_receiver_ = saved_receiver;
        lambda_param_hints_pending_types_.clear();
        lambda_return_type_hint_ = saved_ret_hint;
        return fname;
    }

    std::string Codegen::expr_ternary(const TernaryExpr *t) {
        if (t->is_elvis) {
            const std::string cond = expr(t->cond.get());
            const std::string cond_t = c_type_of_expr(t->cond.get());
            std::string fallback = expr(t->else_b.get());
            // 字符串的 elvis：判“非空”（len > 0），两侧统一成 sinter_string
            if (cond_t == "sinter_string") {
                fallback = str_cast(fallback, c_type_of_expr(t->else_b.get()));
                return "({ sinter_string __el = " + cond + "; __el.len > 0 ? __el : " + fallback +
                       "; })";
            }
            // 可空字符串 `str?`（C 里是 `sinter_string*`）：左值是指针，右值是值，
            // 直接 `p ? p : v` 会让 C 报 `type mismatch in conditional expression`。
            // 这里按指针判空后解引用，两侧统一成 `sinter_string`。
            if (cond_t == "sinter_string*") {
                fallback = str_cast(fallback, c_type_of_expr(t->else_b.get()));
                return "({ sinter_string* __el = " + cond + "; __el ? *__el : " + fallback + "; })";
            }
            return "(" + cond + " ? " + cond + " : " + fallback + ")";
        }
        // 两侧类型不同但存在继承关系时，统一成基类类型（C 的 ?: 要求两侧兼容）
        std::string lt = c_type_of_expr(t->then_b.get());
        std::string rt = c_type_of_expr(t->else_b.get());
        // 字符串参与的三元：统一成 sinter_string，避免 ?: 两侧类型不一致
        if (lt == "sinter_string" || rt == "sinter_string") {
            return "((" + expr(t->cond.get()) + ") ? " + str_cast(expr(t->then_b.get()), lt) +
                   " : " + str_cast(expr(t->else_b.get()), rt) + ")";
        }
        std::string common;
        auto points_to_class = [&](const std::string &c) {
            if (c.empty() || c.back() != '*') return false;
            return sem_.classes().contains(c) ||
                   [&] {
                       for (auto &[cn, ci]: sem_.classes())
                           if ("struct " + ci.c_name + "*" == c) return true;
                       for (auto &[cn, ci]: sem_.external_classes())
                           if ("struct " + ci.c_name + "*" == c) return true;
                       return false;
                   }();
        };
        if (points_to_class(lt) && points_to_class(rt) && lt != rt) {
            if (is_subclass_of_type(lt, rt)) common = rt;
            else if (is_subclass_of_type(rt, lt)) common = lt;
        }
        // 一侧是 null 字面量：公共类型取另一侧（null 直接给 NULL，不做 ->__base）
        auto is_null_lit = [](Expression *x) {
            if (auto *le = dynamic_cast<LiteralExpr *>(x)) return le->lit == LiteralExpr::Null;
            return false;
        };
        const bool ln = is_null_lit(t->then_b.get());
        const bool rn = is_null_lit(t->else_b.get());
        if (ln && !rn && points_to_class(rt)) common = rt;
        else if (rn && !ln && points_to_class(lt)) common = lt;
        const std::string a = common.empty() ? expr(t->then_b.get())
                                             : upcast_if_needed(t->then_b.get(), common);
        const std::string b = common.empty() ? expr(t->else_b.get())
                                             : upcast_if_needed(t->else_b.get(), common);
        return "(" + expr(t->cond.get()) + " ? " + a + " : " + b + ")";
    }

    std::string Codegen::expr_if(const IfExpr *i) {
        return "(" + expr(i->cond.get()) + " ? " + expr(i->then_b.get()) + " : " + expr(i->else_b.get()) + ")";
    }

    std::string Codegen::expr_when(WhenExpr *w) {
        const std::string subj = w->subject ? expr(w->subject.get()) : "";
        const std::string subj_t = w->subject ? c_type_of_expr(w->subject.get()) : std::string();
        // `when` 的等值匹配若作用在字符串上，C 里不能用 `==`（sinter_string 是结构体，
        // 比较运算符非法），必须走 sinter_string_equals。比较元素的类型也要一起看：
        // 模式侧是字符串字面量时同样成立。
        const auto is_str_c = [](const std::string &t) {
            return t == "sinter_string" || t == "const char*" || t == "char*";
        };
        const bool subj_is_str = is_str_c(subj_t);
        // 兜底值：没有 `else` 分支时这里要给出一个和分支体**同类型**的值。
        // 以前固定写 `0`，一旦分支体是字符串（`when (c) { ... => "red" }`），
        // 三元表达式就成了 `cond ? sinter_string : 0`，C 报
        // "type mismatch in conditional expression"。字符串兜底给空串。
        const auto zero_for = [&](Expression *e) -> std::string {
            if (!e) return "0";
            const std::string t = c_type_of_expr(e);
            if (is_str_c(t)) return "sinter_string_from_literal(\"\", 0)";
            if (t.empty() || t.back() == '*') return "0";
            return "0";
        };
        std::string acc = w->else_branch ? expr(w->else_branch.get())
                                         : zero_for(w->branches.empty() ? nullptr : w->branches[0].body.get());
        if (w->else_block) {
            if (auto *blk = dynamic_cast<BlockStmt *>(w->else_block.get()); blk && !blk->statements.empty())
                if (auto *ex = dynamic_cast<ExprStmt *>(blk->statements.back().get()))
                    acc = expr(ex->expr.get());
        }

        for (auto it = w->branches.rbegin(); it != w->branches.rend(); ++it) {
            const auto &br = *it;
            std::string conds;
            for (size_t i = 0; i < br.patterns.size(); ++i) {
                const auto &p = br.patterns[i];
                std::string one;
                if (p.mode == WhenExpr::Pattern::Range) {
                    one = "(" + subj + " >= " + expr(p.lo.get()) + " && " + subj +
                          (p.inclusive ? " <= " : " < ") + expr(p.hi.get()) + ")";
                } else if (p.mode == WhenExpr::Pattern::TypeIs) {
                    // 运行时类型链判定：支持子类被基类匹配，和 Java/Kotlin 的 `is` 一致
                    const std::string subj_expr = w->subject ? subj : "0";
                    const std::string subj_c = c_type_of_expr(w->subject.get());
                    if (!subj_c.empty() && subj_c.back() == '*')
                        one = "sinter_type_is((" + subj_expr + ")->__type_chain, \""
                              + p.type_name + "\")";
                    else
                        one = "0";
                } else {
                    if (subj.empty()) {
                        one = expr(p.value.get());
                    } else {
                        const std::string pv = expr(p.value.get());
                        const bool pat_is_str = subj_is_str ||
                                                is_str_c(c_type_of_expr(p.value.get()));
                        if (pat_is_str)
                            one = "sinter_string_equals(" + subj + ", " + pv + ")";
                        else
                            one = "(" + subj + " == " + pv + ")";
                    }
                }
                if (i) conds += " || ";
                conds += one;
            }
            if (conds.empty()) conds = "1";
            std::string body = br.body ? expr(br.body.get()) : "0";
            if (br.body_block) {
                if (auto *blk = dynamic_cast<BlockStmt *>(br.body_block.get()); blk && !blk->statements.empty())
                    if (auto *ex = dynamic_cast<ExprStmt *>(blk->statements.back().get()))
                        body = expr(ex->expr.get());
            }
            acc = "(" + conds + " ? " + body + " : " + acc + ")";
        }
        return acc;
    }

    std::string Codegen::expr_array(ArrayLiteralExpr *a) {
        const std::string ty = a->elements.empty() ? "long long" : c_type_of_expr(a->elements[0].get());
        return expr_array_with(a, ty);
    }

    std::string Codegen::expr_array_with(ArrayLiteralExpr *a, const std::string &elem_c) {
        const std::string n = fresh("__arr");
        const std::string ty = elem_c.empty() ? "long long" : elem_c;
        auto saved = std::move(c_funs_);
        c_funs_ = std::ostringstream{};
        c_funs_ << "static " << ty << " " << n << "[] = {";
        for (size_t i = 0; i < a->elements.size(); ++i) {
            if (i) c_funs_ << ", ";
            // 每个元素都强转成声明的元素类型，避免 int32_t 字面量撞 int64_t 声明
            c_funs_ << "((" << ty << ")" << expr(a->elements[i].get()) << ")";
        }
        c_funs_ << "};\n";
        const std::string code = c_funs_.str();
        c_funs_ = std::move(saved);
        c_funs_ << code;
        return n;
    }

    std::string Codegen::expr_group(GroupingExpr *g) { return "(" + expr(g->expr.get()) + ")"; }

    std::string Codegen::expr_pipeline(PipelineExpr *p) {
        const std::string left = expr(p->left.get());

        if (auto *c = dynamic_cast<CallExpr *>(p->right.get())) {
            if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get())) {
                std::string a = left;
                for (auto &arg: c->args) a += ", " + expr(arg.value.get());
                // 管道左侧实参参与重载解析
                std::vector<CType> ats;
                ats.push_back(arg_type_of(p->left.get()));
                for (auto &arg: c->args) ats.push_back(arg_type_of(arg.value.get()));
                std::string perr;
                const FunctionInfo *pfi = sem_.resolve_overload(id->name, ats, &perr, module_ident_);
                if (!pfi) pfi = sem_.resolve_overload(id->name, ats, &perr);
                if (pfi) {
                    std::string cname = pfi->c_name;
                    if (!pfi->template_params.empty() && pfi->decl) {
                        // 管道左侧实参等价于调用的第一个实参，构造一个等价 CallExpr
                        // 让类型推断与普通调用共用同一套规则。
                        CallExpr synth;
                        Argument first("", nullptr);
                        synth.args.push_back(first);
                        for (auto &arg: c->args)
                            synth.args.emplace_back(arg.name, clone_expr(arg.value.get()));
                        cname = generic_fn_c_name(*pfi, generic_subst_for_call(pfi->decl, &synth));
                    }
                    return cname + "(" + a + ")";
                }
                // 重载解析没命中时不能退回裸名字：那样会生成 `sum3(...)`，
                // 既丢了模块前缀（跨模块时链接期才炸），又绕过了重载分派。
                // 改成按名字找唯一候选，用它的真实 C 名；实在找不到才报错。
                {
                    auto fit = sem_.functions().find(id->name);
                    if (fit != sem_.functions().end() && fit->second.size() == 1)
                        return fit->second.front().c_name + "(" + a + ")";
                    throw std::runtime_error(
                        "line " + std::to_string(c->line) + ": cannot resolve pipeline call `" +
                        id->name + "` with the piped value as its first argument" +
                        (perr.empty() ? std::string() : " (" + perr + ")"));
                }
            }
            if (auto *m = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
                std::string a = expr(m->object.get()) + ", " + left;
                for (auto &arg: c->args) a += ", " + expr(arg.value.get());
                return m->member + "(" + a + ")";
            }
        }
        if (auto *id = dynamic_cast<IdentifierExpr *>(p->right.get())) {
            if (sem_.functions().contains(id->name)) {
                auto &bucket = sem_.functions().at(id->name);
                if (!bucket.empty()) return bucket.front().c_name + "(" + left + ")";
            }
            return id->name + "(" + left + ")";
        }
        if (auto *l = dynamic_cast<LambdaExpr *>(p->right.get()))
            return expr_lambda(l) + "(" + left + ")";
        return expr(p->right.get()) + "(" + left + ")";
    }

    bool Codegen::has_side_effect(const Expression *e) const {
        if (!e) return false;
        switch (e->kind) {
            case Expression::Call:
            case Expression::Assignment: return true;
            case Expression::Binary: {
                auto *b = static_cast<const BinaryExpr *>(e);
                return has_side_effect(b->left.get()) || has_side_effect(b->right.get());
            }
            case Expression::Unary: return has_side_effect(static_cast<const UnaryExpr *>(e)->operand.get());
            case Expression::NonNull: return has_side_effect(static_cast<const NonNullExpr *>(e)->operand.get());
            case Expression::Grouping:
                return has_side_effect(static_cast<const GroupingExpr *>(e)->expr.get());
            case Expression::Ternary: {
                auto *t = static_cast<const TernaryExpr *>(e);
                return has_side_effect(t->cond.get()) || has_side_effect(t->then_b.get()) ||
                       has_side_effect(t->else_b.get());
            }
            case Expression::MemberAccess:
                return has_side_effect(static_cast<const MemberAccessExpr *>(e)->object.get());
            case Expression::IndexAccess: {
                auto *i = static_cast<const IndexAccessExpr *>(e);
                return has_side_effect(i->object.get()) || has_side_effect(i->index.get());
            }
            case Expression::StringTemplate: {
                for (auto &pt: static_cast<const StringTemplateExpr *>(e)->parts)
                    if (has_side_effect(pt.expr.get())) return true;
                return false;
            }
            default: return false;
        }
    }

    std::string Codegen::expr_template(StringTemplateExpr *s) {
        // 生成 sinter_string：字面片段直接以长度构造，避免运行期 strlen
        if (s->parts.empty()) return "sinter_string_from_literal(\"\", 0)";
        std::string acc;
        bool first = true;
        bool acc_is_temp = false;
        for (auto &pt: s->parts) {
            std::string piece;
            // 片段是否「一定是刚构造的临时值」。变量/字段/下标是借用，绝不动。
            bool piece_is_temp = true;
            if (pt.expr) {
                const std::string pt_type = c_type_of_expr(pt.expr.get());
                // 有副作用的片段（`${xs.removeLast()}` / `${xs.size()}`）必须先按
                // 出现顺序落到临时变量：直接嵌进 concat(...) 的实参里，C 的求值
                // 顺序未定义，编译器可能先算 size 再算 removeLast，字符串模板就
                // 显示成删除前的长度（实测 `pop=gamma len=3`）。
                if (has_side_effect(pt.expr.get()) && pt_type != "void") {
                    const std::string tmp = fresh("__tmplv");
                    emit(pt_type + " " + tmp + " = " + expr(pt.expr.get()) + ";");
                    piece = str_cast(tmp, pt_type);
                    piece_is_temp = true;
                } else {
                    piece = str_cast(expr(pt.expr.get()), pt_type);
                    const bool is_str = pt_type == "sinter_string" || pt_type == "const char*" ||
                                        pt_type == "char*";
                    piece_is_temp = !is_str || !is_borrowed_expr(pt.expr.get());
                }
            } else {
                // 片段里的转义同样要按解码后的字节算长度（同 expr_literal）。
                piece = literal_from_source_body(pt.text);
            }
            if (first) { acc = piece; acc_is_temp = piece_is_temp; first = false; }
            else {
                // 只接管确认是临时值的一侧；借用的一侧保留，后面还要用。
                if (acc_is_temp && piece_is_temp)
                    acc = "sinter_string_concat_own(" + acc + ", " + piece + ")";
                else if (acc_is_temp)
                    acc = "sinter_string_concat_take_left(" + acc + ", " + piece + ")";
                else if (piece_is_temp)
                    acc = "sinter_string_concat_take_right(" + acc + ", " + piece + ")";
                else
                    acc = "sinter_string_concat(" + acc + ", " + piece + ")";
                acc_is_temp = true;
            }
        }
        return acc;
    }

    /// variable / 字段 / 下标是「借用」，调用方后面还会用到它们，不能顺手释放。
    bool Codegen::is_borrowed_expr(const Expression *e) {
        return dynamic_cast<const IdentifierExpr *>(e) || dynamic_cast<const MemberAccessExpr *>(e) ||
               dynamic_cast<const IndexAccessExpr *>(e);
    }

    const std::string *Codegen::hoist_name_of(const Expression *e) const {
        auto it = hoist_names_.find(e);
        return it == hoist_names_.end() ? nullptr : &it->second;
    }

    /// 实参是否值得提升：求值为字符串、不是借用（变量 / 字段 / 下标）、
    /// 也不是没有堆缓冲的字符串字面量。
    /// collect_hoist_targets 与 needs_hoist 共用这一处判定：两边规则一旦分叉，
    /// 就会出现「预判说需要提升、实际却没提升」的错配。
    bool Codegen::is_hoistable_arg(const Expression *e) {
        if (!e) return false;
        if (c_type_of_expr(const_cast<Expression *>(e)) != "sinter_string") return false;
        if (is_borrowed_expr(e)) return false;
        if (auto *lit = dynamic_cast<const LiteralExpr *>(e))
            if (lit->lit == LiteralExpr::String) return false;
        return true;
    }

    /// 收集需要提升的实参，后序排列（子树先于本层）。
    ///
    /// 只扫顶层 `println(x + y)` 不够：`std.str.len(join(a, b))` 里的 `join(a, b)`
    /// 是内层调用的实参，同样需要释放，所以对整棵表达式树走一遍，命中即入列。
    ///
    /// 只走「一定会求值」的位置：三元分支、`&&` / `||` 的右侧、`when` 分支都是
    /// 条件求值，提前落地会把没选中的分支也执行一遍（副作用翻倍），
    /// 宁可不提升也不改语义；数组字面量也不进，元素是 memcpy 进去的指针副本，
    /// 提升后释放会给数组留下悬空指针。
    void Codegen::collect_hoist_targets(Expression *e, std::vector<Expression *> &out) {
        if (!e) return;
        if (auto *c = dynamic_cast<CallExpr *>(e)) {
            for (auto &arg: c->args) {
                Expression *a = arg.value.get();
                if (!a) continue;
                collect_hoist_targets(a, out);   // 后序：先排子树里的临时值
                if (is_hoistable_arg(a)) out.push_back(a);
            }
            collect_hoist_targets(c->callee.get(), out);
            return;
        }
        if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
            collect_hoist_targets(b->left.get(), out);
            if (b->op != TokenType::DoubleAmpersand && b->op != TokenType::DoublePipe)
                collect_hoist_targets(b->right.get(), out);
            return;
        }
        if (auto *g = dynamic_cast<GroupingExpr *>(e)) {
            collect_hoist_targets(g->expr.get(), out);
            return;
        }
        if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) {
            collect_hoist_targets(m->object.get(), out);
            return;
        }
        if (auto *i = dynamic_cast<IndexAccessExpr *>(e)) {
            collect_hoist_targets(i->object.get(), out);
            collect_hoist_targets(i->index.get(), out);
            return;
        }
        if (auto *t = dynamic_cast<TernaryExpr *>(e)) {
            collect_hoist_targets(t->cond.get(), out);   // 分支是条件求值，不进去
            return;
        }
        if (auto *a = dynamic_cast<AssignmentExpr *>(e)) {
            collect_hoist_targets(a->value.get(), out);
            return;
        }
        if (auto *u = dynamic_cast<UnaryExpr *>(e)) {
            collect_hoist_targets(u->operand.get(), out);
            return;
        }
        if (auto *p = dynamic_cast<PipelineExpr *>(e)) {
            collect_hoist_targets(p->left.get(), out);
            collect_hoist_targets(p->right.get(), out);
            return;
        }
        if (auto *st = dynamic_cast<StringTemplateExpr *>(e)) {
            for (auto &part: st->parts)
                if (part.expr) collect_hoist_targets(part.expr.get(), out);
            return;
        }
        if (auto *sc = dynamic_cast<ScopeCallExpr *>(e)) {
            // 接收者一定会求值；体是独立语句块，发射到自己那轮时再处理
            collect_hoist_targets(sc->receiver.get(), out);
            return;
        }
    }

    /// 纯谓词：这棵树里有没有需要提升的实参。用于发射语句之前选生成策略。
    bool Codegen::needs_hoist(const Expression *e) {
        std::vector<Expression *> targets;
        collect_hoist_targets(const_cast<Expression *>(e), targets);
        return !targets.empty();
    }

    /// 临时字符串实参的落地。
    ///
    /// 场景：`println("reversed: " + acc)`。这里拼接出来的字符串由谁释放？
    /// 变量会被 RAII 接管，返回值所有权会移交，但「作为实参传出去的临时值」
    /// 在 C 里没有析构点。做法是把这样的实参先赋给一个局部变量，
    /// 语句执行完在调用点之后逐个 free —— 相当于给这段 C 补上作用域。
    std::vector<std::string> Codegen::hoist_temp_str_args(Expression *e) {
        // 一次语句只保留一份映射：上一条语句登记的旧名字必须丢掉，
        // 否则本轮同一节点的文本会被替换成上一轮的变量名（那个作用域早就不在了）。
        hoist_names_.clear();
        std::vector<Expression *> targets;
        collect_hoist_targets(e, targets);
        std::vector<std::string> owned;
        for (Expression *a: targets) {
            if (hoist_names_.contains(a)) continue;   // 同一节点只落一次
            const std::string tmp = fresh("__tmpstr");
            const std::string code = expr(a);
            emit("sinter_string " + tmp + " = " + code + ";");
            hoist_names_[a] = tmp;
            owned.push_back(tmp);
        }
        return owned;
    }

    /// 释放已提升的临时字符串并置空（幂等，供各语句收尾复用）。
    void Codegen::emit_string_free(const std::string &n) {
        emit("sinter_string_free(" + n + "); " + n + ".data = 0; " + n + ".len = 0; " + n +
             ".cap = 0; " + n + ".owned = 0;");
    }

    void Codegen::finish_stmt_temps(const std::vector<std::string> &owned) {
        for (auto &n: owned) emit_string_free(n);
        hoist_names_.clear();
    }

    std::string Codegen::expr_scope_call(ScopeCallExpr *s) {
        const std::string recv = expr(s->receiver.get());
        const std::string rtype = c_type_of_expr(s->receiver.get());

        const std::string tmp = fresh("__sc");
        emit(rtype + " " + tmp + " = " + recv + ";");

        // 块内接收者名：显式 `it ->` 优先，否则用占位名 __it
        const std::string bind = !s->it_name.empty() ? s->it_name : std::string("__it");
        const auto saved_locals = locals_;
        const auto saved_types = local_types_;
        const std::string saved_receiver = lambda_receiver_;

        locals_[bind] = tmp;
        local_types_[bind] = rtype;
        // 块内 `this` / `self` 也指向接收者（Kotlin with/apply 的写法）
        locals_["this"] = tmp;
        local_types_["this"] = rtype;
        locals_["self"] = tmp;
        local_types_["self"] = rtype;
        // 让块内的裸成员 / 裸方法名都解析到接收者
        lambda_receiver_ = bind;

        std::string result = tmp;
        if (s->body) {
            auto *blk = dynamic_cast<BlockStmt *>(s->body.get());
            std::string last_val;
            if (blk && !blk->statements.empty())
                if (auto *ex = dynamic_cast<ExprStmt *>(blk->statements.back().get()))
                    last_val = expr(ex->expr.get());
            if (blk)
                for (auto &st: blk->statements) stmt(st.get());
            // let/run/with 返回块内最后一个表达式的值；apply/also 返回接收者
            if (s->kind_sc != ScopeCallExpr::Apply && s->kind_sc != ScopeCallExpr::Also)
                result = last_val.empty() ? tmp : last_val;
        } else if (s->body_expr) {
            result = expr(s->body_expr.get());
        }

        lambda_receiver_ = saved_receiver;
        locals_ = saved_locals;
        local_types_ = saved_types;
        return result;
    }
} // namespace sinter
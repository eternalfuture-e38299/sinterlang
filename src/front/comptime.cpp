// sinter front: 编译期特性实现
//
// 这一版刻意保持「诚实」：能编译期求值的就折叠，不能求值的就明确报错。
// 绝不再像旧版那样把 comptime 整块静默丢弃——那会让用户以为代码跑了。
//
// 两遍执行（顺序很重要）：
//   1. apply_comptime      泛型单态化之前：折 `Target.*`、裁 `comptime if`。
//      必须在单态化之前，否则被裁掉的分支里的泛型也会被实例化。
//      源位置关键字这一遍**不折**——`__CLASS__` 在泛型模板体里要等到实例
//      （`Box<i64>`）才有最终名字，提前折会把 `Box` 冻进 `Box_i64` 的代码里。
//   2. apply_source_sites  单态化之后、语义分析之前：折 `__FILE__` 等。
//
// 两遍共用同一个带上下文栈的 walker：以前那版只递归进块 / 函数 / 类，
// `when` 分支、`if` 体、lambda 块体里的编译期表达式会被漏掉（漏掉的
// `Target.*` 会一路活到代码生成，被当成 0 输出）。现在按 ast.hpp 的
// 节点种类全量覆盖。
#include "front/comptime.hpp"
#include "front/ast.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace sinter {
    namespace {
        // ---------------------------------------------------------------- 目标默认值
        std::string detect_os() {
#if defined(__ANDROID__)
            return "android";
#elif defined(__linux__)
            return "linux";
#elif defined(_WIN32)
            return "windows";
#elif defined(__APPLE__)
            return "macos";
#else
            return "unknown";
#endif
        }

        std::string detect_arch() {
#if defined(__aarch64__)
            return "arm64";
#elif defined(__arm__)
            return "arm";
#elif defined(__x86_64__)
            return "x86_64";
#elif defined(__i386__)
            return "x86";
#else
            return "unknown";
#endif
        }

        std::string detect_endian() {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
            return "big";
#else
            return "little";
#endif
        }

        // ---------------------------------------------------------------- 构造常量节点
        /// 字符串字面量的正文转义。
        ///
        /// 路径里的 `\` 统一写成 `/`：一方面 Windows 路径不会变成转义序列，
        /// 另一方面 codegen 用「正文长度」当 sinter_string 的 len（已知缺陷：
        /// 转义序列会让长度偏大），少一种转义就少一处偏差。引号与控制字符
        /// 仍然要转义——宁可长度偏大，也不能生成非法 C。
        std::string escape_body(const std::string &s) {
            std::string r;
            r.reserve(s.size() + 8);
            for (const char c: s) {
                switch (c) {
                    case '"': r += "\\\""; break;
                    case '\\': r += '/'; break;
                    case '\n': r += "\\n"; break;
                    case '\r': r += "\\r"; break;
                    case '\t': r += "\\t"; break;
                    default: r += c; break;
                }
            }
            return r;
        }

        ExprPtr make_int(const int64_t v) {
            return std::make_unique<LiteralExpr>(LiteralExpr::Int, std::to_string(v));
        }

        ExprPtr make_bool(const bool v) {
            return std::make_unique<LiteralExpr>(LiteralExpr::Bool, v ? "true" : "false");
        }

        ExprPtr make_string(const std::string &s) {
            return std::make_unique<LiteralExpr>(LiteralExpr::String, "\"" + escape_body(s) + "\"");
        }

        // ---------------------------------------------------------------- 位置上下文
        /// 折叠源位置关键字时需要的「我在哪」。
        /// 每进一层函数 / 类型就更新，出层恢复——因此 `__CLASS__` 在内层函数里
        /// 仍然指向外层的类，和直觉一致。
        struct Site {
            std::string file;       ///< __FILE__
            std::string module;     ///< __MODULE__
            std::string function;   ///< 最近的外层函数 / 方法名（空 = 不在函数里）
            std::string type_name;  ///< 最近的外层 class / struct 名
            bool in_class = false;
            bool in_struct = false;
        };

        /// `file:line:col: ` 前缀；未知位置时退化成更短的形式。
        std::string site_prefix(const std::string &file, const int line, const int col) {
            std::string s = file;
            if (line > 0) {
                if (!s.empty()) s += ':';
                s += std::to_string(line);
                if (col > 0) {
                    s += ':';
                    s += std::to_string(col);
                }
            }
            if (s.empty()) return {};
            return s + ": ";
        }

        /// 把一个编译期关键字解析成字面量。
        /// 返回 false 时 why 里是可读原因（调用方补位置后报错）。
        bool resolve_builtin(const BuiltinExpr &b, const Site &site, LiteralExpr &out,
                             std::string &why) {
            switch (b.which) {
                case BuiltinExpr::File:
                    if (site.file.empty()) {
                        why = "`__FILE__` is unavailable: this module was not loaded from a file";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site.file) + "\"");
                    return true;
                case BuiltinExpr::Module:
                    if (site.module.empty()) {
                        why = "`__MODULE__` is unavailable: this module has no `module` declaration";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site.module) + "\"");
                    return true;
                case BuiltinExpr::Line:
                    if (b.line <= 0) {
                        why = "`__LINE__` has no source position (the node did not come from source)";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::Int, std::to_string(b.line));
                    return true;
                case BuiltinExpr::Column:
                    if (b.col <= 0) {
                        why = "`__COLUMN__` has no source position (the node did not come from source)";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::Int, std::to_string(b.col));
                    return true;
                case BuiltinExpr::Function:
                    if (site.function.empty()) {
                        why = "`__FUNCTION__` is only valid inside a function or method (not inside any function here)";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site.function) + "\"");
                    return true;
                case BuiltinExpr::Class:
                    if (!site.in_class) {
                        why = site.in_struct
                                  ? "`__CLASS__` cannot be used inside struct `" + site.type_name +
                                    "`; use `__STRUCT__` here"
                                  : "`__CLASS__` is only valid inside a class body (not inside any class here)";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site.type_name) + "\"");
                    return true;
                case BuiltinExpr::Struct:
                    if (!site.in_struct) {
                        why = site.in_class
                                  ? "`__STRUCT__` cannot be used inside class `" + site.type_name +
                                    "`; use `__CLASS__` here"
                                  : "`__STRUCT__` is only valid inside a struct body (not inside any struct here)";
                        return false;
                    }
                    out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site.type_name) + "\"");
                    return true;
            }
            why = "unknown compile-time keyword";
            return false;
        }

        // ---------------------------------------------------------------- 编译期求值器
        // 只处理「纯常量」：字面量、Target.*、源位置关键字，以及它们之上的
        // 算术/比较/逻辑/字符串比较。任何带变量、调用、I/O 的表达式一律判定为不可求值。
        struct Evaluator {
            const ComptimeTarget *target = nullptr;
            const Site *site = nullptr;
            /// 已在同一 `comptime { }` 块里求值过的常量（名字 → 字面量）。
            /// 为空时表示没有常量作用域（`comptime if` 的条件、默认值求值）。
            const std::map<std::string, LiteralExpr> *consts = nullptr;
            std::string why;   // 不可求值的原因

            bool eval(const ExprPtr &e, LiteralExpr &out) {
                if (!e) {
                    why = "empty expression";
                    return false;
                }
                // __FILE__ / __LINE__ / __CLASS__ …
                if (auto *b = dynamic_cast<BuiltinExpr *>(e.get())) {
                    if (!site) {
                        why = "compile-time keyword has no usable context here";
                        return false;
                    }
                    return resolve_builtin(*b, *site, out, why);
                }
                // Target.xxx
                if (auto *m = dynamic_cast<MemberAccessExpr *>(e.get())) {
                    auto *o = dynamic_cast<IdentifierExpr *>(m->object.get());
                    if (o && o->name == "Target") {
                        const std::string &f = m->member;
                        if (f == "os") {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(target->os) + "\"");
                            return true;
                        }
                        if (f == "arch") {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(target->arch) + "\"");
                            return true;
                        }
                        if (f == "endian") {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(target->endian) + "\"");
                            return true;
                        }
                        if (f == "pointerBits") {
                            out = LiteralExpr(LiteralExpr::Int, std::to_string(target->pointer_bits));
                            return true;
                        }
                        if (f == "longBits") {
                            out = LiteralExpr(LiteralExpr::Int, std::to_string(target->long_bits));
                            return true;
                        }
                        if (f == "isDebug") {
                            out = LiteralExpr(LiteralExpr::Bool, target->is_debug ? "true" : "false");
                            return true;
                        }
                        why = "unknown `Target` constant: Target." + f;
                        return false;
                    }
                    if (o && o->name == "Site") {
                        // `comptime if (Site.line > 10)` 这类用法：求值器也要认得它。
                        const auto *mm = m;
                        const std::string &f = mm->member;
                        if (f == "file" && !site->file.empty()) {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site->file) + "\"");
                            return true;
                        }
                        if (f == "module" && !site->module.empty()) {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site->module) + "\"");
                            return true;
                        }
                        if (f == "function" && !site->function.empty()) {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site->function) + "\"");
                            return true;
                        }
                        if (f == "class" && site->in_class) {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site->type_name) + "\"");
                            return true;
                        }
                        if (f == "struct" && site->in_struct) {
                            out = LiteralExpr(LiteralExpr::String, "\"" + escape_body(site->type_name) + "\"");
                            return true;
                        }
                        if (f == "line" && mm->line > 0) {
                            out = LiteralExpr(LiteralExpr::Int, std::to_string(mm->line));
                            return true;
                        }
                        if (f == "column" && mm->col > 0) {
                            out = LiteralExpr(LiteralExpr::Int, std::to_string(mm->col));
                            return true;
                        }
                    }
                    why = "member access is not a compile-time constant";
                    return false;
                }
                if (auto *l = dynamic_cast<LiteralExpr *>(e.get())) {
                    out = *l;
                    return true;
                }
                // `comptime { val A = 1; val B = A * 2 }`：同一块内已求值的常量。
                // 只有显式传了 consts 才有这个名字空间；否则给出「不能引用运行时
                // 变量」的准确原因，而不是笼统的「不是常量」。
                if (auto *id = dynamic_cast<IdentifierExpr *>(e.get())) {
                    if (consts) {
                        const auto it = consts->find(id->name);
                        if (it != consts->end()) {
                            out = it->second;
                            return true;
                        }
                    }
                    why = "compile-time evaluation cannot reference runtime variable `" + id->name + "`";
                    return false;
                }
                if (auto *g = dynamic_cast<GroupingExpr *>(e.get())) return eval(g->expr, out);
                if (auto *u = dynamic_cast<UnaryExpr *>(e.get())) {
                    LiteralExpr inner(LiteralExpr::Int, "0");
                    if (!eval(u->operand, inner)) return false;
                    if (u->op == TokenType::Bang) {
                        if (inner.lit != LiteralExpr::Bool) {
                            why = "'!' can only be applied to a compile-time bool";
                            return false;
                        }
                        out = LiteralExpr(LiteralExpr::Bool,
                                          std::get<bool>(inner.value) ? "false" : "true");
                        return true;
                    }
                    if (u->op == TokenType::Minus && inner.lit == LiteralExpr::Int) {
                        out = LiteralExpr(LiteralExpr::Int,
                                          std::to_string(-std::get<int64_t>(inner.value)));
                        return true;
                    }
                    why = "this unary operator is not supported in compile-time evaluation";
                    return false;
                }
                if (auto *b = dynamic_cast<BinaryExpr *>(e.get())) {
                    LiteralExpr l(LiteralExpr::Int, "0"), r(LiteralExpr::Int, "0");
                    if (!eval(b->left, l) || !eval(b->right, r)) {
                        if (why.empty()) why = "one side of this binary expression is not a compile-time constant";
                        return false;
                    }
                    // 字符串 == / !=
                    if (l.lit == LiteralExpr::String && r.lit == LiteralExpr::String) {
                        if (b->op != TokenType::EqualEqual && b->op != TokenType::NotEqual) {
                            why = "compile-time strings only support == / !=";
                            return false;
                        }
                        const bool same = std::get<std::string>(l.value) == std::get<std::string>(r.value);
                        out = LiteralExpr(LiteralExpr::Bool,
                                          (b->op == TokenType::EqualEqual ? same : !same) ? "true" : "false");
                        return true;
                    }
                    // bool 逻辑
                    if (l.lit == LiteralExpr::Bool && r.lit == LiteralExpr::Bool) {
                        const bool lv = std::get<bool>(l.value), rv = std::get<bool>(r.value);
                        bool res = false;
                        if (b->op == TokenType::DoubleAmpersand) res = lv && rv;
                        else if (b->op == TokenType::DoublePipe) res = lv || rv;
                        else if (b->op == TokenType::EqualEqual) res = lv == rv;
                        else if (b->op == TokenType::NotEqual) res = lv != rv;
                        else {
                            why = "compile-time bools only support && / || / == / !=";
                            return false;
                        }
                        out = LiteralExpr(LiteralExpr::Bool, res ? "true" : "false");
                        return true;
                    }
                    if (l.lit != LiteralExpr::Int || r.lit != LiteralExpr::Int) {
                        why = "compile-time arithmetic only supports integers";
                        return false;
                    }
                    const int64_t lv = std::get<int64_t>(l.value), rv = std::get<int64_t>(r.value);
                    int64_t res = 0;
                    bool is_bool = false, bres = false;
                    switch (b->op) {
                        case TokenType::Plus: res = lv + rv; break;
                        case TokenType::Minus: res = lv - rv; break;
                        case TokenType::Star: res = lv * rv; break;
                        case TokenType::Slash:
                            if (rv == 0) {
                                why = "division by zero in compile-time evaluation";
                                return false;
                            }
                            res = lv / rv;
                            break;
                        case TokenType::Percent:
                            if (rv == 0) {
                                why = "modulo by zero in compile-time evaluation";
                                return false;
                            }
                            res = lv % rv;
                            break;
                        case TokenType::EqualEqual: is_bool = true; bres = (lv == rv); break;
                        case TokenType::NotEqual: is_bool = true; bres = (lv != rv); break;
                        case TokenType::Less: is_bool = true; bres = (lv < rv); break;
                        case TokenType::LessEqual: is_bool = true; bres = (lv <= rv); break;
                        case TokenType::Greater: is_bool = true; bres = (lv > rv); break;
                        case TokenType::GreaterEqual: is_bool = true; bres = (lv >= rv); break;
                        default:
                            why = "this binary operator is not supported in compile-time evaluation";
                            return false;
                    }
                    if (is_bool) out = LiteralExpr(LiteralExpr::Bool, bres ? "true" : "false");
                    else out = LiteralExpr(LiteralExpr::Int, std::to_string(res));
                    return true;
                }
                why = "expression is not a compile-time constant (only literals, Target.*, "
                      "__FILE__ / __LINE__ and other compile-time keywords, plus arithmetic on them)";
                return false;
            }
        };

        // ---------------------------------------------------------------- 表达式重写
        // 把 Target.* 折成字面量。返回 nullptr 表示「不是 Target 常量」，调用方保留原节点。
        /// `Site.file` / `Site.line` / `Site.module` / `Site.function` / `Site.class`
        /// / `Site.struct` / `Site.column`：编译期位置对象。
        /// 这是 `__FILE__` 那套宏式关键字的替代写法 —— 读起来就是普通成员访问，
        /// 拼字符串、传参、放进 `comptime` 常量都直接可用。
        ExprPtr fold_site(const MemberAccessExpr *m, const Site &site) {
            auto *o = dynamic_cast<IdentifierExpr *>(m->object.get());
            if (!o || o->name != "Site") return nullptr;
            const std::string &f = m->member;
            if (f == "file") return site.file.empty() ? nullptr : make_string(site.file);
            if (f == "module") return site.module.empty() ? nullptr : make_string(site.module);
            if (f == "function") return site.function.empty() ? nullptr : make_string(site.function);
            if (f == "class") return site.in_class ? make_string(site.type_name) : nullptr;
            if (f == "struct") return site.in_struct ? make_string(site.type_name) : nullptr;
            // 成员访问节点不一定带行列（解析器只在部分节点上填），
            // 退回到对象标识符的位置；两处都没有才放弃折叠。
            const auto *obj = dynamic_cast<IdentifierExpr *>(m->object.get());
            const uint32_t ln = m->line > 0 ? m->line : (obj ? obj->line : 0);
            const uint32_t col = m->col > 0 ? m->col : (obj ? obj->col : 0);
            if (f == "line") return ln > 0 ? make_int(ln) : nullptr;
            if (f == "column") return col > 0 ? make_int(col) : nullptr;
            return nullptr;
        }

        ExprPtr fold_target(Expression *e, const ComptimeTarget &t, const Site *site) {
            auto *m = dynamic_cast<MemberAccessExpr *>(e);
            if (!m) return nullptr;
            if (site) {
                if (auto r = fold_site(m, *site)) return r;
            }
            auto *o = dynamic_cast<IdentifierExpr *>(m->object.get());
            if (!o || o->name != "Target") return nullptr;
            if (m->member == "os") return make_string(t.os);
            if (m->member == "arch") return make_string(t.arch);
            if (m->member == "endian") return make_string(t.endian);
            if (m->member == "pointerBits") return make_int(t.pointer_bits);
            if (m->member == "longBits") return make_int(t.long_bits);
            if (m->member == "isDebug") return make_bool(t.is_debug);
            return nullptr;
        }

        // ---------------------------------------------------------------- 遍历 + 折叠
        /// 带声明上下文的 walker。
        ///
        /// fold_sites=false（第一遍）：只折 Target.*、裁 comptime；源位置关键字
        /// 原样留下（`comptime if` 的条件里由 Evaluator 直接算）。
        /// fold_sites=true（第二遍）：把剩下的源位置关键字折成字面量。
        class Folder {
        public:
            Folder(const ComptimeTarget &target, Site site, const bool fold_sites,
                   std::string &error)
                : target_(target), site_(std::move(site)), fold_sites_(fold_sites),
                  error_(error) {}

            void program(Program &p) { stmts(p.statements); }

            [[nodiscard]] bool failed() const { return !error_.empty(); }

        private:
            const ComptimeTarget &target_;
            Site site_;
            bool fold_sites_;
            std::string &error_;

            // ------------------------------------------------------------ 诊断
            void fail_at(const int line, const int col, std::string msg) {
                if (!error_.empty()) return;
                error_ = site_prefix(site_.file, line, col) + std::move(msg);
            }

            void fail(const Expression &e, std::string msg) {
                fail_at(e.line, e.col, std::move(msg));
            }

            void fail(const Statement &s, std::string msg) {
                fail_at(s.line, s.col, std::move(msg));
            }

            // ------------------------------------------------------------ 表达式
            void expr(ExprPtr &e) {
                if (!e || failed()) return;

                // `Site.struct` 写在函数里这类「位置不成立」的用法：报错，
                // 而不是把 Site.xxx 原样漏进生成的 C 里（那会变成 C 编译错误）。
                if (auto *ma = dynamic_cast<MemberAccessExpr *>(e.get())) {
                    if (auto *oi = dynamic_cast<IdentifierExpr *>(ma->object.get());
                        oi && oi->name == "Site" && fold_sites_ && error_.empty()) {
                        error_ = "`Site." + ma->member +
                                 "` is not available here (wrong nesting: class vs struct vs function)";
                    }
                }
                if (auto f = fold_target(e.get(), target_, &site_)) {
                    e = std::move(f);
                    return;
                }

                if (auto *b = dynamic_cast<BuiltinExpr *>(e.get())) {
                    if (!fold_sites_) return;   // 留给第二遍（位置 / 名字才是最终值）
                    LiteralExpr lit(LiteralExpr::Int, "0");
                    std::string why;
                    if (!resolve_builtin(*b, site_, lit, why)) {
                        fail(*b, std::move(why));
                        return;
                    }
                    auto folded = std::make_unique<LiteralExpr>(lit);
                    folded->line = b->line;
                    folded->col = b->col;
                    e = std::move(folded);
                    return;
                }

                switch (e->kind) {
                    case Expression::Binary: {
                        auto *b = static_cast<BinaryExpr *>(e.get());
                        expr(b->left);
                        expr(b->right);
                        break;
                    }
                    case Expression::Unary: {
                        auto *u = static_cast<UnaryExpr *>(e.get());
                        expr(u->operand);
                        break;
                    }
                    case Expression::Grouping: {
                        auto *g = static_cast<GroupingExpr *>(e.get());
                        expr(g->expr);
                        break;
                    }
                    case Expression::Call: {
                        auto *c = static_cast<CallExpr *>(e.get());
                        expr(c->callee);
                        for (auto &a: c->arguments) expr(a);
                        for (auto &a: c->args) expr(a.value);
                        break;
                    }
                    case Expression::MemberAccess: {
                        auto *m = static_cast<MemberAccessExpr *>(e.get());
                        expr(m->object);
                        break;
                    }
                    case Expression::IndexAccess: {
                        auto *i = static_cast<IndexAccessExpr *>(e.get());
                        expr(i->object);
                        expr(i->index);
                        break;
                    }
                    case Expression::Assignment: {
                        auto *a = static_cast<AssignmentExpr *>(e.get());
                        expr(a->target);
                        expr(a->value);
                        break;
                    }
                    case Expression::Ternary: {
                        auto *t = static_cast<TernaryExpr *>(e.get());
                        expr(t->cond);
                        expr(t->then_b);
                        expr(t->else_b);
                        break;
                    }
                    case Expression::IfExpr: {
                        auto *i = static_cast<IfExpr *>(e.get());
                        expr(i->cond);
                        expr(i->then_b);
                        expr(i->else_b);
                        break;
                    }
                    case Expression::Pipeline: {
                        auto *p = static_cast<PipelineExpr *>(e.get());
                        expr(p->left);
                        expr(p->right);
                        break;
                    }
                    case Expression::ArrayLiteral: {
                        auto *a = static_cast<ArrayLiteralExpr *>(e.get());
                        for (auto &x: a->elements) expr(x);
                        break;
                    }
                    case Expression::Cast: {
                        auto *c = static_cast<CastExpr *>(e.get());
                        expr(c->expr);
                        break;
                    }
                    case Expression::StringTemplate: {
                        auto *s = static_cast<StringTemplateExpr *>(e.get());
                        for (auto &part: s->parts)
                            if (part.expr) expr(part.expr);
                        break;
                    }
                    case Expression::Lambda: {
                        auto *l = static_cast<LambdaExpr *>(e.get());
                        // lambda 的形参默认值属于外层作用域；函数体仍算在
                        // 最近的外层命名函数里（`__FUNCTION__` 不会变成 ""）。
                        for (auto &p: l->params) expr(p.default_value);
                        if (auto *body = std::get_if<ExprPtr>(&l->body)) expr(*body);
                        else if (auto *body = std::get_if<StmtPtr>(&l->body)) stmt(body->get());
                        break;
                    }
                    case Expression::Range: {
                        auto *r = static_cast<RangeExpr *>(e.get());
                        expr(r->start);
                        expr(r->end);
                        break;
                    }
                    case Expression::WhenExpr: {
                        auto *w = static_cast<WhenExpr *>(e.get());
                        expr(w->subject);
                        for (auto &br: w->branches) {
                            for (auto &pt: br.patterns) {
                                expr(pt.value);
                                expr(pt.lo);
                                expr(pt.hi);
                            }
                            expr(br.body);
                            stmt(br.body_block.get());
                        }
                        expr(w->else_branch);
                        stmt(w->else_block.get());
                        break;
                    }
                    case Expression::ScopeCall: {
                        auto *s = static_cast<ScopeCallExpr *>(e.get());
                        expr(s->receiver);
                        expr(s->body_expr);
                        stmt(s->body.get());
                        break;
                    }
                    case Expression::BlockExprKind: {
                        auto *b = static_cast<BlockExpr *>(e.get());
                        expr(b->result);
                        stmt(b->block.get());
                        break;
                    }
                    default:
                        // 字面量 / 标识符 / 已折好的关键字：没有子表达式
                        break;
                }
            }

            // ------------------------------------------------------------ 语句
            void stmts(std::vector<StmtPtr> &list) {
                std::vector<StmtPtr> out;
                out.reserve(list.size());
                for (auto &s: list) {
                    if (!s || failed()) return;
                    StmtPtr cur = std::move(s);
                    // comptime 语句可能被整条替换或删除；替换结果还能再是 comptime。
                    while (cur) {
                        auto *ct = dynamic_cast<ComptimeStmt *>(cur.get());
                        if (!ct) break;
                        // comptime { val ... }：常量块展开进外层作用域。
                        // 必须放在 lower_comptime 之前判断：`comptime { if (...) }`
                        // 里的 if 不是常量声明，is_const_block 会返回 false，
                        // 于是落到下面的条件裁剪分支。
                        if (auto *blk = dynamic_cast<BlockStmt *>(ct->body.get())) {
                            if (is_const_block(*blk)) {
                                if (!expand_const_block(*blk, out)) return;
                                cur = nullptr;
                                break;
                            }
                        }
                        cur = lower_comptime(ct);
                        if (failed()) return;
                    }
                    if (!cur) continue;
                    stmt(cur.get());
                    if (failed()) return;
                    out.push_back(std::move(cur));
                }
                list = std::move(out);
            }

            void stmt(Statement *s) {
                if (!s || failed()) return;
                switch (s->kind) {
                    case Statement::Block: {
                        auto *b = static_cast<BlockStmt *>(s);
                        stmts(b->statements);
                        break;
                    }
                    case Statement::VarDecl: {
                        auto *v = static_cast<VarDeclStmt *>(s);
                        expr(v->init);
                        break;
                    }
                    case Statement::Destructure: {
                        auto *d = static_cast<DestructureStmt *>(s);
                        expr(d->init);
                        break;
                    }
                    case Statement::ExprStmt: {
                        auto *e = static_cast<ExprStmt *>(s);
                        expr(e->expr);
                        break;
                    }
                    case Statement::Return: {
                        auto *r = static_cast<ReturnStmt *>(s);
                        expr(r->value);
                        break;
                    }
                    case Statement::If: {
                        auto *i = static_cast<IfStmt *>(s);
                        expr(i->cond);
                        stmt(i->then_b.get());
                        stmt(i->else_b.get());
                        break;
                    }
                    case Statement::While: {
                        auto *w = static_cast<WhileStmt *>(s);
                        expr(w->cond);
                        stmt(w->body.get());
                        break;
                    }
                    case Statement::For: {
                        auto *f = static_cast<ForStmt *>(s);
                        expr(f->iterable);
                        stmt(f->body.get());
                        break;
                    }
                    case Statement::DoWhile: {
                        auto *d = static_cast<DoWhileStmt *>(s);
                        stmt(d->body.get());
                        expr(d->cond);
                        break;
                    }
                    case Statement::Function: {
                        fn(*static_cast<FunctionDecl *>(s));
                        break;
                    }
                    case Statement::Class: {
                        auto *c = static_cast<ClassDecl *>(s);
                        // 进入类型体：外层的函数上下文不再适用
                        const Site saved = site_;
                        site_.function.clear();
                        site_.type_name = c->name;
                        site_.in_class = !c->is_struct;
                        site_.in_struct = c->is_struct;
                        for (auto &p: c->ctor_params) expr(p.default_value);
                        for (auto &args: c->base_args)
                            for (auto &a: args) expr(a);
                        stmts(c->members);
                        stmt(c->destructor.get());
                        if (c->copy_ctor) fn(*c->copy_ctor);
                        if (c->move_ctor) fn(*c->move_ctor);
                        site_ = saved;
                        break;
                    }
                    case Statement::Struct: {
                        auto *st = static_cast<StructDecl *>(s);
                        const Site saved = site_;
                        site_.function.clear();
                        site_.type_name = st->name;
                        site_.in_class = false;
                        site_.in_struct = true;
                        for (auto &f: st->fields) expr(f.default_value);
                        stmts(st->methods);
                        site_ = saved;
                        break;
                    }
                    case Statement::Interface: {
                        auto *it = static_cast<InterfaceDecl *>(s);
                        for (auto &m: it->methods) stmt(m.get());
                        break;
                    }
                    case Statement::Module: {
                        auto *m = static_cast<ModuleStmt *>(s);
                        if (m->is_block) stmts(m->body);
                        break;
                    }
                    case Statement::Comptime: {
                        auto *c = static_cast<ComptimeStmt *>(s);
                        stmt(c->body.get());
                        break;
                    }
                    case Statement::Del: {
                        auto *d = static_cast<DelStmt *>(s);
                        expr(d->target);
                        break;
                    }
                    case Statement::PubBlock: {
                        auto *p = static_cast<PubBlockStmt *>(s);
                        stmts(p->statements);
                        break;
                    }
                    default:
                        // Break / Continue / Enum / Import / Alias / BindCFunction /
                        // EmbedC：没有需要折叠的表达式
                        break;
                }
            }

            /// 进入一个函数：`__FUNCTION__` 从形参默认值起就是函数名本身。
            void fn(FunctionDecl &f) {
                const Site saved = site_;
                site_.function = f.name;
                for (auto &p: f.params) expr(p.default_value);
                stmt(f.body.get());
                site_ = saved;
            }

            // ------------------------------------------------------------ comptime 语句
            /// 块内全是常量声明（`val` / `const`）→ 可当常量块展开。
            /// 空块等价注释；含 `if` 的块留给 lower_comptime 做条件裁剪。
            static bool is_const_block(const BlockStmt &blk) {
                if (blk.statements.empty()) return true;
                for (const auto &s: blk.statements)
                    if (!dynamic_cast<VarDeclStmt *>(s.get())) return false;
                return true;
            }

            /// `comptime { val A = 1 + 2; val B = A * 3 }`
            ///
            /// 逐条求值后**展开到外层作用域**（不是留在一个 C 块里）：`comptime`
            /// 的语义是「让编译器算完」，这些名字必须能被块后面的代码看到。
            /// 值已经折成字面量，所以生成的 C 就是 `const int64_t a = 3;`——
            /// 编译期算过一次，运行期零成本。
            bool expand_const_block(BlockStmt &blk, std::vector<StmtPtr> &out) {
                std::map<std::string, LiteralExpr> env;
                for (auto &s: blk.statements) {
                    auto *v = dynamic_cast<VarDeclStmt *>(s.get());
                    if (!v) {
                        fail(*s, "a comptime block may only contain compile-time constant declarations (`val` / `const`); "
                                 "use `comptime if` for control flow and move ordinary code outside the block");
                        return false;
                    }
                    Evaluator ev{&target_, &site_, &env, {}};
                    LiteralExpr lit(LiteralExpr::Int, "0");
                    if (!ev.eval(v->init, lit)) {
                        std::string msg = "comptime constant `" + v->name +
                                          "` has an initializer that cannot be evaluated at compile time";
                        if (!ev.why.empty()) msg += "：" + ev.why;
                        fail(*v, std::move(msg));
                        return false;
                    }
                    const int line = v->init ? v->init->line : v->line;
                    const int col = v->init ? v->init->col : v->col;
                    auto folded = std::make_unique<LiteralExpr>(lit);
                    folded->line = line;
                    folded->col = col;
                    v->init = std::move(folded);
                    // 没写类型注解时默认 i64：编译期常量常用于「表 / 容量 / 掩码」，
                    // 按 i32 推断会让 3e9 这种值悄悄溢出（普通 val 保持原有推断）。
                    if (!v->type && lit.lit == LiteralExpr::Int) {
                        auto t = std::make_unique<TypeAnnotation>();
                        t->kind = TypeAnnotation::Named;
                        t->name = "i64";
                        v->type = std::move(t);
                    }
                    env.emplace(v->name, lit);   // 后续常量可以引用它
                    out.push_back(std::move(s));
                }
                return true;
            }

            /// 按条件挑一支；返回 nullptr 表示整条语句被删除。
            StmtPtr pick_branch(IfStmt &iff, const char *what) {
                Evaluator ev{&target_, &site_, nullptr, {}};
                LiteralExpr v(LiteralExpr::Bool, "false");
                if (!ev.eval(iff.cond, v) || v.lit != LiteralExpr::Bool) {
                    std::string msg = what;
                    if (!ev.why.empty()) msg += "：" + ev.why;
                    if (iff.cond) fail(*iff.cond, std::move(msg));
                    else fail(iff, std::move(msg));
                    return nullptr;
                }
                const bool taken = std::get<bool>(v.value);
                StmtPtr chosen = taken ? std::move(iff.then_b) : std::move(iff.else_b);
                return chosen;   // 没有 else 且条件为假 → 整条删除
            }

            StmtPtr lower_comptime(ComptimeStmt *ct) {
                // 形式零：comptime assert(cond[, "msg"]) —— 静态断言。
                // 通过 → 整条删除（不生成任何 C）；失败 → 编译期诊断。
                if (auto *a = dynamic_cast<ComptimeAssertStmt *>(ct->body.get())) {
                    Evaluator ev{&target_, &site_, nullptr, {}};
                    LiteralExpr v(LiteralExpr::Bool, "false");
                    if (!ev.eval(a->cond, v) || v.lit != LiteralExpr::Bool) {
                        std::string msg = "comptime assert condition cannot be evaluated at compile time";
                        if (!ev.why.empty()) msg += "：" + ev.why;
                        if (a->cond) fail(*a->cond, std::move(msg));
                        else fail(*a, std::move(msg));
                        return nullptr;
                    }
                    if (std::get<bool>(v.value)) return nullptr;   // 断言成立
                    std::string msg = "comptime assert failed";
                    if (a->message) {
                        Evaluator mev{&target_, &site_, nullptr, {}};
                        LiteralExpr m(LiteralExpr::String, "\"\"");
                        if (mev.eval(a->message, m) && m.lit == LiteralExpr::String) {
                            // value 存的是带引号的原文（和 LiteralExpr 的 raw 一致），
                            // 诊断里不该再带一层引号。
                            std::string text = std::get<std::string>(m.value);
                            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                                text = text.substr(1, text.size() - 2);
                            msg += "：" + text;
                        } else {
                            msg += " (message is not a compile-time string constant)";
                        }
                    }
                    fail(*a, std::move(msg));
                    return nullptr;
                }

                // 形式一：comptime if cond { ... } else { ... }
                if (auto *iff = dynamic_cast<IfStmt *>(ct->body.get()))
                    return pick_branch(*iff, "comptime if condition cannot be evaluated at compile time");

                // 形式二：comptime { ... }，块内首条若是 if 就按条件裁剪
                if (auto *blk = dynamic_cast<BlockStmt *>(ct->body.get())) {
                    if (blk->statements.empty()) return nullptr;   // 空块等价注释
                    if (auto *inner = dynamic_cast<IfStmt *>(blk->statements.front().get()))
                        return pick_branch(*inner, "the 'if' condition inside this comptime block cannot be evaluated at compile time");
                }

                // 其余 comptime 形式：明确失败，绝不静默丢弃
                fail(*ct, "a comptime block supports only three things: `comptime if` pruning, "
                          "constant blocks `comptime { val ... }`, and `comptime assert`; "
                          "move other statements outside the block (they are never silently dropped)");
                return nullptr;
            }
        };

        bool run_pass(std::vector<GenericModule> &modules, const ComptimeTarget &target,
                      const bool fold_sites, std::string &error) {
            for (auto &m: modules) {
                if (!m.program) continue;
                Site site;
                site.file = m.source_path;
                site.module = m.module_name;
                std::string err;
                Folder folder(target, std::move(site), fold_sites, err);
                folder.program(*m.program);
                if (!err.empty()) {
                    error = "[" + m.module_name + "] " + err;
                    return false;
                }
            }
            return true;
        }
    } // namespace

    // ---------------------------------------------------------------- 公开接口
    ComptimeTarget host_target() {
        ComptimeTarget t;
        t.os = detect_os();
        t.arch = detect_arch();
        t.endian = detect_endian();
        t.pointer_bits = static_cast<int>(sizeof(void *) * 8);
        t.long_bits = static_cast<int>(sizeof(long) * 8);
        return t;
    }

    void ComptimeTarget::apply_defines(const std::vector<std::pair<std::string, std::string> > &defs) {
        for (const auto &kv: defs) {
            const std::string &k = kv.first;
            const std::string &v = kv.second;
            if (k == "os" || k == "target.os") os = v;
            else if (k == "arch" || k == "target.arch") arch = v;
            else if (k == "endian") endian = v;
            else if (k == "pointerBits") pointer_bits = std::stoi(v);
            else if (k == "longBits") long_bits = std::stoi(v);
            else if (k == "isDebug") is_debug = (v != "false" && v != "0");
        }
    }

    bool parse_target_flag(const std::string &arg, ComptimeTarget &t) {
        const auto take = [&](const std::string &prefix, std::string &into) {
            if (arg.rfind(prefix, 0) == 0) {
                into = arg.substr(prefix.size());
                return true;
            }
            return false;
        };
        if (take("--target-os=", t.os)) return true;
        if (take("--target-arch=", t.arch)) return true;
        if (take("--target-endian=", t.endian)) return true;
        if (arg == "--release") {
            t.is_debug = false;
            return true;
        }
        if (arg == "--debug") {
            t.is_debug = true;
            return true;
        }
        return false;
    }

    bool apply_comptime(std::vector<GenericModule> &modules, const ComptimeTarget &target,
                        std::string &error) {
        // 第一遍：不动源位置关键字（泛型实例的名字要到单态化之后才定）。
        return run_pass(modules, target, /*fold_sites=*/false, error);
    }

    bool apply_source_sites(std::vector<GenericModule> &modules, const ComptimeTarget &target,
                            std::string &error) {
        // 第二遍：单态化之后，把 __FILE__ / __LINE__ / __FUNCTION__ / __CLASS__ …
        // 折成字面量。此时泛型实例的声明已经改名（Box<T> → Box_i64），
        // 所以 __CLASS__ 在实例体里拿到的是实例名，而不是模板名。
        return run_pass(modules, target, /*fold_sites=*/true, error);
    }
} // namespace sinter

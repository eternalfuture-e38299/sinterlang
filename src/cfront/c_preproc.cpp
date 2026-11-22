// cfront: c_preproc.cpp —— 轻量 C 预处理器实现
//
// 处理顺序（与标准一致的部分）：
//   1. 条件编译：`#if / #ifdef / #ifndef / #elif / #else / #endif` 决定哪段 token 可见；
//   2. 宏：对象式与函数式宏展开（参数代入、`__VA_ARGS__`、禁止自递归）；
//   3. `#include`：按 `-I` 顺序解析并递归展开，同一文件只展开一次；
//   4. `#error` 记诊断，`#pragma` / `#line` 忽略。
//
// 为什么用「pending 缓冲 + 在指令处 flush」：宏表在文件内会变（define/undef），
// 而函数式宏的实参是「后面若干 token」。边读边展开需要同时看前方 token，
// 所以先把当前可见区间攒起来，遇到会改变宏表的指令时再统一展开，语义上等价。
#include "cfront/c_preproc.hpp"

#include "cfront/c_lexer.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <unordered_map>

namespace sinter::cfront {
    namespace {
        namespace fs = std::filesystem;

        struct Macro {
            bool fnlike = false;
            bool variadic = false;
            std::vector<std::string> params;
            CTokens body;
        };

        [[maybe_unused]] bool is_id_start(const char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        }

        bool is_id_char(const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        }

        std::string read_file(const fs::path &p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return {};
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        std::string dirname_of(const std::string &path) {
            const fs::path p(path);
            const fs::path parent = p.parent_path();
            return parent.empty() ? std::string(".") : parent.string();
        }

        /// 把 `\` + 换行 的续行合并掉（lexer 已把整条指令合成一个 token，
        /// 但正文里仍保留续行符，宏体与条件表达式里必须去掉）。
        std::string drop_continuations(const std::string &s) {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '\n' || s[i + 1] == '\r')) {
                    ++i;
                    if (i < s.size() && s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
                    continue;
                }
                if (s[i] == '\r') continue;
                out += s[i];
            }
            return out;
        }

        std::string trim_left(std::string s) {
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            return s.substr(i);
        }

        long long parse_c_int(const std::string &raw) {
            std::string s = raw;
            // 去掉整数后缀
            while (!s.empty()) {
                const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
                if (c == 'u' || c == 'l') s.pop_back();
                else break;
            }
            if (s.empty()) return 0;
            int base = 10;
            size_t off = 0;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                base = 16;
                off = 2;
            } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
                base = 2;
                off = 2;
            } else if (s.size() > 1 && s[0] == '0') {
                base = 8;
                off = 1;
            }
            long long v = 0;
            for (size_t i = off; i < s.size(); ++i) {
                const char c = s[i];
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0 || d >= base) break;
                v = v * base + d;
            }
            return v;
        }

        /// `#if` 表达式的递归下降求值（C 的整数常量表达式子集）。
        /// 标准要求：未定义标识符按 0 处理；`defined X` / `defined(X)` 先于宏展开求值。
        struct CondEval {
            const CTokens &toks;
            size_t i = 0;
            bool failed = false;

            explicit CondEval(const CTokens &t) : toks(t) {}

            const CToken &peek() const {
                static const CToken kEof{};
                return i < toks.size() ? toks[i] : kEof;
            }

            bool match(const CTokKind k) {
                if (i < toks.size() && toks[i].kind == k) {
                    ++i;
                    return true;
                }
                return false;
            }

            long long prim() {
                if (match(CTokKind::LParen)) {
                    const long long v = ternary();
                    match(CTokKind::RParen);
                    return v;
                }
                if (match(CTokKind::IntLiteral)) return parse_c_int(toks[i - 1].text);
                if (match(CTokKind::CharLiteral)) {
                    const std::string &s = toks[i - 1].text;
                    return s.size() >= 3 ? static_cast<long long>(s[1]) : 0LL;
                }
                if (match(CTokKind::Identifier)) return 0;   // 未定义标识符按 0
                failed = true;
                if (i < toks.size()) ++i;
                return 0;
            }

            long long unary() {
                if (match(CTokKind::Minus)) return -unary();
                if (match(CTokKind::Plus)) return unary();
                if (match(CTokKind::Bang)) return unary() == 0 ? 1 : 0;
                if (match(CTokKind::Tilde)) return ~unary();
                return prim();
            }

            long long binary(int min_prec) {
                long long lhs = unary();
                while (true) {
                    const CTokKind k = peek().kind;
                    int prec = 0;
                    switch (k) {
                        case CTokKind::PipePipe: prec = 1; break;
                        case CTokKind::AmpAmp: prec = 2; break;
                        case CTokKind::Pipe: prec = 3; break;
                        case CTokKind::Caret: prec = 4; break;
                        case CTokKind::Amp: prec = 5; break;
                        case CTokKind::EqualEqual:
                        case CTokKind::NotEqual: prec = 6; break;
                        case CTokKind::Less:
                        case CTokKind::Greater:
                        case CTokKind::LessEqual:
                        case CTokKind::GreaterEqual: prec = 7; break;
                        case CTokKind::Shl:
                        case CTokKind::Shr: prec = 8; break;
                        case CTokKind::Plus:
                        case CTokKind::Minus: prec = 9; break;
                        case CTokKind::Star:
                        case CTokKind::Slash:
                        case CTokKind::Percent: prec = 10; break;
                        default: return lhs;
                    }
                    if (prec < min_prec) return lhs;
                    ++i;
                    const long long rhs = binary(prec + 1);
                    switch (k) {
                        case CTokKind::PipePipe: lhs = (lhs || rhs) ? 1 : 0; break;
                        case CTokKind::AmpAmp: lhs = (lhs && rhs) ? 1 : 0; break;
                        case CTokKind::Pipe: lhs |= rhs; break;
                        case CTokKind::Caret: lhs ^= rhs; break;
                        case CTokKind::Amp: lhs &= rhs; break;
                        case CTokKind::EqualEqual: lhs = (lhs == rhs); break;
                        case CTokKind::NotEqual: lhs = (lhs != rhs); break;
                        case CTokKind::Less: lhs = (lhs < rhs); break;
                        case CTokKind::Greater: lhs = (lhs > rhs); break;
                        case CTokKind::LessEqual: lhs = (lhs <= rhs); break;
                        case CTokKind::GreaterEqual: lhs = (lhs >= rhs); break;
                        case CTokKind::Shl: lhs <<= rhs; break;
                        case CTokKind::Shr: lhs >>= rhs; break;
                        case CTokKind::Plus: lhs += rhs; break;
                        case CTokKind::Minus: lhs -= rhs; break;
                        case CTokKind::Star: lhs *= rhs; break;
                        case CTokKind::Slash: lhs = rhs ? lhs / rhs : 0; break;
                        case CTokKind::Percent: lhs = rhs ? lhs % rhs : 0; break;
                        default: break;
                    }
                }
            }

            long long ternary() {
                const long long c = binary(1);
                if (match(CTokKind::Question)) {
                    const long long a = ternary();
                    match(CTokKind::Colon);
                    const long long b = ternary();
                    return c ? a : b;
                }
                return c;
            }
        };
    } // namespace

    class Preprocessor {
    public:
        explicit Preprocessor(CPreprocOptions opt) : opt_(std::move(opt)) {}

        CPreprocResult run(const std::string &source, const std::string &path) {
            for (auto &kv: opt_.predefined) do_define(kv.first + " " + kv.second);
            stack_.push_back(fs::path(path).string());
            file(source, path, 0);
            stack_.pop_back();
            return std::move(res_);
        }

    private:
        CPreprocOptions opt_;
        CPreprocResult res_;
        std::unordered_map<std::string, Macro> macros_;
        std::set<std::string> once_;      // `#pragma once` 的文件（规范化路径）
        std::vector<std::string> stack_;   // 正在展开的文件（防 A→B→A 无 guard 递归）
        /// 词法结果缓存：同一个头被多个头 `#include` 时只词法分析一次。
        /// 注意缓存的是「token 流」而不是「展开结果」—— 条件编译与宏必须在每次
        /// 包含时重新求值，因为 musl 的 bits/alltypes.h 就是靠 `__NEED_x` 宏
        /// 在多次包含时声明不同内容的（加 guard 反而会漏掉类型）。
        std::map<std::string, CTokens> lexed_;
        CTokens pending_;
        struct Cond {
            bool parent_active = true;
            bool taken = true;
            bool active = true;
        };
        std::vector<Cond> conds_;

        [[nodiscard]] bool active() const { return conds_.empty() || conds_.back().active; }

        void flush() {
            if (pending_.empty()) return;
            CTokens in;
            in.swap(pending_);
            std::set<std::string> hide;
            expand(in, res_.tokens, hide, 0);
        }

        /// `#arg`：把实参的原始拼写包成 C 字符串字面量。
        static std::string stringize(const CTokens &arg) {
            std::string spelling;
            for (size_t i = 0; i < arg.size(); ++i) {
                if (i) spelling += ' ';
                spelling += arg[i].text;
            }
            std::string out = "\"";
            for (const char c: spelling) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
            return out;
        }

        /// `a ## b`：合并相邻 token 的拼写。拼出来的东西可能不再是合法 token，
        /// 本轮不校验，按原样交给语法层。**孤立 `#` / `##` 必须丢掉**：
        /// 让 `##` 流进语法层会在声明中间报 expected ';'（musl 的 tgmath.h 就是
        /// 这样把函数体拼出来的，实测过）。
        static void paste(CTokens &v) {
            CTokens out;
            out.reserve(v.size());
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i].kind == CTokKind::HashHash) {
                    if (!out.empty() && i + 1 < v.size()) {
                        out.back().text += v[i + 1].text;
                        ++i;
                    }
                    continue;
                }
                if (v[i].kind == CTokKind::Hash) continue;
                out.push_back(v[i]);
            }
            v.swap(out);
        }

        void expand(const CTokens &in, CTokens &out, std::set<std::string> &hide, int depth) {
            if (depth > 64) {
                out.insert(out.end(), in.begin(), in.end());
                return;
            }
            for (size_t i = 0; i < in.size(); ++i) {
                const CToken &t = in[i];
                if (t.kind == CTokKind::Identifier && !hide.contains(t.text)) {
                    const auto it = macros_.find(t.text);
                    if (it != macros_.end()) {
                        const Macro &m = it->second;
                        if (!m.fnlike) {
                            std::set<std::string> h2 = hide;
                            h2.insert(t.text);
                            CTokens body = m.body;
                            paste(body);
                            for (auto &b: body) {
                                b.line = t.line;
                                b.column = t.column;
                                b.offset = t.offset;
                            }
                            expand(body, out, h2, depth + 1);
                            continue;
                        }
                        if (i + 1 < in.size() && in[i + 1].kind == CTokKind::LParen) {
                            std::vector<CTokens> args;
                            CTokens cur;
                            int lvl = 0;
                            size_t j = i + 1;
                            bool closed = false;
                            for (; j < in.size(); ++j) {
                                const CToken &a = in[j];
                                if (a.kind == CTokKind::LParen) {
                                    ++lvl;
                                    if (lvl == 1) continue;
                                } else if (a.kind == CTokKind::RParen) {
                                    --lvl;
                                    if (lvl == 0) {
                                        closed = true;
                                        break;
                                    }
                                } else if (a.kind == CTokKind::Comma && lvl == 1) {
                                    args.push_back(cur);
                                    cur.clear();
                                    continue;
                                }
                                cur.push_back(a);
                            }
                            if (closed) {
                                args.push_back(cur);
                                std::vector<CTokens> expanded_args;
                                for (auto &a: args) {
                                    CTokens e;
                                    std::set<std::string> h;
                                    expand(a, e, h, depth + 1);
                                    expanded_args.push_back(std::move(e));
                                }
                                CTokens body;
                                for (size_t bi = 0; bi < m.body.size(); ++bi) {
                                    const CToken &bt = m.body[bi];
                                    // `#param` → 字符串化（用未展开的实参拼写）
                                    if (bt.kind == CTokKind::Hash && bi + 1 < m.body.size() &&
                                        m.body[bi + 1].kind == CTokKind::Identifier) {
                                        int hp = -1;
                                        for (size_t k = 0; k < m.params.size(); ++k)
                                            if (m.params[k] == m.body[bi + 1].text) {
                                                hp = static_cast<int>(k);
                                                break;
                                            }
                                        if (hp >= 0 && hp < static_cast<int>(args.size())) {
                                            CToken sl;
                                            sl.kind = CTokKind::StringLiteral;
                                            sl.text = stringize(args[static_cast<size_t>(hp)]);
                                            sl.line = bt.line;
                                            sl.column = bt.column;
                                            sl.offset = bt.offset;
                                            body.push_back(sl);
                                            ++bi;
                                            continue;
                                        }
                                    }
                                    if (bt.kind == CTokKind::Identifier) {
                                        int idx = -1;
                                        for (size_t k = 0; k < m.params.size(); ++k)
                                            if (m.params[k] == bt.text) {
                                                idx = static_cast<int>(k);
                                                break;
                                            }
                                        if (idx >= 0) {
                                            if (idx < static_cast<int>(expanded_args.size()))
                                                body.insert(body.end(), expanded_args[static_cast<size_t>(idx)].begin(),
                                                            expanded_args[static_cast<size_t>(idx)].end());
                                            continue;
                                        }
                                        if (m.variadic && bt.text == "__VA_ARGS__") {
                                            for (size_t k = m.params.size(); k < expanded_args.size(); ++k) {
                                                if (k > m.params.size()) {
                                                    CToken comma;
                                                    comma.kind = CTokKind::Comma;
                                                    comma.text = ",";
                                                    body.push_back(comma);
                                                }
                                                body.insert(body.end(), expanded_args[k].begin(),
                                                            expanded_args[k].end());
                                            }
                                            continue;
                                        }
                                    }
                                    body.push_back(bt);
                                }
                                paste(body);
                                for (auto &b: body) {
                                    b.line = t.line;
                                    b.column = t.column;
                                    b.offset = t.offset;
                                }
                                std::set<std::string> h2 = hide;
                                h2.insert(t.text);
                                expand(body, out, h2, depth + 1);
                                i = j;
                                continue;
                            }
                        }
                    }
                }
                out.push_back(t);
            }
        }

        void do_define(const std::string &arg) {
            size_t p = 0;
            while (p < arg.size() && (arg[p] == ' ' || arg[p] == '\t')) ++p;
            std::string name;
            while (p < arg.size() && is_id_char(arg[p])) name += arg[p++];
            if (name.empty()) return;
            Macro m;
            if (p < arg.size() && arg[p] == '(') {
                m.fnlike = true;
                ++p;
                std::string cur;
                for (; p < arg.size() && arg[p] != ')'; ++p) {
                    const char c = arg[p];
                    if (c == ',') {
                        if (!cur.empty()) m.params.push_back(cur);
                        cur.clear();
                        continue;
                    }
                    if (c == '.' && p + 2 < arg.size() && arg[p + 1] == '.' && arg[p + 2] == '.') {
                        m.variadic = true;
                        p += 2;
                        continue;
                    }
                    if (c != ' ' && c != '\t') cur += c;
                }
                // 最后一个形参没有逗号收尾，必须在循环外补一次。
                // 漏掉这一句的后果是「最后一个形参永远不参与替换」：
                // `#define E(t) t x;` 里 t 会被当成普通标识符，
                // musl 的 math.h `__ISREL_DEF(rel, op, type)` 正是这么踩的。
                if (!cur.empty()) m.params.push_back(cur);
                if (p < arg.size() && arg[p] == ')') ++p;
            }
            const std::string body = p < arg.size() ? arg.substr(p) : std::string();
            CLexer bl(body);
            bl.merge_preproc(false);
            CLexResult br = bl.tokenize();
            for (auto &t: br.tokens)
                if (t.kind != CTokKind::EndOfFile && t.kind != CTokKind::Comment)
                    m.body.push_back(t);
            macros_[name] = std::move(m);
            res_.macros.insert(name);
        }

        void do_undef(const std::string &arg) {
            const std::string a = trim_left(arg);
            std::string name;
            for (const char c: a) {
                if (!is_id_char(c)) break;
                name += c;
            }
            macros_.erase(name);
            res_.macros.erase(name);
        }

        /// `defined X` / `defined(X)` → 1/0，然后做宏展开，交给 CondEval 求值。
        long long eval_condition(const std::string &arg, const CToken &at) {
            CLexer lx(arg);
            lx.merge_preproc(false);
            CLexResult lr = lx.tokenize();
            CTokens raw;
            for (auto &t: lr.tokens)
                if (t.kind != CTokKind::EndOfFile && t.kind != CTokKind::Comment) raw.push_back(t);
            for (auto &e: lr.errors)
                res_.errors.push_back(std::to_string(at.line) + ":" + std::to_string(at.column) +
                                      ": #if: " + e.message);

            CTokens pre;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i].kind == CTokKind::Identifier && raw[i].text == "defined") {
                    bool paren = false;
                    size_t j = i + 1;
                    if (j < raw.size() && raw[j].kind == CTokKind::LParen) {
                        paren = true;
                        ++j;
                    }
                    std::string name;
                    if (j < raw.size() && raw[j].kind == CTokKind::Identifier) name = raw[j].text;
                    if (name.empty()) {
                        res_.errors.push_back(std::to_string(at.line) + ": #if: 'defined' needs a macro name");
                        pre.push_back(raw[i]);
                        continue;
                    }
                    CToken num;
                    num.kind = CTokKind::IntLiteral;
                    num.text = macros_.contains(name) ? "1" : "0";
                    num.line = at.line;
                    num.column = at.column;
                    pre.push_back(num);
                    i = j;
                    if (paren && i + 1 < raw.size() && raw[i + 1].kind == CTokKind::RParen) ++i;
                    continue;
                }
                pre.push_back(raw[i]);
            }

            CTokens expanded;
            std::set<std::string> hide;
            expand(pre, expanded, hide, 0);
            CondEval ev(expanded);
            const long long v = ev.ternary();
            if (ev.failed)
                res_.errors.push_back(std::to_string(at.line) + ": #if: cannot evaluate condition '" +
                                      arg + "'");
            return v;
        }

        bool do_include(const std::string &arg, const std::string &path, int depth) {
            const std::string a = trim_left(arg);
            if (a.empty()) return false;
            const char open = a[0];
            if (open != '"' && open != '<') return false;
            const char close = open == '"' ? '"' : '>';
            const size_t e = a.find(close, 1);
            if (e == std::string::npos) return false;
            const std::string target = a.substr(1, e - 1);

            std::vector<fs::path> cands;
            if (open == '"') cands.emplace_back(fs::path(dirname_of(path)) / target);
            for (const auto &d: opt_.include_dirs) cands.emplace_back(fs::path(d) / target);

            for (const auto &c: cands) {
                std::error_code ec;
                if (!fs::exists(c, ec)) continue;
                const std::string key = fs::weakly_canonical(c, ec).string();
                if (ec) continue;
                // 只跳过显式 `#pragma once` 的文件。不能「同一文件只展开一次」：
                // musl 的 bits/alltypes.h 每次被包含时靠 `__NEED_clock_t` 这类宏
                // 决定要 typedef 什么，跳过第二次包含会静默丢掉 clock_t / pid_t。
                if (once_.contains(key)) return true;
                // 循环包含：A→B→A。没有 guard 的头在真实 libc 里也偶有出现，
                // 真按标准展开会一直撞到深度上限为止（实测某头被展开 32 次、
                // 产出上千条重复诊断）。直接跳过正在展开的文件。
                if (std::find(stack_.begin(), stack_.end(), key) != stack_.end()) return true;
                if (res_.include_count > 4096) return false;   // 病态递归的兜底
                if (depth + 1 > opt_.max_include_depth) {
                    res_.errors.push_back(key + ": #include nesting deeper than " +
                                          std::to_string(opt_.max_include_depth));
                    return false;
                }
                const std::string src = read_file(c);
                ++res_.include_count;
                stack_.push_back(key);
                file(src, key, depth + 1);
                stack_.pop_back();
                return true;
            }
            res_.unresolved_includes.push_back(target);
            return false;
        }

        void handle_directive(const CToken &t, const std::string &path, int depth) {
            res_.directives.push_back(t);
            const std::string clean = drop_continuations(t.text);
            size_t p = 0;
            while (p < clean.size() && (clean[p] == ' ' || clean[p] == '\t')) ++p;
            if (p < clean.size() && clean[p] == '#') ++p;
            while (p < clean.size() && (clean[p] == ' ' || clean[p] == '\t')) ++p;
            const size_t name_begin = p;
            while (p < clean.size() && (clean[p] == '_' || std::isalpha(static_cast<unsigned char>(clean[p]))))
                ++p;
            const std::string dir = clean.substr(name_begin, p - name_begin);
            const std::string arg = clean.substr(p);
            const std::string where = path + ":" + std::to_string(t.line) + ":";

            if (dir == "if" || dir == "ifdef" || dir == "ifndef") {
                flush();
                const bool parent = active();
                bool v = false;
                if (parent) {
                    if (dir == "if") {
                        v = eval_condition(arg, t) != 0;
                    } else {
                        const std::string a = trim_left(arg);
                        std::string name;
                        for (const char c: a) {
                            if (!is_id_char(c)) break;
                            name += c;
                        }
                        const bool defined = macros_.contains(name);
                        v = (dir == "ifdef") ? defined : !defined;
                    }
                }
                Cond c;
                c.parent_active = parent;
                c.taken = parent ? v : true;
                c.active = parent && v;
                conds_.push_back(c);
                return;
            }
            if (dir == "elif") {
                if (conds_.empty()) {
                    res_.errors.push_back(where + " #elif without #if");
                    return;
                }
                Cond &c = conds_.back();
                if (!c.parent_active || c.taken) {
                    c.active = false;
                } else {
                    const bool v = eval_condition(arg, t) != 0;
                    c.active = v;
                    c.taken = v;
                }
                return;
            }
            if (dir == "else") {
                if (conds_.empty()) {
                    res_.errors.push_back(where + " #else without #if");
                    return;
                }
                Cond &c = conds_.back();
                c.active = c.parent_active && !c.taken;
                c.taken = true;
                return;
            }
            if (dir == "endif") {
                if (conds_.empty()) {
                    res_.errors.push_back(where + " #endif without #if");
                    return;
                }
                conds_.pop_back();
                return;
            }
            if (!active()) return;

            if (dir == "define") {
                flush();
                do_define(arg);
                return;
            }
            if (dir == "undef") {
                flush();
                do_undef(arg);
                return;
            }
            if (dir == "include") {
                flush();
                // 原样的 #include 行也要留在输出里：LSP 的 includelist、格式化器、
                // 以及 Sinter 侧的 `embed C { #include ... }` 都靠它。
                res_.tokens.push_back(t);
                do_include(arg, path, depth);
                return;
            }
            if (dir == "error") {
                res_.errors.push_back(where + " " + trim_left(arg));
                return;
            }
            if (dir == "pragma") {
                const std::string a = trim_left(arg);
                if (a.rfind("once", 0) == 0) once_.insert(path);
                return;
            }
            // #line / 其它 pragma：忽略
        }

        void file(const std::string &source, const std::string &path, int depth) {
            if (depth > opt_.max_include_depth) return;
            // 给本文件的每个 token 打上文件号（主文件是 0）。展开宏时 body 的
            // token 会带着「定义处」的文件号，这与 gcc 的 in-expansion 归属一致。
            uint32_t fid = 0;
            {
                auto it = std::find(res_.files.begin(), res_.files.end(), path);
                if (it == res_.files.end()) {
                    fid = static_cast<uint32_t>(res_.files.size());
                    res_.files.push_back(path);
                } else {
                    fid = static_cast<uint32_t>(it - res_.files.begin());
                }
            }
            CTokens toks;
            const auto cached = lexed_.find(path);
            if (cached != lexed_.end()) {
                toks = cached->second;      // 拷贝：下面的指令处理会消费/跳过 token
            } else {
                CLexer lx(source);
                lx.keep_comments(false);
                lx.merge_preproc(true);
                CLexResult lr = lx.tokenize();
                for (const auto &e: lr.errors)
                    res_.errors.push_back(path + ":" + std::to_string(e.line) + ":" +
                                          std::to_string(e.column) + ": " + e.message);
                for (auto &t: lr.tokens) t.file_id = fid;
                toks = lr.tokens;
                lexed_[path] = toks;
            }

            for (auto &t: toks) {
                if (t.kind == CTokKind::PreprocLine) {
                    handle_directive(t, path, depth);
                    continue;
                }
                if (t.kind == CTokKind::EndOfFile) continue;
                if (!active()) continue;
                pending_.push_back(t);
            }
            flush();
        }
    };

    CPreprocResult preprocess_c(const std::string &source, const std::string &path,
                                const CPreprocOptions &options) {
        Preprocessor pp(options);
        return pp.run(source, path);
    }
} // namespace sinter::cfront

// sinter: sinter_format.cpp —— Sinter 源码格式化实现
//
// 目标：**只重排空白与换行，不改动任何非空白字符**。这条约束让格式化可以
// 在任何（哪怕是语法不完整的）代码上安全运行 —— 编辑器里的文件经常正在敲。
//
// 规则（对应 .editorconfig 风格的默认值）：
//
//   1. 缩进 4 空格（`options_.tab_size`），不用制表符
//   2. `{` 留在行尾；跨行的 `}` 单独成行并回到上一层缩进，
//      原本写成一行 `{ ... }` 的紧凑块保持一行
//   3. 括号内不断行：`(a, b)` 里的逗号后面补一个空格
//   4. 二元运算符两侧各一个空格：`= + - * / < > == != <= >= && || => ->`
//   5. `,` 后一个空格、`;` 后一个空格（for 头里）
//   6. 连续空行压成最多一行
//   7. 文件末尾保留一个换行
//   8. 字符串 / 字符字面量 / 注释内部**一个字节都不动**（`///`、`/** */` 同样）
//   9. `embed C { ... }` 的正文原样保留（那是 C 代码，不该按 Sinter 缩进）
//
// 为什么自己扫一遍而不是复用词法器：这里需要在扫描过程中同时决定「要不要
// 输出空格」，而词法器只给 token。用一个极简的状态机（普通 / 字符串 / 注释 /
// embedC）更直接，也避免与 LSP 的容错需求耦合。
#include "format/sinter_format.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace sinter::format {
    namespace {
        /// 行首缩进需要的空格数
        std::string indent_of(const int32_t level, const int32_t size) {
            if (level <= 0) return {};
            return std::string(static_cast<size_t>(level) * static_cast<size_t>(size), ' ');
        }

        bool is_word_char(const char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
        }

        /// 这些运算符两侧要留空格。多字符优先（`==` 先于 `=`）。
        const char *kSpacedOps[] = {
            "==", "!=", "<=", ">=", "&&", "||", "=>", "->", "+=", "-=", "*=", "/=",
            "%=", "&=", "|=", "^=", "<<", ">>", "??", "|>", "..<", "..",
        };

        /// 这些多字符运算符内部不允许空格：`a?.b`、`Foo::bar`。
        const char *kTightOps[] = {
            "?.", "::",
        };

        /// 输出行缓冲：把「一行里已经攒了什么」和缩进交给它，
        /// 这样插入空格时不至于写下重复的空白。
        struct LineWriter {
            std::string out;
            int32_t indent_level = 0;
            int32_t indent_size = 4;
            bool line_started = false;   // 本行是否已有非空白字符
            bool want_space = false;     // 下一个字符前要补一个空格
            int32_t blank_run = 0;       // 已经攒了几个空行
            /// 每个未闭合 `{` 对应的块里是否出现过换行。
            /// 没换行的 `{ ... }`（用户刻意写成一行）保持一行，不强行拆开。
            std::vector<char> blocks;

            void newline() {
                for (char &block: blocks) block = 1;
                if (line_started) {
                    out += '\n';
                    line_started = false;
                } else if (blank_run < 1) {
                    out += '\n';
                    ++blank_run;
                }
                want_space = false;
            }

            /// 请求一个单词边界（重复请求只会产生一个空格）
            void space() {
                if (line_started) want_space = true;
            }

            /// 取消之前的空格请求（紧贴写法用：`.` / `(` / `,`）
            void no_space() { want_space = false; }

            void emit_indent() {
                if (!line_started) {
                    out += indent_of(indent_level, indent_size);
                    line_started = true;
                    blank_run = 0;
                    want_space = false;   // 行首不要空格
                } else if (want_space) {
                    out += ' ';
                    want_space = false;
                }
            }

            void put(const char c) {
                emit_indent();
                out += c;
            }

            void put_raw(const std::string &s) {
                if (s.empty()) return;
                emit_indent();
                out += s;
            }

            /// 结尾收拾：确保恰好一个换行
            std::string finish() const {
                std::string r = out;
                while (!r.empty() && (r.back() == '\n' || r.back() == ' ' || r.back() == '\t'))
                    r.pop_back();
                if (!r.empty()) r += '\n';
                return r;
            }
        };
        char last_nonspace_char(const std::string &s) {
            for (auto it = s.rbegin(); it != s.rend(); ++it) {
                if (*it != ' ' && *it != '\t') return *it;
            }
            return '\0';
        }

        std::string trailing_word(const std::string &s) {
            size_t end = s.size();
            while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
            size_t start = end;
            while (start > 0 && is_word_char(s[start - 1])) --start;
            return s.substr(start, end - start);
        }

        /// `-1` / `!x` 左侧是不是表达式起点（用于区分一元与二元运算符）。
        bool expression_start_char(const char c) {
            switch (c) {
                case '\0':
                case '\n':
                case '(':
                case '[':
                case '{':
                case ',':
                case ';':
                case ':':
                case '=':
                case '+':
                case '-':
                case '*':
                case '/':
                case '%':
                case '<':
                case '>':
                case '&':
                case '|':
                case '^':
                case '?':
                case '!':
                case '~': return true;
                default: return false;
            }
        }

        bool expression_start_word(const std::string &word) {
            static const char *kWords[] = {
                "ret", "return", "if", "else", "while", "for", "when", "in", "is",
                "case", "default", "val", "var", "const", "del", "copy", "move",
                "not", "must", "and", "or",
            };
            for (const char *w: kWords) {
                if (word == w) return true;
            }
            return false;
        }
    } // namespace

    std::string format_sinter(const std::string &src, const FormatOptions &opt) {
        LineWriter w;
        w.indent_size = opt.tab_size > 0 ? opt.tab_size : 4;

        const size_t n = src.size();
        size_t i = 0;
        bool prev_is_close = false;    // 上一个落笔的是 `)` / `]` / `}`
        // 刚才丢弃的原空白里是否含空格（用来区分「词内相邻」与「两个词之间」）。
        // 丢弃发生在上面的空白分支，这里只记一个标记，落笔后立刻复位。
        bool word_gap = false;

        /// 每个分支落笔后都要复位：只有「刚丢弃过空白」这一个字符才有意义

        auto last_char = [&]() -> char { return w.out.empty() ? '\0' : w.out.back(); };

        /// 落笔前决定「词与词之间」要不要空格
        auto boundary_between_words = [&]() {
            const char pc = last_char();
            if (pc == '\0' || pc == '\n' || pc == ' ' || pc == '\t') return;
            // 上一个字符是词字符 / 右括号 → 需要分隔
            if (is_word_char(pc) || pc == ')' || pc == ']' || pc == '"' || pc == '\'')
                w.space();
        };

        while (i < n) {
            const char c = src[i];

            // ---------------- 换行 ----------------
            if (c == '\n') {
                w.newline();
                prev_is_close = false;
                word_gap = false;
                ++i;
                continue;
            }
            if (c == '\r') { ++i; continue; }

            // ---------------- 行内空白：丢弃（由规则重新生成） ----------------
            if (c == ' ' || c == '\t') { word_gap = true; ++i; continue; }

            // ---------------- 行注释 ----------------
            if (c == '/' && i + 1 < n && src[i + 1] == '/') {
                size_t j = i;
                while (j < n && src[j] != '\n') ++j;
                w.space();
                w.put_raw(src.substr(i, j - i));
                i = j;
                prev_is_close = false;
                continue;
            }

            // ---------------- 块注释 ----------------
            if (c == '/' && i + 1 < n && src[i + 1] == '*') {
                size_t j = i + 2;
                while (j + 1 < n && !(src[j] == '*' && src[j + 1] == '/')) ++j;
                j = (j + 1 < n) ? j + 2 : n;
                const std::string block = src.substr(i, j - i);
                w.space();
                if (block.find('\n') == std::string::npos) {
                    w.put_raw(block);
                    prev_is_close = false;
                } else {
                    // 跨行块注释：整段原样，之后强制换行
                    w.put_raw(block);
                    w.newline();
                    prev_is_close = false;
                }
                i = j;
                continue;
            }

            // ---------------- 字符串 / 字符字面量：原样 ----------------
            if (c == '"' || c == '\'') {
                const char quote = c;
                size_t j = i + 1;
                while (j < n) {
                    if (src[j] == '\\' && j + 1 < n) { j += 2; continue; }
                    if (src[j] == quote) { ++j; break; }
                    if (src[j] == '\n') break;
                    ++j;
                }
                boundary_between_words();
                w.put_raw(src.substr(i, j - i));
                i = j;
                prev_is_close = true;
                continue;
            }

            // ---------------- `{` ----------------
            if (c == '{') {
                // `embed C {` 的正文原样保留
                bool is_embed_c = false;
                {
                    const std::string &o = w.out;
                    size_t k = o.size();
                    while (k > 0 && o[k - 1] != '\n') --k;
                    const std::string head = o.substr(k);
                    if (head.find("embed") != std::string::npos &&
                        (head.find(" C") != std::string::npos || head.find("\tC") != std::string::npos))
                        is_embed_c = true;
                }
                boundary_between_words();
                w.space();
                w.put('{');
                ++w.indent_level;
                w.blocks.push_back(0);
                ++i;
                prev_is_close = false;

                if (is_embed_c) {
                    // 从 `{` 之后到配对的 `}` 之前一个字节都不改
                    const size_t body_start = i;
                    int depth = 1;
                    size_t j = i;
                    bool in_str = false, in_chr = false, in_lc = false, in_bc = false;
                    while (j < n && depth > 0) {
                        const char d = src[j];
                        if (in_lc) { if (d == '\n') in_lc = false; ++j; continue; }
                        if (in_bc) {
                            if (d == '*' && j + 1 < n && src[j + 1] == '/') { in_bc = false; j += 2; continue; }
                            ++j; continue;
                        }
                        if (in_str) { if (d == '\\') { j += 2; continue; } if (d == '"') in_str = false; ++j; continue; }
                        if (in_chr) { if (d == '\\') { j += 2; continue; } if (d == '\'') in_chr = false; ++j; continue; }
                        if (d == '/' && j + 1 < n && src[j + 1] == '/') { in_lc = true; j += 2; continue; }
                        if (d == '/' && j + 1 < n && src[j + 1] == '*') { in_bc = true; j += 2; continue; }
                        if (d == '"') { in_str = true; ++j; continue; }
                        if (d == '\'') { in_chr = true; ++j; continue; }
                        if (d == '{') { ++depth; ++j; continue; }
                        if (d == '}') {
                            --depth;
                            if (depth == 0) break;
                        }
                        ++j;
                    }
                    std::string body = src.substr(body_start, j - body_start);
                    // 首尾各去掉一个换行，正文内部原样
                    if (!body.empty() && body.front() == '\n') body.erase(0, 1);
                    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                    w.newline();
                    if (!body.empty()) {
                        w.out += body;
                        w.out += '\n';
                    }
                    w.line_started = false;
                    w.want_space = false;
                    if (w.indent_level > 0) --w.indent_level;
                    i = j;   // 指向 `}`
                    // 交给下面的 `}` 分支收尾（它会再减一次缩进，所以先加回来）
                    ++w.indent_level;
                    continue;
                }
                continue;
            }

            // ---------------- `}` ----------------
            if (c == '}') {
                const bool compact = !w.blocks.empty() && !w.blocks.back();
                if (!w.blocks.empty()) w.blocks.pop_back();
                if (w.indent_level > 0) --w.indent_level;
                if (!compact) {
                    if (w.line_started) w.newline();
                    w.no_space();
                } else if (last_nonspace_char(w.out) == '{') {
                    w.no_space();
                } else {
                    w.space();
                }
                w.put('}');
                ++i;
                prev_is_close = true;
                // `} else` / `} while` 留在同一行
                size_t k = i;
                while (k < n && (src[k] == ' ' || src[k] == '\t')) ++k;
                if (k < n && (src.compare(k, 4, "else") == 0 || src.compare(k, 5, "while") == 0))
                    w.space();
                else if (k < n && src[k] != '\n' && src[k] != ';' && src[k] != ',' &&
                         src[k] != ')' && src[k] != '}' && src[k] != '.')
                    w.space();
                continue;
            }

            // ---------------- `;` ----------------
            if (c == ';') {
                w.no_space();
                w.put(';');
                ++i;
                prev_is_close = false;
                w.space();
                continue;
            }

            // ---------------- `,` ----------------
            if (c == ',') {
                w.no_space();
                w.put(',');
                ++i;
                prev_is_close = false;
                w.space();
                continue;
            }

            // ---------------- `(` ----------------
            if (c == '(') {
                // 关键字后留空格（`if (` / `for (` / `while (`），函数名后紧贴
                const std::string &o = w.out;
                size_t k = o.size();
                while (k > 0 && o[k - 1] != '\n' && o[k - 1] != ' ') --k;
                const std::string word = o.substr(k);
                const bool kw = (word == "if" || word == "for" || word == "while" ||
                                 word == "when" || word == "switch" || word == "catch");
                if (kw) { w.space(); } else { w.no_space(); }
                w.put('(');
                word_gap = false;
                ++i;
                prev_is_close = false;
                continue;
            }

            // ---------------- `)` / `]` ----------------
            if (c == ')' || c == ']') {
                w.no_space();
                w.put(c);
                word_gap = false;
                ++i;
                prev_is_close = true;
                continue;
            }

            // ---------------- 数字字面量：连指数一起吃，避免 `1e-9` 变成 `1e - 9` ----------------
            if (c >= '0' && c <= '9' && !is_word_char(last_nonspace_char(w.out))) {
                size_t j = i;
                if (c == '0' && j + 1 < n &&
                    (src[j + 1] == 'x' || src[j + 1] == 'X' || src[j + 1] == 'b' ||
                     src[j + 1] == 'B' || src[j + 1] == 'o' || src[j + 1] == 'O')) {
                    j += 2;
                    while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) ||
                                     src[j] == '_' || src[j] == '\'')) ++j;
                } else {
                    while (j < n && ((src[j] >= '0' && src[j] <= '9') ||
                                     src[j] == '_' || src[j] == '\'')) ++j;
                    if (j < n && src[j] == '.') {
                        ++j;
                        while (j < n && ((src[j] >= '0' && src[j] <= '9') ||
                                         src[j] == '_' || src[j] == '\'')) ++j;
                    }
                    if (j < n && (src[j] == 'e' || src[j] == 'E')) {
                        ++j;
                        if (j < n && (src[j] == '+' || src[j] == '-')) ++j;
                        while (j < n && (src[j] >= '0' && src[j] <= '9')) ++j;
                    }
                }
                while (j < n && std::strchr("fFuUlL", src[j]) != nullptr) ++j;
                boundary_between_words();
                w.put_raw(src.substr(i, j - i));
                i = j;
                prev_is_close = true;
                word_gap = false;
                continue;
            }

            // ---------------- 紧贴的多字符运算符（`?.` / `::`） ----------------
            {
                bool handled = false;
                for (const char *op: kTightOps) {
                    const size_t len = std::char_traits<char>::length(op);
                    if (src.compare(i, len, op) != 0) continue;
                    w.no_space();
                    w.put_raw(op);
                    w.no_space();
                    word_gap = false;
                    i += len;
                    prev_is_close = false;
                    handled = true;
                    break;
                }
                if (handled) continue;
            }

            // ---------------- 多字符运算符 ----------------
            {
                bool handled = false;
                for (const char *op: kSpacedOps) {
                    const size_t len = std::char_traits<char>::length(op);
                    if (src.compare(i, len, op) != 0) continue;
                    w.space();
                    w.put_raw(op);
                    w.space();
                    word_gap = false;
                    i += len;
                    prev_is_close = false;
                    handled = true;
                    break;
                }
                if (handled) continue;
            }

            // ---------------- 单字符运算符 ----------------
            if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                c == '<' || c == '>' || c == '&' || c == '|' || c == '^' || c == '!' ||
                c == '?' || c == '~') {
                if (c == '-' || c == '+' || c == '!' || c == '~') {
                    const char pc = last_nonspace_char(w.out);
                    const bool unary = c == '!' || c == '~' ||
                                       expression_start_char(pc) ||
                                       expression_start_word(trailing_word(w.out));
                    if (unary) {
                        // 一元：`ret -1` / `(-v)` / `x = -1`。右侧紧贴，
                        // 否则会输出成 `ret - 1`。
                        if (pc != '(' && pc != '[' && pc != '.') w.space();
                        w.put(c);
                        w.no_space();
                        word_gap = false;
                    } else {
                        w.space();
                        w.put(c);
                        w.space();
                    }
                } else {
                    w.space();
                    w.put(c);
                    w.space();
                }
                word_gap = false;
                ++i;
                prev_is_close = false;
                continue;
            }

            // ---------------- `.` / `:` / `@`：`:` 后一个空格，其余两侧紧贴 ----------------
            if (c == '.' || c == ':' || c == '@') {
                if (c == ':') {
                    // `v: i64` 紧贴；`class X : Any` 保留用户原有的空格。
                    // 这两处都是 `:`，语义上下文不同，格式化器不替用户猜。
                    if (word_gap) w.space(); else w.no_space();
                } else {
                    w.no_space();
                }
                w.put(c);
                if (c == ':') {
                    w.space();   // `a: i64` / `fun f(): void`
                } else {
                    w.no_space();
                }
                word_gap = false;
                ++i;
                prev_is_close = false;
                continue;
            }

            // ---------------- 普通字符 ----------------
            {
                if (is_word_char(c)) {
                    // 需要空格的三种情况：
                    //  1) 原来这里有空白（word_gap）——`module demo`
                    //  2) 上一个落笔的是 `)` / `]` / `}` / 字面量——`f() x`
                    //  3) 上一笔是运算符并且请求了空格——`x > 2` 里的 `2`
                    // 其余（词内相邻、`(` 之后、`.` 之后）一律紧贴。
                    if (word_gap || prev_is_close || w.want_space) w.space();
                    else w.no_space();
                    w.put(c);
                    prev_is_close = false;
                    word_gap = false;
                    ++i;
                    continue;
                }
                // 其他符号（如 `#` `$` 等意外字符）：原样、紧贴
                w.put(c);
                prev_is_close = false;
                ++i;
                continue;
            }
        }

        return w.finish();
    }
} // namespace sinter::format

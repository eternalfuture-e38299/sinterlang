#include "c2_lexer.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace sinter::cfront2 {
    namespace {
        const std::unordered_set<std::string> &keywords() {
            static const std::unordered_set<std::string> set = {
                "auto", "break", "case", "char", "const", "continue", "default", "do",
                "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
                "int", "long", "register", "restrict", "return", "short", "signed", "sizeof",
                "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile",
                "while", "_Bool", "_Complex", "_Imaginary",
            };
            return set;
        }

        bool ident_start(const char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        }
        bool ident_char(const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }
        bool digit(const char c) { return c >= '0' && c <= '9'; }

        /// 最长匹配优先。数组顺序从长到短，保证 `...` 不被拆成 `.` `.` `.`。
        const char *const kPunct[] = {
            "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
            "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
            "{", "}", "(", ")", "[", "]", ";", ",", ".", "&", "*", "+", "-", "~", "!",
            "/", "%", "<", ">", "^", "|", "?", ":", "=", "#",
        };
    } // namespace

    bool is_keyword(const std::string &s) { return keywords().contains(s); }

    std::vector<Token> lex(const std::string &src) {
        std::vector<Token> out;
        const size_t n = src.size();
        size_t i = 0;
        int line = 1;
        int col = 1;
        auto advance = [&]() {
            if (src[i] == '\n') { ++line; col = 1; } else { ++col; }
            ++i;
        };
        auto add = [&](TokKind k, size_t start, int l, int c) {
            out.push_back({k, src.substr(start, i - start), start, l, c});
        };

        while (i < n) {
            const char ch = src[i];

            // 换行 / 空白
            if (ch == '\n' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
                advance();
                continue;
            }

            // 预处理行：整行作为一个 token（跨行反斜杠续行）
            if (ch == '#') {
                const size_t start = i;
                const int l = line, c = col;
                while (i < n) {
                    if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') {
                        advance();
                        advance();
                        continue;
                    }
                    if (src[i] == '\n') break;
                    advance();
                }
                add(TokKind::Preproc, start, l, c);
                continue;
            }

            // 注释
            if (ch == '/' && i + 1 < n && src[i + 1] == '/') {
                while (i < n && src[i] != '\n') advance();
                continue;
            }
            if (ch == '/' && i + 1 < n && src[i + 1] == '*') {
                advance();
                advance();
                while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) advance();
                if (i + 1 < n) { advance(); advance(); }
                continue;
            }

            const size_t start = i;
            const int l = line, c = col;

            // 标识符 / 关键字
            if (ident_start(ch)) {
                while (i < n && ident_char(src[i])) advance();
                const std::string text = src.substr(start, i - start);
                add(is_keyword(text) ? TokKind::Keyword : TokKind::Identifier, start, l, c);
                continue;
            }

            // 数字字面量（十六进制 / 浮点 / 指数 / 后缀）
            if (digit(ch) || (ch == '.' && i + 1 < n && digit(src[i + 1]))) {
                bool is_float = false;
                if (ch == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                    advance(); advance();
                    while (i < n && std::isxdigit(static_cast<unsigned char>(src[i]))) advance();
                } else {
                    while (i < n && (digit(src[i]) || src[i] == '.')) {
                        if (src[i] == '.') is_float = true;
                        advance();
                    }
                    if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                        is_float = true;
                        advance();
                        if (i < n && (src[i] == '+' || src[i] == '-')) advance();
                        while (i < n && digit(src[i])) advance();
                    }
                }
                // 后缀 f/F/l/L/u/U
                while (i < n && std::strchr("fFlLuU", src[i]) != nullptr) advance();
                add(is_float ? TokKind::FloatLiteral : TokKind::IntLiteral, start, l, c);
                continue;
            }

            // 字符串 / 字符（含转义）
            if (ch == '"' || ch == '\'') {
                const char quote = ch;
                advance();
                while (i < n) {
                    if (src[i] == '\\' && i + 1 < n) { advance(); advance(); continue; }
                    if (src[i] == quote) { advance(); break; }
                    if (src[i] == '\n') break;
                    advance();
                }
                add(quote == '"' ? TokKind::StringLiteral : TokKind::CharLiteral, start, l, c);
                continue;
            }

            // 标点（最长匹配）
            bool matched = false;
            for (const char *p: kPunct) {
                const size_t len = std::strlen(p);
                if (src.compare(i, len, p) == 0) {
                    for (size_t k = 0; k < len; ++k) advance();
                    add(TokKind::Punct, start, l, c);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;

            // 未识别字符：当作单字符标点，保证 parser 能继续
            advance();
            add(TokKind::Punct, start, l, c);
        }
        out.push_back({TokKind::Eof, {}, n, line, col});
        return out;
    }
} // namespace sinter::cfront2

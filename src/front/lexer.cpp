/*******************************************************************************
 * File: lexer
 * Project: sinter
 * Created: 2026/11/17
 * Author: eternalfuture-e38299
 * Github: https://github.com/eternalfuture-e38299
 *
 * MIT License
 *
 * Copyright (c) 2026 eternalfuture-e38299
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *******************************************************************************/

#include "front/lexer.hpp"
#include "support/keywords.hpp"
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace sinter {
    namespace {
        /// 从 keywords.hpp 的 constexpr 表铺平成查表用的哈希表。
        /// 关键字本就不多（<64），reserve 之后无 rehash，查表仍是 O(1)。
        std::unordered_map<std::string, TokenType> build_keyword_table() {
            std::unordered_map<std::string, TokenType> table;
            table.reserve(kKeywords.size() * 2);
            for (const auto &kw: kKeywords) table.emplace(std::string(kw.text), kw.type);
            return table;
        }
    } // namespace

    const std::unordered_map<std::string, TokenType> Lexer::keywords = build_keyword_table();

    Lexer::Lexer(std::string s) : source(std::move(s)) {}

    char Lexer::peek() const { return pos < source.size() ? source[pos] : '\0'; }
    char Lexer::peek_next() const { return pos + 1 < source.size() ? source[pos + 1] : '\0'; }
    char Lexer::peek_at(size_t n) const { return pos + n < source.size() ? source[pos + n] : '\0'; }

    char Lexer::advance() {
        const char c = source[pos++];
        if (c == '\n') { line++; column = 1; }
        else column++;
        return c;
    }

    bool Lexer::match(const char c) {
        if (peek() != c) return false;
        advance();
        return true;
    }

    void Lexer::skip_whitespace() {
        while (pos < source.size()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') advance();
            else if (c == '/' && (peek_next() == '/' || peek_next() == '*')) skip_comment();
            else break;
        }
    }

    void Lexer::skip_comment() {
        if (peek_next() == '/') {
            while (pos < source.size() && peek() != '\n') advance();
        } else {
            advance(); advance();
            while (pos < source.size()) {
                if (peek() == '*' && peek_next() == '/') { advance(); advance(); return; }
                advance();
            }
            error("unterminated comment");
        }
    }

    void Lexer::add_token(TokenType t, const std::string &l) {
        // 用 token 起始列而非当前列：column 此时已随扫描前进到 token 末尾，
        // 直接用它会让诊断的 ^ 指到字符串字面量的右引号之后。
        tokens.emplace_back(t, l, line, token_start_col);
    }

    void Lexer::read_string() {
        std::string raw = "\"";
        while (pos < source.size()) {
            const char c = advance();
            if (c == '"') {
                raw += '"';
                add_token(TokenType::StringLiteral, raw);
                return;
            }
            if (c == '$' && peek() == '{') {
                // 回退到字符串起点，改用模板解析
                tokens.pop_back();
                error("internal error: string template needs re-parsing");
            }
            if (c == '\\') {
                if (pos >= source.size()) break;
                raw += '\\';
                raw += advance();
            } else raw += c;
        }
        error("unterminated string literal");
    }

    void Lexer::read_char() {
        std::string raw = "'";
        if (pos >= source.size()) error("unterminated character literal");
        if (const char c = advance(); c == '\\') {
            raw += '\\';
            raw += advance();
        } else raw += c;
        if (peek() != '\'') error("character literal must end with a closing quote");
        advance();
        raw += '\'';
        add_token(TokenType::CharLiteral, raw);
    }

    void Lexer::read_number() {
        std::string n;
        // 0x / 0b / 0o
        if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
            n += advance(); n += advance();
            while (pos < source.size() && (std::isxdigit(peek()) || peek() == '_')) {
                if (peek() != '_') n += peek();
                advance();
            }
            // 转 10 进制
            const auto v = std::strtoll(n.c_str(), nullptr, 16);
            n = std::to_string(v);
        } else if (peek() == '0' && (peek_next() == 'b' || peek_next() == 'B')) {
            advance(); advance();
            std::string bits;
            while (pos < source.size() && (peek() == '0' || peek() == '1' || peek() == '_')) {
                if (peek() != '_') bits += peek();
                advance();
            }
            const auto v = std::strtoll(bits.c_str(), nullptr, 2);
            n = std::to_string(v);
        } else if (peek() == '0' && (peek_next() == 'o' || peek_next() == 'O')) {
            // 八进制：`0o17` == 15。以前这里缺分支，`0o17` 会被当十进制 0 读掉，
            // 后面的 `o17` 直接丢弃 —— 静默产出 0，属于「看起来能编但结果是错的」。
            advance(); advance();
            std::string oct;
            while (pos < source.size() && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
                if (peek() != '_') oct += peek();
                advance();
            }
            const auto v = std::strtoll(oct.c_str(), nullptr, 8);
            n = std::to_string(v);
        } else {
            bool fl = false;
            while (pos < source.size()) {
                if (std::isdigit(peek()) || peek() == '_') {
                    if (peek() != '_') n += peek();
                    advance();
                    continue;
                }
                if (peek() == '.' && !fl) {
                    // 1..5 是区间，不是小数
                    if (peek_next() == '.') break;
                    // 1.foo 不是小数
                    if (!std::isdigit(peek_next())) break;
                    fl = true;
                    n += advance();
                    continue;
                }
                if ((peek() == 'e' || peek() == 'E') && (std::isdigit(peek_next()) ||
                    ((peek_next() == '+' || peek_next() == '-') && std::isdigit(peek_at(2))))) {
                    fl = true;
                    n += advance();
                    if (peek() == '+' || peek() == '-') n += advance();
                    continue;
                }
                break;
            }
            if (n.empty()) n = "0";
            if (fl) {
                add_token(TokenType::FloatLiteral, n);
                // 读后缀 f32/f64
                std::string suf;
                while (pos < source.size() && (std::isalnum(peek()))) suf += advance();
                if (!suf.empty()) {
                    Token t = tokens.back();
                    t.lexeme = n + suf;
                    tokens.back() = t;
                }
                return;
            }
        }
        add_token(TokenType::IntegerLiteral, n);
        // 读后缀 i8/i16/i32/i64/u8/.../f32/f64
        std::string suf;
        while (pos < source.size() && std::isalnum(peek())) suf += advance();
        if (!suf.empty()) {
            Token t = tokens.back();
            t.lexeme = n + suf;
            tokens.back() = t;
        }
    }

    void Lexer::read_identifier() {
        std::string id;
        while (pos < source.size() && (std::isalnum(peek()) || peek() == '_')) id += advance();
        if (const auto it = keywords.find(id); it != keywords.end()) add_token(it->second, id);
        else add_token(TokenType::Identifier, id);
    }

    void Lexer::read_embed_c() {
        add_token(TokenType::Embed, "embed");
        skip_whitespace();
        if (peek() != 'C' && peek() != 'c') return;
        advance();
        skip_whitespace();
        // `embed C header { ... }`：这段 C 同时写进模块头文件（.h），
        // 供其它编译单元引用。用哨兵 '\x02' 前缀标记，parser 负责拆。
        bool in_header = false;
        if (source.compare(pos, 6, "header") == 0 &&
            (pos + 6 >= source.size() || !(std::isalnum(source[pos + 6]) || source[pos + 6] == '_'))) {
            for (int k = 0; k < 6; ++k) advance();
            skip_whitespace();
            in_header = true;
        }
        if (peek() != '{') { error("expected '{' after 'embed C'"); }
        advance();
        add_token(TokenType::LeftBrace, "{");
        std::string code;
        int depth = 1;
        while (pos < source.size() && depth > 0) {
            char c = advance();
            if (c == '{') { depth++; code += c; }
            else if (c == '}') {
                depth--;
                if (depth > 0) code += c;
            } else code += c;
        }
        if (depth != 0) error("unterminated 'embed C' block");
        if (in_header) code.insert(code.begin(), '\x02');
        add_token(TokenType::EmbedCBody, code);
        add_token(TokenType::RightBrace, "}");
    }

    [[noreturn]] void Lexer::error(const std::string &m) const {
        std::cerr << "Lexer " << line << ":" << column << " " << m << "\n";
        throw std::runtime_error(m);
    }

    Tokens Lexer::tokenize() {
        while (pos < source.size()) {
            skip_whitespace();
            if (pos >= source.size()) break;
            // 记录 token 起点：add_token 里要用它作为列号
            token_start_col = column;
            const char c = peek();

            if (c == '\n') {
                advance();
                if (paren_depth == 0) add_token(TokenType::Newline, "\n");
                continue;
            }

            // embed C
            if (c == 'e' && source.compare(pos, 5, "embed") == 0 &&
                (pos + 5 >= source.size() || !std::isalnum(source[pos + 5]))) {
                for (int k = 0; k < 5; ++k) advance();
                read_embed_c();
                continue;
            }

            // 三引号多行字符串：原样保留换行与缩进；转义成普通字符串 token，
            // 让后面的模板解析（`${...}`）继续可用。
            if (c == '"' && source.compare(pos, 3, "\"\"\"") == 0) {
                advance();
                advance();
                advance();
                std::string raw = "\"";
                while (pos < source.size()) {
                    if (source.compare(pos, 3, "\"\"\"") == 0) {
                        advance();
                        advance();
                        advance();
                        break;
                    }
                    const char ch = advance();
                    if (ch == '\\') raw += "\\\\";
                    else if (ch == '"') raw += "\\\"";
                    else if (ch == '\n') raw += "\\n";
                    else if (ch == '\r') { /* 统一掉 Windows 换行 */ }
                    else raw += ch;
                }
                raw += '"';
                add_token(TokenType::StringLiteral, raw);
                continue;
            }

            // 字符串模板（含 "${...}"）
            if (c == '"') {
                advance();
                {
                    std::string raw = "\"";
                    int brace = 0;          // ${...} 嵌套花括号深度
                    while (pos < source.size()) {
                        const char ch = advance();
                        // ${ 进入插值上下文
                        if (ch == '$' && pos < source.size() && source[pos] == '{') {
                            raw += ch;
                            raw += advance();
                            brace++;
                            continue;
                        }
                        if (brace > 0) {
                            raw += ch;
                            if (ch == '{') brace++;
                            else if (ch == '}') brace--;
                            // 插值内的字符串字面量：整体跳过，避免其 " 结束外层字符串
                            else if (ch == '"') {
                                while (pos < source.size()) {
                                    const char c2 = advance();
                                    if (c2 == '\\' && pos < source.size()) {
                                        raw += c2;
                                        raw += advance();
                                        continue;
                                    }
                                    raw += c2;
                                    if (c2 == '"') break;
                                }
                            } else if (ch == '\\' && pos < source.size()) {
                                raw += advance();
                            }
                            continue;
                        }
                        if (ch == '"') { raw += '"'; break; }
                        if (ch == '\\' && pos < source.size()) { raw += '\\'; raw += advance(); continue; }
                        raw += ch;
                    }
                    add_token(TokenType::StringLiteral, raw);
                }
                continue;
            }

            if (c == '\'') { advance(); read_char(); continue; }
            if (std::isdigit(c)) { read_number(); continue; }

            switch (c) {
                case '(': advance(); add_token(TokenType::LeftParen, "("); paren_depth++; continue;
                case ')': advance(); add_token(TokenType::RightParen, ")"); paren_depth--; continue;
                case '{': advance(); add_token(TokenType::LeftBrace, "{"); brace_depth++; continue;
                case '}': advance(); add_token(TokenType::RightBrace, "}"); brace_depth--; continue;
                case '[': advance(); add_token(TokenType::LeftBracket, "["); continue;
                case ']': advance(); add_token(TokenType::RightBracket, "]"); continue;
                case ',': advance(); add_token(TokenType::Comma, ","); continue;
                case ';': advance(); add_token(TokenType::Semicolon, ";"); continue;
                case '@': advance(); add_token(TokenType::At, "@"); paren_depth += 0; continue;
                case '~': advance(); add_token(TokenType::Tilde, "~"); continue;
                default: break;
            }

            // 多字符运算符
            if (c == '.' && peek_next() == '.' && peek_at(2) == '.') {
                advance(); advance(); advance();
                add_token(TokenType::DotDotDot, "...");
                continue;
            }
            if (c == '.' && peek_next() == '.' && peek_at(2) == '<') {
                advance(); advance(); advance();
                add_token(TokenType::RangeOpen, "..<");
                continue;
            }
            if (c == '.' && peek_next() == '.') {
                advance(); advance();
                add_token(TokenType::DoubleDot, "..");
                continue;
            }
            if (c == '|' && peek_next() == '>') {
                advance(); advance();
                add_token(TokenType::PipeGt, "|>");
                continue;
            }
            if (c == '?' && peek_next() == ':') {
                advance(); advance();
                add_token(TokenType::Elvis, "?:");
                continue;
            }
            if (c == '?' && peek_next() == '?') {
                advance(); advance();
                add_token(TokenType::QuestionQuestion, "??");
                continue;
            }
            if (c == '?' && peek_next() == '.') {
                advance(); advance();
                add_token(TokenType::QuestionDot, "?.");
                continue;
            }
            if (c == '=' && peek_next() == '>') {
                advance(); advance();
                add_token(TokenType::FatArrow, "=>");
                continue;
            }
            if (c == '=' && peek_next() == '=') {
                advance(); advance();
                add_token(TokenType::EqualEqual, "==");
                continue;
            }
            if (c == '!' && peek_next() == '!') {
                advance(); advance();
                add_token(TokenType::BangBang, "!!");
                continue;
            }
            if (c == '!' && peek_next() == '=') {
                advance(); advance();
                add_token(TokenType::NotEqual, "!=");
                continue;
            }
            if (c == '<' && peek_next() == '=') {
                advance(); advance();
                add_token(TokenType::LessEqual, "<=");
                continue;
            }
            if (c == '>' && peek_next() == '=') {
                advance(); advance();
                add_token(TokenType::GreaterEqual, ">=");
                continue;
            }
            if (c == '-' && peek_next() == '>') {
                advance(); advance();
                add_token(TokenType::Arrow, "->");
                continue;
            }
            if (c == '+' && peek_next() == '=') { advance(); advance(); add_token(TokenType::PlusEqual, "+="); continue; }
            if (c == '-' && peek_next() == '=') { advance(); advance(); add_token(TokenType::MinusEqual, "-="); continue; }
            if (c == '*' && peek_next() == '=') { advance(); advance(); add_token(TokenType::StarEqual, "*="); continue; }
            if (c == '/' && peek_next() == '=') { advance(); advance(); add_token(TokenType::SlashEqual, "/="); continue; }
            if (c == '%' && peek_next() == '=') { advance(); advance(); add_token(TokenType::PercentEqual, "%="); continue; }
            if (c == '|' && peek_next() == '|') { advance(); advance(); add_token(TokenType::DoublePipe, "||"); continue; }
            if (c == '&' && peek_next() == '&') { advance(); advance(); add_token(TokenType::DoubleAmpersand, "&&"); continue; }
            if (c == ':' && peek_next() == ':') { advance(); advance(); add_token(TokenType::DoubleColon, "::"); continue; }

            if (c == '=') { advance(); add_token(TokenType::Equal, "="); continue; }
            if (c == '!') { advance(); add_token(TokenType::Bang, "!"); continue; }
            if (c == '<') { advance(); add_token(TokenType::Less, "<"); continue; }
            if (c == '>') { advance(); add_token(TokenType::Greater, ">"); continue; }
            if (c == '+') { advance(); add_token(TokenType::Plus, "+"); continue; }
            if (c == '-') { advance(); add_token(TokenType::Minus, "-"); continue; }
            if (c == '*') { advance(); add_token(TokenType::Star, "*"); continue; }
            if (c == '/') { advance(); add_token(TokenType::Slash, "/"); continue; }
            if (c == '%') { advance(); add_token(TokenType::Percent, "%"); continue; }
            if (c == '|') { advance(); add_token(TokenType::Pipe, "|"); continue; }
            if (c == '&') { advance(); add_token(TokenType::Ampersand, "&"); continue; }
            if (c == ':') { advance(); add_token(TokenType::Colon, ":"); continue; }
            if (c == '?') { advance(); add_token(TokenType::Question, "?"); continue; }
            if (c == '.') { advance(); add_token(TokenType::Dot, "."); continue; }

            if (std::isalpha(c) || c == '_') { read_identifier(); continue; }

            error(std::string("unexpected character: ") + c);
        }
        add_token(TokenType::Newline, "\n");
        add_token(TokenType::EndOfFile, "");
        return tokens;
    }
}
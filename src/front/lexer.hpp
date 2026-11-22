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

#pragma once
#include "support/token.hpp"
#include <string>
#include <unordered_map>

namespace sinter {
    class Lexer {
    public:
        explicit Lexer(std::string src);
        Tokens tokenize();
    private:
        std::string source;
        size_t pos = 0, line = 1, column = 1;
        /// 当前 token 的起始列（1 基）。列号用于诊断的 ^ 指示，
        /// 必须在扫描之前抓取 —— add_token 时 column 已指向 token 末尾。
        size_t token_start_col = 1;
        Tokens tokens;
        int paren_depth = 0;
        int brace_depth = 0;
        int template_depth = 0;
        static const std::unordered_map<std::string, TokenType> keywords;

        [[nodiscard]] char peek() const;
        [[nodiscard]] char peek_next() const;
        [[nodiscard]] char peek_at(size_t n) const;
        char advance();
        bool match(char c);
        void skip_whitespace();
        void skip_comment();
        void add_token(TokenType t, const std::string& l);
        void read_string();
        void read_template_string();
        void read_char();
        void read_number();
        void read_identifier();
        void read_embed_c();
        [[noreturn]] void error(const std::string& msg) const;
    };
}

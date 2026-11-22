/*******************************************************************************
 * File: token
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
#include <string>
#include <vector>

namespace sinter {
    enum class TokenType {
        // 单字符
        LeftParen, RightParen, LeftBrace, RightBrace,
        LeftBracket, RightBracket, Comma, Dot, Semicolon,
        Colon, Question, BangBang, Equal, Plus, Minus, Star, Slash,
        Percent, Pipe, Less, Greater, Bang, At, Tilde, Ampersand,
        // 运算符
        DoubleDot, DoublePipe, DoubleAmpersand,
        EqualEqual, NotEqual, LessEqual, GreaterEqual,
        PlusEqual, MinusEqual, StarEqual, SlashEqual, PercentEqual,
        Arrow, FatArrow, DotDotDot, DoubleColon,
        // 新增运算符
        PipeGt,          // |>
        Elvis,           // ?:
        QuestionQuestion,// ??
        QuestionDot,     // ?.
        RangeOpen,       // ..<
        /// 关键字
        Module, Import, Fun, Class, Struct, Interface, Enum,
        Const, Val, Var, Ret, If, Else, While, For, Do,
        Break, Continue, True, False, Null, In, When, Is,
        Override, Must, Embed, Bind, From, Alias, Comptime,
        Pub, Private, Inline, Static, Abstract, Final, Extends, Implements, Super, Self,
        Annotation, Try, Throw, Catch, Finally, As,
        Del, Ext, Copy, Move, Default,
        Operator,
        /// 编译期位置关键字：`__FILE__` / `__MODULE__` / `__LINE__` / `__COLUMN__`
        /// / `__FUNCTION__` / `__CLASS__` / `__STRUCT__`。
        /// 它们在语义分析之前被折成字面量（见 front/comptime.cpp），
        /// 单独成 token 是为了让编辑器能把它们当关键字着色，而不是普通标识符。
        BuiltinFile, BuiltinModule, BuiltinLine, BuiltinColumn,
        BuiltinFunction, BuiltinClass, BuiltinStruct,
        // 字面量 / 标识符
        IntegerLiteral, FloatLiteral, StringLiteral, CharLiteral,
        Identifier, Newline, EndOfFile, Unknown, EmbedCBody,
    };

    struct Token {
        TokenType type;
        std::string lexeme;
        size_t line, column;

        explicit Token(const TokenType t = TokenType::Unknown, std::string l = "",
                       const size_t ln = 0, const size_t col = 0)
            : type(t), lexeme(std::move(l)), line(ln), column(col) {}
    };
    using Tokens = std::vector<Token>;
}

/*******************************************************************************
 * File: keywords
 * Project: sinter
 * Created: 2026/11/19
 *
 * 关键字表的**唯一来源**。
 *
 * 以往这张表藏在 src/front/lexer.cpp 的 `Lexer::keywords` 里，只有编译器自己看得见；
 * 编辑器侧（sinter-lsp / sinter-ide）只能抄一份，于是「语言加了新关键字、编辑器
 * 还当普通标识符」这种漂移迟早发生。现在改成一份 constexpr 数据：
 *   - 词法器启动时把它铺平成哈希表（热路径仍是 O(1) 查表）；
 *   - 编辑器直接用同一份数组做高亮、补全与声明识别。
 *
 * 新增关键字只改这里。
 *
 * 编译期位置关键字（`__FILE__` …）也在这张表里：它们是真正的关键字，
 * 编辑器必须能把它们和普通标识符区分开（`val __LINE__ = 1` 不是合法代码）。
 *******************************************************************************/

#pragma once
#include <array>
#include <string_view>

#include "support/token.hpp"

namespace sinter {
    struct KeywordEntry {
        std::string_view text;
        TokenType type;
    };

    /// 与 TokenType 一一对应的关键字表（顺序即词法器的查表内容，与顺序无关）
    inline constexpr std::array<KeywordEntry, 63> kKeywords = {{
            {"module", TokenType::Module}, {"import", TokenType::Import},
            {"fun", TokenType::Fun}, {"function", TokenType::Fun},
            {"class", TokenType::Class},
            {"struct", TokenType::Struct}, {"interface", TokenType::Interface},
            {"enum", TokenType::Enum}, {"const", TokenType::Const},
            {"val", TokenType::Val}, {"var", TokenType::Var},
            {"return", TokenType::Ret}, {"ret", TokenType::Ret},
            {"if", TokenType::If}, {"else", TokenType::Else},
            {"while", TokenType::While}, {"for", TokenType::For},
            {"do", TokenType::Do}, {"break", TokenType::Break},
            {"continue", TokenType::Continue}, {"true", TokenType::True},
            {"false", TokenType::False}, {"null", TokenType::Null},
            {"nil", TokenType::Null}, {"in", TokenType::In},
            {"when", TokenType::When}, {"is", TokenType::Is},
            {"override", TokenType::Override}, {"must", TokenType::Must},
            {"embed", TokenType::Embed}, {"bind", TokenType::Bind},
            {"from", TokenType::From}, {"alias", TokenType::Alias},
            {"comptime", TokenType::Comptime},
            {"pub", TokenType::Pub}, {"priv", TokenType::Private},
            {"annotation", TokenType::Annotation},
            {"as", TokenType::As}, {"try", TokenType::Try}, {"throw", TokenType::Throw},
            {"catch", TokenType::Catch}, {"finally", TokenType::Finally},
            {"inline", TokenType::Inline},
            {"static", TokenType::Static}, {"abstract", TokenType::Abstract},
            {"final", TokenType::Final}, {"extends", TokenType::Extends},
            {"implements", TokenType::Implements}, {"super", TokenType::Super},
            {"this", TokenType::Self}, {"self", TokenType::Self},
            {"del", TokenType::Del}, {"delete", TokenType::Del},
            {"ext", TokenType::Ext}, {"default", TokenType::Default},
            {"operator", TokenType::Operator},
            // 编译期位置关键字：折成字面量，见 front/comptime.cpp
            {"__FILE__", TokenType::BuiltinFile},
            {"__MODULE__", TokenType::BuiltinModule},
            {"__LINE__", TokenType::BuiltinLine},
            {"__COLUMN__", TokenType::BuiltinColumn},
            {"__FUNCTION__", TokenType::BuiltinFunction},
            {"__CLASS__", TokenType::BuiltinClass},
            {"__STRUCT__", TokenType::BuiltinStruct},
    }};

    [[nodiscard]] inline bool is_keyword(const std::string_view s) {
        for (const auto &k: kKeywords) {
            if (k.text == s) return true;
        }
        return false;
    }
} // namespace sinter

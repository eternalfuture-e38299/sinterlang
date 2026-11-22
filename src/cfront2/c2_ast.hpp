// cfront2: 第二个 C 前端（独立词法 + C99 递归下降解析）
//
// 与 src/cfront 的关系：
//   * cfront 负责 embed C / bind C header 的快速容错分析；
//   * cfront2 是一个独立的第二实现，用于 `sinterd cparse2/symbols2` 和
//     生成 C 的完整语法校验。两者没有共享 parser。
//
// 范围：C99 翻译单元（声明符、struct/union/enum、typedef、语句、完整表达式）。
// 预处理交给上游（cfront 的预处理器或编译器的 -E）；cfront2 会跳过 `#` 行并
// 记录 #include，因此也能直接读没有宏的生成 C。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sinter::cfront2 {
    enum class TokKind {
        Eof,
        Identifier,
        Keyword,
        IntLiteral,
        FloatLiteral,
        StringLiteral,
        CharLiteral,
        Punct,
        Preproc,
    };

    struct Token {
        TokKind kind = TokKind::Eof;
        std::string text;
        size_t offset = 0;
        int line = 1;
        int col = 1;
    };

    struct Diagnostic {
        int line = 0;
        int col = 0;
        std::string message;
    };

    struct Parameter {
        std::string name;
        std::string type;
    };

    struct FunctionDecl {
        std::string name;
        std::string return_type;
        std::vector<Parameter> params;
        bool variadic = false;
        bool definition = false;
        int line = 0;
    };

    struct FieldDecl {
        std::string name;
        std::string type;
        int line = 0;
    };

    struct RecordDecl {
        std::string kind;   // struct / union / enum
        std::string name;
        std::vector<FieldDecl> fields;
        std::vector<std::string> variants;   // enum
        int line = 0;
    };

    struct TypedefDecl {
        std::string name;
        std::string type;
        int line = 0;
    };

    struct Unit {
        std::vector<FunctionDecl> functions;
        std::vector<RecordDecl> records;
        std::vector<TypedefDecl> typedefs;
        std::vector<std::string> includes;
        std::vector<Diagnostic> diagnostics;
    };

    [[nodiscard]] Unit parse(const std::string &source);
} // namespace sinter::cfront2

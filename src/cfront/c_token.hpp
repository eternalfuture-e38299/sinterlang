// cfront: c_token.hpp —— 独立的 C 词法单元定义
//
// 这一层故意不依赖 Sinter 的 TokenType：C 的 token 集合与 Sinter 不同
// （预处理指令、存储类说明符、限定符等），混用会让两边都变难维护。
// 但行/列语义保持一致（1-based），方便 LSP 统一处理两种语言的位置。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sinter::cfront {
    enum class CTokKind : uint8_t {
        // ---- 特殊 ----
        EndOfFile,
        Unknown,
        // ---- 标识符 / 字面量 ----
        Identifier,
        IntLiteral,
        FloatLiteral,
        CharLiteral,
        StringLiteral,
        /// `__attribute__((...))` / `__declspec(...)` / `asm(...)` 整段吸收成一个 token
        Attribute,
        // ---- 预处理 ----
        Hash,             // # (行首)
        HashHash,         // ##
        PreprocLine,      // 整条预处理指令（保留原文，供 LSP 与后续展开使用）
        // ---- 关键字（C11 + 常用扩展）----
        KwAuto, KwBreak, KwCase, KwChar, KwConst, KwContinue, KwDefault,
        KwDo, KwDouble, KwElse, KwEnum, KwExtern, KwFloat, KwFor, KwGoto,
        KwIf, KwInline, KwInt, KwLong, KwRegister, KwRestrict, KwReturn,
        KwShort, KwSigned, KwSizeof, KwStatic, KwStruct, KwSwitch,
        KwTypedef, KwUnion, KwUnsigned, KwVoid, KwVolatile, KwWhile,
        KwAlignas, KwAlignof, KwAtomic, KwBool, KwComplex, KwGeneric, KwImaginary,
        KwNoreturn, KwStaticAssert, KwThreadLocal,
        // ---- 标点 / 运算符 ----
        LParen, RParen, LBrace, RBrace, LBracket, RBracket,
        Semicolon, Comma, Dot, Arrow, Ellipsis,
        Plus, Minus, Star, Slash, Percent,
        Amp, Pipe, Caret, Tilde, Bang,
        Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
        AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign,
        EqualEqual, NotEqual, Less, Greater, LessEqual, GreaterEqual,
        AmpAmp, PipePipe, Shl, Shr, PlusPlus, MinusMinus,
        Question, Colon,
        // ---- 杂项（LSP 用）----
        Comment,
    };

    /// 词法单元。保留 offset/line/column 三元组：
    /// - offset 用于精确切片原文（格式化器需要）
    /// - line/column 用于 LSP 的 Position（0-based 由 LSP 层转换）
    struct CToken {
        CTokKind kind = CTokKind::Unknown;
        std::string text;
        size_t offset = 0;
        uint32_t line = 1;
        uint32_t column = 1;
        /// 该 token 来自哪个文件：`CPreprocResult::files` 的下标（0 = 主文件）。
        /// 没有它，`bind C header "a.h"` 里 `#include` 进来的头报错时，位置会被
        /// 算到 a.h 头上 —— 行列对得上、文件是错的，比不报还难查。
        uint32_t file_id = 0;
        /// 预处理指令名（仅 PreprocLine 有效），如 "include" / "define" / "if"
        std::string directive;
        /// 该 token 之前是否有换行（格式化器用）
        bool leading_newline = false;
        /// 该 token 之前连续换行的次数（1 = 普通换行，>=2 = 存在空行）。格式化器据此
        /// 决定是否保留空行，避免把每个换行都渲染成空行。
        uint8_t blank_lines_before = 0;
        /// 该 token 之前的空格数（格式化器用）
        uint8_t leading_spaces = 0;

        size_t end_offset() const { return offset + text.size(); }
    };

    using CTokens = std::vector<CToken>;

    /// 关键词表：单一来源。name 必须与 CTokKind 一致，便于表驱动扩展。
    struct CKeyword {
        const char *name;
        CTokKind kind;
    };

    /// 返回全部 C 关键字（含 C11 与常用扩展）。顺序无关，查表内部建哈希。
    const CKeyword *c_keywords(size_t &count);

    /// 把 CTokKind 转成可读名字（诊断信息用）
    const char *c_tok_name(CTokKind k);
} // namespace sinter::cfront
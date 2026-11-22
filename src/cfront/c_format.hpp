// cfront: c_format.hpp —— C 代码格式化器
//
// 用途：sinterd 的 format 请求，以及 embed C 块内的自动缩进（LSP 用）。
// 策略：基于 token 流做「保留换行的规范化」——即在用户已有的换行处
// 只修正缩进与空格，不在用户没有换行的地方强行插入换行。
// 这样格式化结果稳定、可预测，不会把用户刻意的排版打散。
#pragma once
#include <string>

namespace sinter::cfront {
    struct CFormatOptions {
        int indent_width = 4;
        bool use_tabs = false;
        /// case 标签相对 switch 缩进几级（0 = 与 switch 同级）
        int case_indent_delta = 1;
        /// 保留用户原有换行（true）还是尽量压平（false）
        bool keep_line_breaks = true;
        /// 保留原有的空行（最多连续 max_blank_lines 个）
        int max_blank_lines = 1;
    };

    /// 格式化一段 C 代码。即使代码有语法错误也会尽量输出（容错），
    /// 因为它基于 token 而不是 AST。
    std::string format_c(const std::string &source, const CFormatOptions &opt = {});

    /// 只重新缩进，不改任何 token 之间的空格与换行。
    /// embed C 块内做「自动缩进」时用这个，最保守。
    std::string reindent_c(const std::string &source, int base_indent, const CFormatOptions &opt = {});
} // namespace sinter::cfront
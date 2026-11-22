// cfront: c_preproc.hpp —— 轻量 C 预处理器
//
// 存在意义：真实 C 头文件里到处都是 `#ifndef X_H` 头文件保护、`#if defined(__GNUC__)`
// 条件声明和 `#define` 常量/短宏。只把预处理行当成「原样文本」跳过的解析器，
// 会把 `#if __GNUC__` 里的两套声明都当成有效代码，或者被 `extern "C" {` 直接噎住。
//
// 边界（刻意）：
//   - 不做 `#` / `##` 的完整字符串化与拼接：遇到就原样保留，不报错；
//   - 不做 `#include` 的宏展开形式（`#include MACRO`）；
//   - 只有 `#pragma once` 与「正在展开中的文件」（防 A→B→A）会被跳过。刻意**不**做
//     「同一文件只展开一次」：musl 的 bits/alltypes.h 每次包含时靠 `__NEED_clock_t`
//     这类宏决定要 typedef 什么，隐式 once 会静默丢掉 clock_t / pid_t。
#pragma once
#include "cfront/c_token.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sinter::cfront {
    struct CPreprocOptions {
        /// `-I` 搜索路径（`#include <...>` 只用这些；`#include "..."` 先看当前文件目录）
        std::vector<std::string> include_dirs;
        /// 预定义宏（等价于命令行 `-D name=body`）
        std::vector<std::pair<std::string, std::string> > predefined;
        int max_include_depth = 32;
    };

    struct CPreprocResult {
        /// 已展开的 token 流（不含 PreprocLine），可直接交给语法分析器
        CTokens tokens;
        /// 原始预处理行（LSP / 格式化器需要保留原文）
        std::vector<CToken> directives;
        /// 错误文本（已带 file:line:col 前缀）
        std::vector<std::string> errors;
        /// 找不到的 `#include` 目标：调用方决定是硬错误还是忽略
        std::vector<std::string> unresolved_includes;
        /// `CToken::file_id` → 文件路径。0 一定是主文件；被 `#include` 进来的
        /// 文件按首次出现的顺序追加。诊断据此把错误归到真正出错的文件上。
        std::vector<std::string> files;
        /// 最终生效的宏名（调试 / LSP 悬停用）
        std::set<std::string> macros;
        size_t include_count = 0;
    };

    /// 预处理一段 C 源码。`path` 只用于诊断与 `#include "..."` 的相对解析。
    CPreprocResult preprocess_c(const std::string &source, const std::string &path,
                                const CPreprocOptions &options = {});
} // namespace sinter::cfront

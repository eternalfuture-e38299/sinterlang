// sinter: format.hpp —— Sinter 源码格式化（sinterd / sinter-lsp 共用）
//
// 这是 Sinter 的格式化器，和 `src/cfront/c_format.*`（C 代码格式化）是两件事：
//   * sinterd 的 `format` 请求格式化 .sinter 源码 → 必须用这一份；
//   * `reindent` 请求只给 `embed C { ... }` 的正文做缩进 → 用 cfront 的 C 版本。
//
// 只重排空白与换行，不改动任何非空白字符；规则与行缓冲实现见 .cpp 文件头。
#pragma once

#include <cstdint>
#include <string>

namespace sinter::format {
    /// 格式化选项。默认值与 `.editorconfig` 惯例一致。
    struct FormatOptions {
        int32_t tab_size = 4;       ///< 每层缩进的空格数
        bool trim_trailing = true;  ///< 去掉行尾空白（当前实现总是去）
        bool final_newline = true;  ///< 文件末尾补一个换行
    };

    /// 格式化 Sinter 源码。
    ///
    /// **只动空白**：非空白字符的顺序与内容保证不变（含字符串、注释、`embed C`
    /// 正文）。因此可以对语法不完整的代码调用 —— 编辑器里的文件经常正在敲。
    [[nodiscard]] std::string format_sinter(const std::string &src, const FormatOptions &opt = {});
} // namespace sinter::format

// sinter-lsp: format.hpp —— Sinter 源码格式化（LSP 侧包装）
//
// 真正的格式化器在 `src/format/sinter_format.*`，sinterd 与 sinter-lsp 共用
// 同一份实现。这里只保留「格式化结果 → LSP TextEdit[]」的转换，避免两个
// 工具各写一套规则（之前 sinterd 误用 C 格式化器，把 `=>` 拆成 `= >`）。
#pragma once

#include <string>

#include "document.hpp"
#include "format/sinter_format.hpp"
#include "json.hpp"

namespace sinterlsp {
    using FormatOptions = sinter::format::FormatOptions;
    using sinter::format::format_sinter;

    /// 把格式化结果包成 LSP `TextEdit[]`。无变化时返回空数组。
    ///
    /// 用**整文件替换**而不是增量 range：行数一变，增量 range 就容易错位，
    /// 而订阅全量同步的客户端（CLion 插件）本来就要整篇文本。
    Json format_to_lsp_edits(const std::string &original, const std::string &formatted,
                             const Document &doc);
} // namespace sinterlsp

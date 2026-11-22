// c_bridge —— 把 C 头文件里的声明翻译成 Sinter 的 AST 节点
//
// 位置：只属于 driver 层。C 前端（cfront）与语义层（sinter_sem）互不认识，
// 由这里把 `bind C header "x.h"` 解析出来的 C 声明转换成：
//   * BindCFunctionStmt  —— 函数原型 → Sinter 可直接按名字调用的外部函数
//   * CStructDeclStmt    —— struct/union/typedef 结构体 → 不透明 C 类型
//
// 类型映射规则集中在 map_base()/build()，是「C ABI ↔ Sinter 类型」的唯一来源。
#pragma once
#include "front/ast.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sinter_bridge {
    struct Options {
        std::vector<std::string> include_dirs;                        ///< -I
        std::vector<std::pair<std::string, std::string> > predefined;  ///< -D name=body
    };

    struct Result {
        /// 成功时注入模块 AST 的节点（BindCFunctionStmt / CStructDeclStmt）
        std::vector<sinter::StmtPtr> nodes;
        /// 失败时的人类可读诊断（英文，已带 file:line:col 前缀）
        std::vector<std::string> errors;
        int functions = 0;
        int structs = 0;
        int skipped = 0;   // 无法映射而被跳过的声明数（已计入 errors）
    };

    /// 解析 `path` 指向的 C 头文件并转换成 Sinter AST 节点。
    /// `display` 只用于诊断里的文件名（通常是用户在 bind C header 里写的路径）。
    Result load_header(const std::string &path, const std::string &display, const Options &options);
} // namespace sinter_bridge

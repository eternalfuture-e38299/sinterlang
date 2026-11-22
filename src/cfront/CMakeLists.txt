# cfront：C 前端的词法与语法分析
#
# 两个消费者：
#   1. sinterd —— Sinter 的 embed C { ... } 块里是原样透传的 C 代码。要给 LSP
#      提供跨语言能力（跳转、补全、悬停），就需要对这段 C 代码建独立的
#      token 流与 AST。
#   2. sinterc —— 编译期解析 `bind C header "x.h"` 指定的真实头文件
#      （经 src/driver/c_bridge.cpp），把函数原型与不透明结构体翻成 Sinter AST。
# 它仍是独立静态库 sinter_cfront：语义层与代码生成后端都不依赖它。
# 只做「结构」不做 C 语义/类型检查；预处理能展开常见宏，但不追求完整宏系统。
add_library(sinter_cfront STATIC
        c_token.cpp        # 关键字表 + token 名称表（单一来源）
        c_lexer.cpp        # C 词法：字面量、运算符、预处理行、注释
        c_preproc.cpp      # C 预处理：条件编译、宏展开、#include
        c_ast.cpp          # 类型规范化 + 符号收集
        c_parser.cpp       # 顶层 / 类型说明符 / 声明符
        c_parser_stmt.cpp  # 语句
        c_parser_expr.cpp  # 表达式（优先级爬升）
        c_format.cpp       # 格式化（sinterd 的 format 请求）
)

target_link_libraries(sinter_cfront PUBLIC sinter_support)
sinter_apply_common(sinter_cfront)

# C 前端自测（ctest 注册）
add_executable(cfront_selftest cfront_selftest.cpp)
target_link_libraries(cfront_selftest PRIVATE sinter_cfront)
sinter_apply_common(cfront_selftest)
set_target_properties(cfront_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

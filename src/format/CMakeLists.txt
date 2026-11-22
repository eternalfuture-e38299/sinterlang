# sinter_format：Sinter 源码格式化
#
# 单一实现，sinterd 的 `format` 请求与 sinter-lsp 的 textDocument/formatting
# 都链接它。与 cfront 的 `c_format`（C 代码格式化）区分：
# 后者只用于 `embed C { ... }` 正文的缩进。
add_library(sinter_format STATIC
        sinter_format.cpp
)

sinter_apply_common(sinter_format)
# sinter_apply_common 的 include 是相对调用目录算的；这里显式给出源码根，
# 让 `#include "format/sinter_format.hpp"` 在 sinterd / sinter-lsp 里成立。
target_include_directories(sinter_format PUBLIC ${CMAKE_SOURCE_DIR}/src)

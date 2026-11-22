# support：词法记号、诊断、通用小工具，其它模块都依赖它
add_library(sinter_support STATIC
        diagnostics.cpp
)

target_include_directories(sinter_support
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

sinter_apply_common(sinter_support)
set_target_properties(sinter_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
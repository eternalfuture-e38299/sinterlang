# front：词法分析 + 语法分析 + AST + 泛型单态化
add_library(sinter_front STATIC
        lexer.cpp
        parser.cpp
        generic_mono.cpp
        comptime.cpp
)

target_link_libraries(sinter_front PUBLIC sinter_support)
sinter_apply_common(sinter_front)

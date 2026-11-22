# cfront2：第二个 C 前端（独立词法 + C99 解析）
add_library(sinter_cfront2 STATIC
        c2_lexer.cpp
        c2_parser.cpp
)
sinter_apply_common(sinter_cfront2)
target_include_directories(sinter_cfront2 PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_executable(cfront2_selftest cfront2_selftest.cpp)
target_link_libraries(cfront2_selftest PRIVATE sinter_cfront2)
sinter_apply_common(cfront2_selftest)
target_include_directories(cfront2_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(cfront2_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME cfront2_selftest COMMAND cfront2_selftest)

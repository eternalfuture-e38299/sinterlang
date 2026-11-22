# sinter-lsp —— Sinter 语言服务器（LSP over stdio）
#
# 依赖：sinter_front（拿 Sinter 词法器，用于符号索引）
#       sinter_support（诊断与 token 定义）
# 不链接后端与语义层：LSP 需要的是「快速、容错」，而非完整编译。
add_executable(sinter-lsp
        main.cpp
        json.cpp
        document.cpp
        transport.cpp
        index.cpp
        server.cpp
        features.cpp
        format.cpp
        self_test.cpp
)

target_include_directories(sinter-lsp PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(sinter-lsp PRIVATE sinter_front sinter_cfront sinter_format)
target_compile_options(sinter-lsp PRIVATE ${SINTER_WARNING_FLAGS})

set_target_properties(sinter-lsp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 自测：不需要编辑器，直接验证协议与五项能力。
#
# 两个细节：
#   1) 用 $<TARGET_FILE:...> 而不是裸目标名：产物被 RUNTIME_OUTPUT_DIRECTORY 移到了 bin/，
#      ctest 不会去那里找（裸名会报 BAD_COMMAND）。
#   2) 用 sh -c 先复制到 /tmp 再执行：本项目位于 /sdcard（noexec 挂载点），
#      直接执行会失败；/sdcard 与 /tmp 同属一个 proot，复制后可正常运行。
add_test(NAME sinter_lsp_selftest
        COMMAND sh -c "cp \"$1\" /tmp/sinter-lsp-under-test && chmod 755 /tmp/sinter-lsp-under-test && exec /tmp/sinter-lsp-under-test --self-test" sh
        $<TARGET_FILE:sinter-lsp>)

# 端到端：真的起子进程，按 LSP 规范用 Content-Length 帧走一遍编辑器会话。
# 自测直接调 handle_message，绕过了 stdio 与帧解析，这一步专门补那一段。
# 需要 python3（仅标准库）；环境里没有就跳过，不让整套 ctest 失败。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME sinter_lsp_integration
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/lsp_integration_test.py
                    $<TARGET_FILE:sinter-lsp>)
endif()
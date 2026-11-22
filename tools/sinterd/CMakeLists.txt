# sinterd —— Sinter 语言后端守护进程
#
# 只依赖 cfront（C 前端的词法/语法/格式化）。不链接 Sinter 编译器的后端与语义层，
# 这样它既能编进 Android proot 这类受限环境，也不会因为编译器内部改动而跟着重编。
add_executable(sinterd
        main.cpp
        protocol.cpp
        server.cpp
        ops.cpp
        ops_tokens.cpp
)

target_include_directories(sinterd PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(sinterd PRIVATE sinter_cfront sinter_cfront2 sinter_format)
target_compile_options(sinterd PRIVATE ${SINTER_WARNING_FLAGS})

set_target_properties(sinterd PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 自测作为 CTest 用例注册：--self-test 不需要 socket，能在任意环境跑。
#
# 两个细节：
#   1) 用 $<TARGET_FILE:...> 而不是裸目标名：产物被 RUNTIME_OUTPUT_DIRECTORY 移到了 bin/，
#      ctest 不会去那里找（裸名会报 BAD_COMMAND）。
#   2) 用 sh -c 先复制到 /tmp 再执行：本项目位于 /sdcard（noexec 挂载点），
#      直接执行会失败；/sdcard 与 /tmp 同属一个 proot，复制后可正常运行。
add_test(NAME sinterd_selftest
        COMMAND sh -c "cp \"$1\" /tmp/sinterd-under-test && chmod 755 /tmp/sinterd-under-test && exec /tmp/sinterd-under-test --self-test" sh
        $<TARGET_FILE:sinterd>)

# 端到端：真的起守护进程、走 Unix socket。
# 自测直接调 handle_request_line，看不见「一次 recv 带回多条请求」、
# 「数字 id 回显」这类只在真实连接上暴露的问题，这一步专门补那一段。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME sinterd_e2e
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/sinterd_e2e_test.py
                    $<TARGET_FILE:sinterd>)
endif()
# driver：编译器命令行入口
add_executable(sinterc
        main.cpp
        c_bridge.cpp   # `bind C header`：C 头文件声明 → Sinter AST
)

target_link_libraries(sinterc PRIVATE sinter_backend sinter_sem sinter_front sinter_cfront sinter_support)
sinter_apply_common(sinterc)

set_target_properties(sinterc PROPERTIES
        OUTPUT_NAME sinterc
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
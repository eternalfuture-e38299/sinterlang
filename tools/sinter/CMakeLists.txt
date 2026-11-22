# sinter：包管理器 + 构建前端
add_executable(sinter
        main.cpp
        manifest.cpp
        registry.cpp
        bundled.cpp
        cmake_gen.cpp
)

target_include_directories(sinter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
sinter_apply_common(sinter)

# 内置包目录（仓库根下的 packages/），编译期注入便于分发后仍能找到
target_compile_definitions(sinter PRIVATE
        SINTER_BUNDLED_DIR="${CMAKE_SOURCE_DIR}/packages")

set_target_properties(sinter PROPERTIES
        OUTPUT_NAME sinter
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
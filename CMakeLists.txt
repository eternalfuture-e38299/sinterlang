cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# 版本号唯一来源：仓库根目录的 VERSION。
# configure_file 会把它生成到 build/generated/sinter/version.hpp，
# 自举工具链则由 sinter 读取同一个文件并传 -DSINTER_VERSION_STRING。
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SINTER_VERSION)
string(STRIP "${SINTER_VERSION}" SINTER_VERSION)

project(Sinter
        VERSION ${SINTER_VERSION}
        DESCRIPTION "Sinter language compiler (Sinter -> C)"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# build/generated/sinter/version.hpp 是唯一被 C++ TU include 的版本头。
# ---------------------------------------------------------------------------
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/support/version.hpp.in"
               "${CMAKE_BINARY_DIR}/generated/sinter/version.hpp"
               @ONLY)

# ---------------------------------------------------------------------------
# 全局设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

option(SINTER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"   OFF)
option(SINTER_USE_PCH      "Use precompiled header to speed up builds" ON)

set(SINTER_WARNING_FLAGS -Wall -Wextra -Wpedantic)
if(SINTER_WARNINGS_AS_ERRORS)
    list(APPEND SINTER_WARNING_FLAGS -Werror)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# 预编译头：所有 TU 共享同一份，显著加快并行构建
# ---------------------------------------------------------------------------
set(SINTER_PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/support/pch.hpp")
if(SINTER_USE_PCH AND NOT EXISTS "${SINTER_PCH_HEADER}")
    set(SINTER_USE_PCH OFF)
endif()

# 给目标统一挂上 warning + PCH
function(sinter_apply_common target)
    target_include_directories(${target}
            PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/src"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # 注意用顶层 CMAKE_BINARY_DIR：这个函数在子目录里也会被调用，
    # 用 CMAKE_CURRENT_BINARY_DIR 会变成 build/tools/sinter/generated，找不到 version.hpp。
            PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_compile_options(${target} PRIVATE ${SINTER_WARNING_FLAGS})
    if(SINTER_USE_PCH)
        target_precompile_headers(${target} PRIVATE "${SINTER_PCH_HEADER}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 各子模块（静态库），拆细后可以并行编译、单独复用
# ---------------------------------------------------------------------------
# enable_testing() 必须在任何 add_test() 之前调用，否则子目录里注册的用例会被丢弃
# （之前 sinterd / sinter-lsp 的自测没能进 ctest，原因就在这里）。
if(SINTER_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src/support)
add_subdirectory(src/cfront)   # C 前端（供 embed C 块分析与 LSP 使用）
add_subdirectory(src/cfront2)  # 第二个 C 前端（独立实现，用于完整 C99 校验/符号）
add_subdirectory(src/format)   # Sinter 源码格式化（sinterd / sinter-lsp 共用）
add_subdirectory(src/front)
add_subdirectory(src/sem)
add_subdirectory(src/backend)
add_subdirectory(src/driver)

# ---------------------------------------------------------------------------
# 包管理器（独立小工具）
# ---------------------------------------------------------------------------
add_subdirectory(tools/sinter)
add_subdirectory(tools/sinterd)   # 后端守护进程：格式化 + C 语法分析
add_subdirectory(tools/sinter-lsp) # LSP 前端（JSON-RPC over stdio）

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT sinterc)

# ==================== Release strip ====================
# 收集所有需要 strip 的目标：可执行文件 + 共享库（静态库不 strip）
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_strip_cmd "${CMAKE_STRIP}" --strip-all)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_strip_cmd "${CMAKE_STRIP}" -S)
    endif()

    if(_strip_cmd)
        add_custom_target(strip_all ALL
                COMMAND ${CMAKE_COMMAND} -E echo "Stripping release binaries..."
                COMMAND ${_strip_cmd} $<TARGET_FILE:sinterc>
                COMMAND ${_strip_cmd} $<TARGET_FILE:sinter>
                COMMAND ${_strip_cmd} $<TARGET_FILE:sinterd>
                COMMAND ${_strip_cmd} $<TARGET_FILE:sinter-lsp>
                DEPENDS sinterc sinter sinterd sinter-lsp
                COMMENT "Stripping release binaries"
                VERBATIM)
    endif()
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(SINTER_BUILD_TESTS)
endif()

# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
install(TARGETS sinterc sinter sinterd sinter-lsp
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/sinter
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# 四份文档缺一不可：LSP.md 以前漏装，用户拿到的包里就没有编辑器支持那一份。
install(FILES docs/LANGUAGE.md docs/CMAKE.md docs/PACKAGES.md docs/LSP.md
        LICENSE README.md CHANGELOG.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)

message(STATUS "Sinter ${PROJECT_VERSION} | build=${CMAKE_BUILD_TYPE} | pch=${SINTER_USE_PCH} | tests=${SINTER_BUILD_TESTS}")
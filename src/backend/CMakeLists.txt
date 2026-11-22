# backend：C 代码生成（按关注点拆成多个 TU，便于并行编译）
add_library(sinter_backend STATIC
        codegen.cpp            # 顶层驱动：generate_c / generate_h / emit_program_body
        codegen_types.cpp      # 类型推断与 C 类型映射
        codegen_lifecycle.cpp  # 构造/析构/拷贝/移动 + 函数声明与定义
        codegen_stmts.cpp      # 语句生成
        codegen_exprs.cpp      # 表达式生成
)

target_link_libraries(sinter_backend PUBLIC sinter_sem)
sinter_apply_common(sinter_backend)
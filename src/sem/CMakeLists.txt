# sem：名称解析、类型检查、模块系统
add_library(sinter_sem STATIC
        semantic.cpp
        typeck.cpp
        ownership.cpp
)

target_link_libraries(sinter_sem PUBLIC sinter_front)
sinter_apply_common(sinter_sem)

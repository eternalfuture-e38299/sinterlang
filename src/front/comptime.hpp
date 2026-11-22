// sinter front: 编译期特性
//   - Target.* 常量折叠
//   - comptime if 条件裁剪
//   - comptime 块明确报错（不再静默丢弃）
//   - 源位置关键字 __FILE__ / __MODULE__ / __LINE__ / __COLUMN__ /
//     __FUNCTION__ / __CLASS__ / __STRUCT__ 折叠
//
// 为什么拆成两遍、各自在什么时机跑：
//   - 第一遍 apply_comptime 必须在 instantiate_generic_types 之前：
//     `GenericMono` 要看到被 comptime if 裁掉的分支，否则会去实例化永不会
//     用到的泛型；`Semantic` 也不该看到 `Target.*`。
//   - 第二遍 apply_source_sites 必须在单态化之后：`__CLASS__` 在 `Box<T>`
//     的实例体里应该是 `Box_i64`，提前折只能拿到模板名。
//   - 两遍都在 Semantic 之前：语义层与代码生成都不认识这些关键字，
//     只认识字面量。
#pragma once
#include "front/ast.hpp"
#include "front/generic_mono.hpp"
#include <string>
#include <utility>
#include <vector>

namespace sinter {
    /// 编译期目标描述。命令行可覆盖，默认取宿主编译目标。
    struct ComptimeTarget {
        std::string os = "unknown";     ///< Target.os
        std::string arch = "unknown";   ///< Target.arch
        int pointer_bits = 64;          ///< Target.pointerBits
        std::string endian = "little";  ///< Target.endian
        int long_bits = 64;             ///< Target.longBits
        bool is_debug = false;          ///< Target.isDebug

        /// 用 -D key=value / --target-os= / --target-arch= 覆盖
        void apply_defines(const std::vector<std::pair<std::string, std::string> > &defs);
    };

    /// 解析 `--target-os=xxx` / `--target-arch=xxx` / `--release` / `--debug`。
    /// 命中返回 true 并写入 t。
    bool parse_target_flag(const std::string &arg, ComptimeTarget &t);

    /// 按当前宿主推断默认目标。
    ComptimeTarget host_target();

    /// 第一遍（泛型单态化之前）：
    ///   - 把 `Target.os` 等折叠成字面量
    ///   - `comptime if cond { A } else { B }` 按 cond 保留一支、整体替换
    ///   - `comptime { ... }` 与无法求值的 `comptime` → 明确报错（不再静默丢弃）
    /// 源位置关键字这一遍保持原样（它们的函数 / 类型名要到单态化之后才定）。
    /// 返回 false 时 error 里带「[模块] 文件:行:列: 原因」。
    bool apply_comptime(std::vector<GenericModule> &modules, const ComptimeTarget &target,
                        std::string &error);

    /// 第二遍（泛型单态化之后、语义分析之前）：
    /// 把 `__FILE__` / `__MODULE__` / `__LINE__` / `__COLUMN__` /
    /// `__FUNCTION__` / `__CLASS__` / `__STRUCT__` 折成字面量。
    /// 上下文的边界：
    ///   - `__FUNCTION__` 取最近的外层函数 / 方法名（lambda 不另起名字）；
    ///   - `__CLASS__` / `__STRUCT__` 取最近的外层类型，用错关键字会报错；
    ///   - 两者在类型体 / 函数体之外使用会报错，而不是给出空串。
    bool apply_source_sites(std::vector<GenericModule> &modules, const ComptimeTarget &target,
                            std::string &error);
} // namespace sinter

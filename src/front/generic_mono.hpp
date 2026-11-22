// sinter front: 泛型类 / 泛型结构体单态化
#pragma once
#include "front/ast.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace sinter {
    /// 一个待展开的模块（名字 + 已解析的 AST）
    struct GenericModule {
        std::string module_name;
        Program *program = nullptr;
        /// 源文件路径（`__FILE__` 的值）。空表示「这个模块不是从文件加载的」，
        /// 只有内存里手工构造的模块才会空。
        std::string source_path;
    };

    /// 单态化产出的实例 → 模板形参表。
    /// 代码生成阶段靠它把实例名尾部的类型后缀（`_str` / `_str_i64`）还原成
    /// 类型实参，从而把 `sinter_gen_clone(v)` 这类按 T 分派的多态调用
    /// 落到 `sinter_gen_clone_str(v)` 上。
    struct GenericInstantiation {
        const std::string *decl_name = nullptr;   // 实例名（`List_str`）
        std::vector<TemplateParam> tparams;       // 模板形参（`<T>`）
        std::string module_name;                  // 实例落在哪个模块
    };

    /// 单态化副产物：实例的类型实参名表。语义层与代码生成共用同一份。
    class GenericTable {
    public:
        void clear() { items_.clear(); }
        void add(const std::string &inst, const std::vector<TemplateParam> &tparams,
                 const std::string &module_name);

        /// 把实例名（可带模块前缀）归约成类型实参名。
        /// 例如 `std.generic.List_str` → {"str"}，`Map_str_i64` → {"str","i64"}。
        bool split(const std::string &inst, std::vector<std::string> &args) const;
        /// 模板名 + 类型实参名 → 实例短名（`List` + {"str"} → `List_str`）
        static std::string compose(const std::string &tmpl, const std::vector<std::string> &args);

        /// 取某个实例对应的模板形参表（用于按形参名解析「类型标签」）。
        bool params_of(const std::string &inst, std::vector<TemplateParam> &out) const {
            std::string s = inst;
            const auto dot = s.rfind('.');
            if (dot != std::string::npos) s = s.substr(dot + 1);
            const auto it = items_.find(s);
            if (it == items_.end()) return false;
            out = it->second.tparams;
            return true;
        }
        /// 全部实例短名（遍历用）
        void names(std::vector<std::string> &out) const {
            for (const auto &kv: items_) out.push_back(kv.first);
        }

    private:
        struct Item {
            std::vector<TemplateParam> tparams;
            std::string module_name;
        };
        std::unordered_map<std::string, Item> items_;   // 实例短名 -> 形参表
    };

    /// 把泛型类/结构体展开成普通类：
    ///   class Box<T>(...) + 使用处的 `Box<i64>` → 声明模块里多出一个普通类 `Box_i64`，
    ///   所有类型注解与构造调用都被改写成这个具体类型；模板本身从 AST 中移除。
    /// 同名实例只生成一份；跨模块使用会记录到声明它的模块里，并给你用方补 import。
    /// 失败时 error 里带可读原因（例如构造缺少显式类型实参）。
    bool instantiate_generic_types(std::vector<GenericModule> &modules, std::string &error,
                                   GenericTable *table = nullptr);
} // namespace sinter

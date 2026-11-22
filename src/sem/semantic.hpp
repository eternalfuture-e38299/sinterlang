// sinter semantic — 占位，实际实现见下方 (此文件随后被完整覆盖)
#pragma once
#include "front/ast.hpp"
#include "support/diagnostics.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <optional>
#include <cstdio>
#include <cstdlib>

namespace sinter {
    // ==========================================================================
    // 类型描述
    // ==========================================================================
    struct CType {
        enum Kind { Void, Int, Float, Bool, Str, Char, Ptr, Struct, Enum, Fn, Unknown };
        Kind kind = Unknown;
        std::string name;       // C 侧名字（struct xxx / const char* ...）
        int bits = 0;
        bool is_signed = true;
        bool nullable = false;
        bool is_ref = false;   // T& (non-null reference)
        std::string sinter_name; // Sinter 侧类型名

        std::string to_c() const;
        bool is_numeric() const { return kind == Int || kind == Float; }
        static CType make_int(int bits, bool is_signed, const std::string &sinter);
        static CType make_void();
        static CType make_bool();
        static CType make_str();
        static CType make_double();
        static CType make_float();
        static CType unknown();
    };

    // ==========================================================================
    // 符号
    // ==========================================================================
    struct ParamInfo {
        std::string name;
        std::string c_name;        // 生成 C 的形参名（snake_case；空则按 name 现算）
        std::string type_c;
        std::string type_sinter;
    };

    struct FunctionInfo {
        std::string name;          // Sinter 名
        std::string c_name;        // C 名（含重载 disambiguator）
        std::string module_name;
        std::vector<ParamInfo> params;
        std::string return_type_name;
        FunctionDecl *decl = nullptr;
        bool is_extern = false;
        bool variadic = false;
        bool is_pub = false;
        bool is_inline = false;
        bool is_static = false;
        bool is_method = false;
        std::string owner_class;   // 方法所属类
        bool is_virtual = false;
        int vtable_slot = -1;
        std::vector<std::string> template_params;
        std::vector<std::string> template_constraints;
        std::string ext_receiver;
        std::vector<std::string> aliases_via; // fun f2 = f1 登记的目标

        // 可变参数（`fun f(xs: i64...)`）：is_varargs 为真时，params.back() 是
        // 接收全部剩余实参的那个形参，其类型是「元素类型」而非「数组类型」；
        // 调用点会按实参个数把整组数据绑定成一个局部数组再传进来。
        bool is_varargs = false;

        // 重载/别名签名：参数类型串
        [[nodiscard]] std::string signature() const;

        /// 可变参数的「元素类型」：register_function 会把末位形参记成 `T*`。
        /// 解析实参和生成调用点时都要还原成 `T` 才能与单个实参比较。
        [[nodiscard]] std::string varargs_elem_type() const {
            if (params.empty()) return "long long";
            std::string c = params.back().type_c;
            while (!c.empty() && (c.back() == '*' || c.back() == ' ')) c.pop_back();
            return c.empty() ? std::string("long long") : c;
        }
    };

    struct FieldInfo {
        std::string name;      // Sinter 侧字段名
        std::string c_name;    // 生成 C 的字段名（snake_case）
        std::string type_c;
        std::string type_sinter;
        bool is_public = true;
        bool is_mutable = true;
        bool is_static = false;
        VarDeclStmt *decl = nullptr;   // 静态字段的原始声明（取初值用）
    };

    struct ClassInfo {
        std::string name;
        std::string c_name;
        std::string module_name;
        std::vector<ParamInfo> ctor_params;
        std::vector<FieldInfo> fields;
        std::string base_class;
        std::vector<std::string> interfaces;
        bool has_destructor = false;
        bool is_struct = false;
        /// 外部 C 结构体：只有声明没有布局，生成 C 时只发前置声明，
        /// 类型拼写直接用 c_name（`struct Foo` 或 `FILE`）。
        bool opaque = false;
        bool is_abstract = false;
        bool is_pub = false;
        bool is_annotation = false;          // annotation class: compile-time only
        const ClassDecl *decl = nullptr;     // primary constructor defaults (same-module codegen)
        std::vector<bool> annotation_defaults; // per annotation parameter
        std::vector<std::pair<std::string, int> > vtable; // method -> slot
        std::vector<std::string> vtable_slots;            // slot -> owner.method
        // 跨模块：外部（import 来的）基类信息
        std::string base_module;      // 基类所属模块 ident
        std::string base_c_name;      // 基类 C 结构名（含模块前缀）
        std::vector<FieldInfo> inherited_fields;  // 从外部基类继承来的字段
        std::vector<std::pair<std::string, int> > inherited_vtable;  // 方法名 -> 已有槽位
        std::vector<std::string> inherited_slots;                   // 槽位 -> owner.method
        std::vector<ParamInfo> base_ctor_params;  // 外部基类构造参数
    };

    struct EnumInfo {
        std::string name;
        std::string c_name;
        std::string module_name;
        std::vector<std::string> variants;
        bool is_pub = false;
    };

    // ==========================================================================
    // 诊断：统一使用 support/diagnostics.hpp 的 Diagnostic（含行列与源码片段渲染）
    // ==========================================================================
    // 此前这里另有一份同名的 Diagnostic（level/message/file/line），
    // 与 support 版本重复且没有列号，导致报错无法指到具体 token。
    // 现已统一，Semantic 只负责收集，由 driver 统一渲染。

    // ==========================================================================
    // 语义分析器
    // ==========================================================================
    class Semantic {
    public:
        Semantic();
        // 跨模块继承：登记来自其它模块的类布局信息（由 main 的模块加载器调用）
        void declare_external_class(const ClassInfo &ci) { external_classes_[ci.name] = ci; }
        const std::unordered_map<std::string, ClassInfo> &external_classes() const { return external_classes_; }
        const ClassInfo *find_class(const std::string &n) const {
            auto it = classes_.find(n);
            if (it != classes_.end()) return &it->second;
            auto e = external_classes_.find(n);
            if (e != external_classes_.end()) return &e->second;
            return nullptr;
        }
        void analyze(Program &p, const std::string &module_name);

        /// 泛型实例的类型实参表：`List_str` → {"str"}、`Map_str_i64` → {"str","i64"}。
        /// 单态化在语义分析之前完成，实例本体里已没有 `T`；方法体里按类型分派的
        /// 多态调用（`sinter_gen_clone(v)` 之类）靠这张表在语义层解析出具体函数。
        void declare_generic_instance(const std::string &inst, const std::vector<std::string> &args) {
            generic_args_[inst] = args;
        }
        const std::vector<std::string> *generic_args_of(const std::string &n) const {
            const auto it = generic_args_.find(n);
            return it == generic_args_.end() ? nullptr : &it->second;
        }
        bool stdlib_imported() const { return stdlib_imported_; }

        const std::unordered_map<std::string, std::vector<FunctionInfo> > &functions() const { return functions_; }
        const std::unordered_map<std::string, ClassInfo> &classes() const { return classes_; }
        const std::unordered_map<std::string, EnumInfo> &enums() const { return enums_; }
        const std::unordered_map<std::string, FieldInfo> &static_fields() const { return static_fields_; }
        const std::unordered_map<std::string, std::string> &aliases() const { return aliases_; }
        const std::unordered_map<std::string, std::string> &var_types() const { return var_types_; }
        const std::vector<Diagnostic> &diagnostics() const { return diags_; }
        bool has_errors() const;

        std::string module_to_ident(const std::string &m) const;
        /// 把任意 Sinter 标识符转成生成 C 用的 snake_case（`sinter_out_int` 风格）。
        static std::string to_snake_ident(const std::string &s);

        /// 把「C 类型 + 变量名」拼成合法的 C 声明。
        ///
        /// 普通类型是 `int64_t x`；但函数指针类型的 C 语法要求把名字塞进中间：
        /// `int64_t (*)(int64_t)` + `f` → `int64_t (*f)(int64_t)`。
        /// 不做这一步会生成 `int64_t (*)(int64_t) f`，gcc 直接语法错误。
        static std::string param_decl(const std::string &type_c, const std::string &name);
        std::string type_to_c(const TypeAnnotation *t) const;
        std::string type_to_c_no_ref(const TypeAnnotation *t) const;
        std::string type_to_c(const CType &t) const;

        /// 重载解析：按实参类型选择；返回 nullptr 表示未找到
        const FunctionInfo *resolve_overload(const std::string &name,
                                             const std::vector<CType> &arg_types,
                                             std::string *err = nullptr);
        /// 不做实例分派归约的精确重载解析（内部使用）
        const FunctionInfo *resolve_overload_exact(const std::string &name,
                                                   const std::vector<CType> &arg_types,
                                                   std::string *err = nullptr) const;
        /// 实例方法体里的多态调用归约（`sinter_gen_clone` → `sinter_gen_clone_str`）
        std::string dispatch_by_instance_param(const std::string &name) const;
        /// 带模块过滤的重载解析（跨模块调用 mod.fn）
        const FunctionInfo *resolve_overload(const std::string &name,
                                             const std::vector<CType> &arg_types,
                                             std::string *err,
                                             const std::string &module_ident) const;

        /// 数字类型提升
        static CType promote(const CType &a, const CType &b);
        /// 可赋值性
        static bool is_assignable(const CType &from, const CType &to, bool strict = false);
        /// must 约束检查
        bool check_constraint(const std::string &constraint, const CType &arg) const;

        /// 类型名 → CType
        CType resolve_type(const TypeAnnotation *t) const;
        CType resolve_type_name(const std::string &name) const;

        /// vtable 槽位查询
        int vtable_slot_of(const std::string &class_name, const std::string &method) const;

        void report_error(const std::string &m);
        /// 带源位置的报错：driver 会据此渲染出源码行与 ^^^ 指示。
        void report_error(const std::string &m, int line, int col, int length = 0) {
            diags_.push_back({Severity::Error, m,
                              current_file_.empty() ? cur_module_ident_ : current_file_,
                              line, col, length});
        }
        /// 按表达式节点的位置报错（节点没有位置时退化为无位置）。
        void report_error(const std::string &m, const Expression *e) {
            if (!e || e->line <= 0) { report_error(m); return; }
            report_error(m, e->line, e->col);
        }

        /// 带稳定错误码与修复建议的报错：渲染成 `error[E0301]: ...` + `= hint: ...`。
        /// 错误码段位约定：E01xx 语法 / E02xx 声明 / E03xx 类型 / E04xx 泛型 /
        /// E05xx 编译期求值 / E06xx 代码生成 / E07xx extern C ABI。
        void report_error_ex(const std::string &code, const std::string &m,
                             const std::string &hint, int line, int col, int length = 0) {
            Diagnostic d;
            d.severity = Severity::Error;
            d.message = m;
            d.file = current_file_.empty() ? cur_module_ident_ : current_file_;
            d.line = line;
            d.col = col;
            d.length = length;
            d.code = code;
            d.hint = hint;
            d.scope = scope_text();   // 模块 › 类/结构体/接口 › 函数
            diags_.push_back(std::move(d));
        }
        void report_error_ex(const std::string &code, const std::string &m,
                             const std::string &hint, const Expression *e) {
            if (!e || e->line <= 0) { report_error_ex(code, m, hint, 0, 0); return; }
            report_error_ex(code, m, hint, e->line, e->col);
        }
        /// 设置当前正在分析的源文件真实路径，供诊断渲染源码片段。
        void set_current_file(const std::string &path) { current_file_ = path; }
        void report_warn(const std::string &m);
        /// 弱警告：不阻塞构建，默认打印；用于「能跑但很可能不是本意」的模式
        /// （如 return/break/continue 之后的不可达语句）。
        void report_weak_ex(const std::string &code, const std::string &m,
                            const std::string &hint, int line, int col);

        std::string current_module() const { return current_module_; }

    private:
        /// `when` 穷尽性检查：只对**确定是 enum** 的 subject 报告缺少的变体。
        /// 类型推不出（或分支里出现非枚举值）时一律跳过 —— 宁可漏报也不误报，
        /// 和本文件其它检查的设计原则一致。
        void check_when_exhaustive(Program &p);
        void validate_annotations(Program &p);

        std::string current_file_;   // 当前分析的源文件真实路径
        std::unordered_map<std::string, std::vector<FunctionInfo> > functions_;
        std::unordered_map<std::string, ClassInfo> classes_;
        /// 泛型实例名 → 类型实参短名（`List_str` → {"str"}）
        std::unordered_map<std::string, std::vector<std::string> > generic_args_;
        std::unordered_map<std::string, EnumInfo> enums_;
        std::unordered_map<std::string, std::string> aliases_;
        std::unordered_map<std::string, std::string> var_types_;
        std::unordered_map<std::string, FieldInfo> static_fields_; // "Class.field"
        std::unordered_map<std::string, ClassInfo> external_classes_; // 其它模块的类
        std::unordered_set<std::string> numeric_types_;
        std::unordered_set<std::string> user_functions_;
        std::vector<Diagnostic> diags_;
        bool stdlib_imported_ = false;
        std::string current_module_;
        std::string cur_module_ident_;
        int conflict_count_ = 0;
        // 诊断上下文：模块 › 类/结构体/接口 › 函数。出错时一并打印，
        // 这样同名的 `trim` 在两个类里报错也能一眼区分。
        std::string scope_kind_;   // "class" / "struct" / "interface"（空 = 模块级）
        std::string scope_type_;   // 类型名
        std::string scope_fn_;     // 函数/方法名
        /// 拼成 `module <m> › class <T> › fun <f>`
        std::string scope_text() const;
    public:
        /// 类型检查进入某个函数体时调用：让诊断带上准确的函数名。
        void set_scope_function(const std::string &n) { scope_fn_ = n; }
        /// 进入类/结构体/接口体时调用。
        void set_scope_type(const std::string &kind, const std::string &name) {
            scope_kind_ = kind;
            scope_type_ = name;
            scope_fn_.clear();
        }
    private:

        void register_builtins();
        void collect(Program &p);
        void collect_class(ClassDecl *c);
        void prescan(Program &p);
        void predeclare_class(ClassDecl *c);
        /// 接口按抽象类登记（生成只有对象头的 `struct`，接口方法进 vtable）
        void predeclare_interface(InterfaceDecl *it);
        void register_function(FunctionDecl *f, const std::string &owner_class, bool is_method);
        void build_vtables();
        void check_stmt(Statement *s);
        void check_function(FunctionDecl *f);
        void check_function_body(FunctionDecl *f);
        /// 当前正在检查的函数（用于把实例方法体定位到所属类）
        FunctionDecl *cur_fn_ = nullptr;
        void check_expr(Expression *e);
        std::string mangled_name(const std::string &name, const std::string &sig) const;
        static std::string sanitize(const std::string &t);
    };
}
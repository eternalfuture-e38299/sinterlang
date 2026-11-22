// sinter 类型检查：在生成 C 之前拒绝明显非法的程序。
//
// 设计原则（很重要）：
//   1. **保守** —— 只要有一侧类型推不出来（Unknown），就一律放行。
//      宁可漏报，也不能把合法程序判死。
//   2. **只报确定错误** —— 报错意味着「无论如何都编译不过」，
//      例如把 str 当数字做减法、把 str 赋给 i64 变量。
//   3. 诊断必须带源位置 —— 用户要看到自己那一行，而不是生成代码的行。
//
// 没有这一层时，上述错误会一路漏到 C 编译器，用户看到的是
// `sinter_main.c:9:22: 'nope' undeclared`，完全对不上自己的源码。
#pragma once

#include "front/ast.hpp"
#include "sem/semantic.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace sinter {
    /// 在 Semantic 收集完符号之后运行：遍历函数体，推断表达式类型，
    /// 对确定的类型错误报告诊断。
    class TypeChecker {
    public:
        explicit TypeChecker(Semantic &sem) : sem_(sem) {}

        /// 检查整个程序。诊断通过 Semantic::report_error 汇总。
        void check_program(Program &p);

    private:
        using Scope = std::unordered_map<std::string, CType>;
        std::vector<Scope> scopes_;

        Semantic &sem_;
        /// 当前函数的返回类型（return 语句校验用）
        CType return_type_;
        /// 当前所在的类（方法体里 `this` 的类型、字段解析用）
        const ClassInfo *current_class_ = nullptr;
        /// 防御性递归深度上限：AST 异常时避免爆栈
        int depth_ = 0;

        void push_scope() { scopes_.emplace_back(); }
        void pop_scope() { if (!scopes_.empty()) scopes_.pop_back(); }
        void declare(const std::string &name, const CType &t) {
            if (!scopes_.empty()) scopes_.back()[name] = t;
        }
        [[nodiscard]] const CType *lookup_local(const std::string &name) const;

        void stmt(Statement *s);
        void function_body(FunctionDecl *f);

        /// 推断表达式类型；推不出来返回 CType::unknown()。
        CType infer(Expression *e);
        /// 调用表达式的类型推断（构造函数 / 自由函数 / 方法）
        CType infer_call(CallExpr *c);

        /// 若 from 不能赋给 to，报告一条带位置的错误。
        /// `code` 区分场景：E0301 初始化 / E0302 赋值 / E0303 返回值。
        void require_assignable(const CType &from, const CType &to, const Expression *at,
                                const std::string &what, const std::string &code = "E0301");

        /// 类名 → ClassInfo（含外部模块）
        [[nodiscard]] const ClassInfo *find_class(const std::string &n) const {
            return sem_.find_class(n);
        }
        /// got 是否是 want 的子类（沿 base_class 向上找）
        [[nodiscard]] bool is_subclass_of(const std::string &got, const std::string &want) const;
        /// 从类型名解析 CType（`str` / `i64` / `Box_i64` …）
        [[nodiscard]] CType type_of_name(const std::string &n) const {
            return sem_.resolve_type_name(n);
        }
    };
}
/*******************************************************************************
 * File: parser
 * Project: sinter
 * Created: 2026/11/17
 * Author: eternalfuture-e38299
 * Github: https://github.com/eternalfuture-e38299
 *
 * MIT License
 *
 * Copyright (c) 2026 eternalfuture-e38299
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *******************************************************************************/

#pragma once
#include "support/token.hpp"
#include "support/diagnostics.hpp"
#include "front/ast.hpp"
#include <vector>
#include <optional>
#include <stdexcept>

namespace sinter {
    class Parser {
    public:
        explicit Parser(const std::vector<Token> &t);
        explicit Parser(std::vector<Token> &&t);

        /// 带基准位置的构造：`base_line` / `base_col` 是 `tokens` 里第一个字符
        /// 在**外层源码**中的位置（1 基）。字符串插值 `"${ ... }"` 会把片段
        /// 单独丢进一个子 Parser，片段里的 token 位置是相对于片段起点的；
        /// 不平移回去，诊断和 `__LINE__` / `__COLUMN__` 就永远指向 1:1。
        Parser(const std::vector<Token> &t, int base_line, int base_col);
        Parser(std::vector<Token> &&t, int base_line, int base_col);

        Program parse();

        /// 让语法错误带上「文件:行:列 + 源码行 + ^^^」。不设置的话退回
        /// 单行 stderr 文本（`Parser 12: ...`），只适合内部自测。
        void set_source_path(std::string p) { source_path_ = std::move(p); }
        void set_diagnostics(Diagnostics *d) { diags_ = d; }

        struct ParseError : std::runtime_error {
            using std::runtime_error::runtime_error;
        };

    private:
        // 持有值的副本：绝对禁止引用，否则 Parser(sub.tokenize()) 这类写法会悬垂。
        std::vector<Token> tokens;
        size_t current = 0;
        int line = 0, column = 0;
        /// 子解析器的位置基准（1 基）；0 表示「token 位置就是绝对位置」。
        int base_line_ = 0, base_col_ = 0;
        bool in_pub_block = false;
        bool in_class_body = false;
        bool no_raii_context = false;
        /// 条件表达式里禁止尾随 lambda：`if v < target {` 的 `{` 是语句块。
        int suppress_trailing_lambda_ = 0;
        /// 语法错误落到哪：为空时退回 stderr；设置后走统一诊断渲染。
        std::string source_path_;
        Diagnostics *diags_ = nullptr;

        [[nodiscard]] Token peek() const;
        [[nodiscard]] Token peek_next() const;
        [[nodiscard]] Token peek_at(size_t n) const;
        [[nodiscard]] Token previous() const;
        /// 子解析器（字符串插值）的相对位置 → 外层源码的绝对位置。
        [[nodiscard]] Token shift_position(Token t) const;
        [[nodiscard]] bool check(TokenType t) const;
        [[nodiscard]] bool is_at_end() const;

        Token advance();
        bool match(TokenType t);
        bool match_any(std::initializer_list<TokenType> ts);
        Token consume(TokenType t, const std::string &msg);
        [[noreturn]] void error(const std::string &msg) const;

        void consume_newlines();
        void skip_all_newlines_and_semis();
        void expect_statement_end(const std::string &msg);
        void parse_annotations(std::vector<Annotation> &into);

        // 声明
        StmtPtr parse_declaration();
        /// parse_declaration 的实现体；外层包装负责补源位置。
        StmtPtr parse_declaration_impl();
        StmtPtr parse_declaration_with_modifiers(std::vector<Annotation> anns,
                                                 bool is_pub, bool is_inline, bool is_static);
        StmtPtr parse_module();
        StmtPtr parse_import();
        StmtPtr parse_function(std::vector<Annotation> anns, bool is_pub, bool is_inline, bool is_static);
        StmtPtr parse_class_like(std::vector<Annotation> anns, bool is_struct, bool is_pub, bool is_data = false);
        /// data class/struct：在解析完类体后追加 equals/hashCode/toString/copy/componentN。
        void add_data_methods(ClassDecl &c);
        StmtPtr parse_interface(std::vector<Annotation> anns, bool is_pub);
        StmtPtr parse_enum(std::vector<Annotation> anns, bool is_pub);
        StmtPtr parse_const();
        StmtPtr parse_var_decl(bool is_const, bool is_static, bool is_pub);
        StmtPtr parse_destructure(bool is_mutable);
        StmtPtr parse_alias(bool is_pub);
        StmtPtr parse_comptime(std::vector<Annotation> anns);
        StmtPtr parse_bind_c();
        StmtPtr parse_embed_c(std::vector<Annotation> anns);
        StmtPtr parse_pub_block(std::vector<Annotation> anns);
        StmtPtr parse_no_raii_block();

        // 语句
        StmtPtr parse_return();
        StmtPtr parse_if_stmt();
        StmtPtr parse_try();
        StmtPtr parse_throw();
        StmtPtr parse_while();
        StmtPtr parse_do_while();
        StmtPtr parse_for();
        StmtPtr parse_break();
        StmtPtr parse_continue();
        StmtPtr parse_del();
        StmtPtr parse_block();
        StmtPtr parse_expr_stmt();
        StmtPtr parse_destructor();
        ExprPtr parse_block_lambda();

        // 表达式
        ExprPtr parse_expression();
        ExprPtr parse_assignment();
        ExprPtr parse_pipeline();
        ExprPtr parse_ternary();
        ExprPtr parse_range();
        ExprPtr parse_logical_or();
        ExprPtr parse_logical_and();
        ExprPtr parse_equality();
        ExprPtr parse_comparison();
        ExprPtr parse_term();
        ExprPtr parse_factor();
        ExprPtr parse_unary();
        ExprPtr parse_call();
        ExprPtr parse_primary();
        /// parse_primary 的实现体；外层包装负责补源位置。
        ExprPtr parse_primary_impl();
        ExprPtr parse_lambda();
        ExprPtr parse_if_expr();
        ExprPtr parse_when_expr();
        ExprPtr parse_string_template(const std::string &raw, const Token &literal);
        ExprPtr parse_scope_call(ExprPtr receiver);

        // 类型
        std::unique_ptr<TypeAnnotation> parse_type();
        std::unique_ptr<TypeAnnotation> parse_basic_type();
        std::vector<Parameter> parse_parameters();
        std::vector<std::unique_ptr<TypeAnnotation> > parse_type_args();
        /// 运算符重载：`operator+(other: T): T { ... }`
        StmtPtr parse_operator(std::vector<Annotation> anns, bool is_pub);

        // 可变参数：`fun f(xs: i64...)` / `fun f(xs: T...)`。
        // 不引入 va_list，调用点把实参逐个绑定为同类型局部变量后内联展开。
        bool variadic_flag_ = false;
        std::string variadic_param_name_;
        void apply_variadic(FunctionDecl &f);

        /// 校验泛型参数表：类型参数包只能出现在最后一位，且不能有多个。
        void validate_tparams(const std::vector<TemplateParam> &tps);

        /// 当前正在解析的函数/类的类型参数包名（用于识别 `Args[i]`），
        /// 为空表示不在泛型作用域内，`T[N]` 一律按数组处理。
        std::vector<std::string> active_pack_names_;
        [[nodiscard]] bool is_active_pack(const std::string &n) const;

        /// 解析结束时把 `active_pack_names_` 回退到指定长度
        struct PackScopeGuard {
            std::vector<std::string> *v = nullptr;
            size_t n = 0;
            PackScopeGuard() = default;
            PackScopeGuard(std::vector<std::string> &vec, size_t len) : v(&vec), n(len) {}
            PackScopeGuard(PackScopeGuard &&o) noexcept : v(o.v), n(o.n) { o.v = nullptr; }
            PackScopeGuard &operator=(const PackScopeGuard &) = delete;
            ~PackScopeGuard() { if (v) v->resize(n); }
        };
        std::optional<PackScopeGuard> pack_scope_;

        bool looks_like_type_arg_list();
        bool looks_like_ctor_call();
        bool is_explicit_alias_form();
        static bool is_identifier_expr(Expression *e);
        bool looks_like_destructure();
    };
}
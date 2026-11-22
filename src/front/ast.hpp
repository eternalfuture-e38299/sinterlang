/*******************************************************************************
 * File: ast
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
#include <memory>
#include <vector>
#include <string>
#include <variant>

namespace sinter {
    struct Expression;
    struct Statement;
    using ExprPtr = std::unique_ptr<Expression>;
    using StmtPtr = std::unique_ptr<Statement>;

    struct TypeAnnotation;

    inline std::unique_ptr<TypeAnnotation> clone_type(const TypeAnnotation *);
    inline ExprPtr clone_expr(const Expression *);

    // ==========================================================================
    // 注解 @no_raii / @inline / @pub ...
    // ==========================================================================
    struct Annotation {
        std::string name;
        std::vector<ExprPtr> args;

        Annotation() = default;
        Annotation(const Annotation &o) : name(o.name) {
            for (auto &a: o.args) args.push_back(clone_expr(a.get()));
        }
        Annotation &operator=(const Annotation &o) {
            if (this != &o) {
                name = o.name;
                args.clear();
                for (auto &a: o.args) args.push_back(clone_expr(a.get()));
            }
            return *this;
        }
        Annotation(Annotation &&) = default;
        Annotation &operator=(Annotation &&) = default;
    };

    // ==========================================================================
    // 类型
    // ==========================================================================
    struct TypeAnnotation {
        enum Kind { Named, Pointer, Array, Generic, Optional, FunctionType };

        Kind kind = Named;
        std::string name;
        std::unique_ptr<TypeAnnotation> inner_type;
        std::vector<std::unique_ptr<TypeAnnotation> > generic_args;
        /// Kotlin 接收者函数类型：`Receiver.(args) -> ret` 的 receiver。
        /// 生成 C 时 receiver 作为函数指针的第一个参数。
        std::unique_ptr<TypeAnnotation> receiver_type;
        int array_size = 0;

        /// 类型参数包索引：`Args[0]` 记 name="Args"、pack_index=0；
        /// `-1` 表示不是包索引（普通命名类型 / 数组）。
        /// 与 `T[N]` 数组的区别：仅当 name 是当前作用域的模板类型参数包名时才置位。
        int pack_index = -1;

        // 语义补充
        bool is_const_ref = false;   // const T&
        bool is_ref = false;         // T&
        bool is_rvalue_ref = false;  // T&&

        TypeAnnotation() = default;

        explicit TypeAnnotation(std::string n) : name(std::move(n)) {}

        TypeAnnotation(const TypeAnnotation &) = delete;
        TypeAnnotation &operator=(const TypeAnnotation &) = delete;
        TypeAnnotation(TypeAnnotation &&) = default;
        TypeAnnotation &operator=(TypeAnnotation &&) = default;

        [[nodiscard]] bool is_number_name() const {
            if (kind != Named) return false;
            static const char *kNums[] = {
                "number", "i8", "i16", "i32", "i64", "i128",
                "u8", "u16", "u32", "u64", "u128",
                "f32", "f64", "int", "long", "short", "byte", "float", "double"
            };
            for (auto *n: kNums) if (name == n) return true;
            return false;
        }
    };

    // ==========================================================================
    // 表达式
    // ==========================================================================
    struct Expression {
        enum Kind {
            Literal, Identifier, Binary, Unary, NonNull, Call, MemberAccess,
            IndexAccess, Assignment, Pipeline, Lambda, Ternary,
            Range, Cast, Grouping, ArrayLiteral, IfExpr, WhenExpr,
            BlockExprKind, StringTemplate, ScopeCall, Builtin,
        };

        Kind kind;
        // 源位置（1 基）。0 表示未知 —— 诊断里按「无位置」处理。
        // parser 在建立节点时填入，类型检查与后续诊断都用它定位到源码行。
        int line = 0;
        int col = 0;

        virtual ~Expression() = default;
    };

    struct LiteralExpr : Expression {
        enum Lit { Int, Float, String, Char, Bool, Null };

        Lit lit;
        std::variant<int64_t, double, std::string, char, bool> value;
        std::string raw;
        std::string suffix; // i32 / u8 / f32 ...

        LiteralExpr(const Lit l, const std::string &r) : lit(l), raw(r) {
            kind = Literal;
            try {
                switch (l) {
                    case Int: value = static_cast<int64_t>(std::stoll(r)); break;
                    case Float: value = std::stod(r); break;
                    case String: value = r; break;
                    case Char: value = r.size() >= 3 ? r[1] : '\0'; break;
                    case Bool: value = (r == "true"); break;
                    case Null: value = 0; break;
                }
            } catch (...) {
                switch (l) {
                    case Float: value = 0.0; break;
                    case Int: value = 0LL; break;
                    default: value = 0LL; break;
                }
            }
        }
    };

    struct IdentifierExpr : Expression {
        std::string name;
        explicit IdentifierExpr(std::string n) : name(std::move(n)) { kind = Identifier; }
    };

    /// 编译期位置关键字（`__FILE__` / `__LINE__` / `__CLASS__` …）。
    ///
    /// 解析阶段只记下「是哪一个」和它自己的行列；真正的值由 comptime pass 填充：
    ///   - 文件 / 模块 / 行 / 列：从编译单元与节点位置直接得到；
    ///   - 函数 / 类 / 结构体：从遍历时的声明上下文栈得到。
    /// 因此这个节点绝不该活到语义分析——fold 完之后 AST 里只剩字面量。
    struct BuiltinExpr : Expression {
        enum Which {
            File,      ///< __FILE__    当前源文件（编译器看到的路径）
            Module,    ///< __MODULE__  当前模块名（`module foo`）
            Line,      ///< __LINE__    关键字所在行（1 基）
            Column,    ///< __COLUMN__  关键字所在列（1 基）
            Function,  ///< __FUNCTION__  最近的外层函数 / 方法名
            Class,     ///< __CLASS__   当前 class 名（struct 体内报错）
            Struct,    ///< __STRUCT__  当前 struct 名（class 体内报错）
        };

        Which which;
        explicit BuiltinExpr(const Which w) : which(w) { kind = Builtin; }

        /// 关键字的拼写，用于诊断与代码生成的自检信息。
        [[nodiscard]] static const char *spelling(const Which w) {
            switch (w) {
                case File: return "__FILE__";
                case Module: return "__MODULE__";
                case Line: return "__LINE__";
                case Column: return "__COLUMN__";
                case Function: return "__FUNCTION__";
                case Class: return "__CLASS__";
                case Struct: return "__STRUCT__";
            }
            return "__FILE__";
        }
    };

    struct BinaryExpr : Expression {
        TokenType op;
        ExprPtr left, right;

        BinaryExpr(const TokenType o, ExprPtr l, ExprPtr r)
            : op(o), left(std::move(l)), right(std::move(r)) { kind = Binary; }
    };

    struct UnaryExpr : Expression {
        TokenType op;
        ExprPtr operand;
        UnaryExpr(const TokenType o, ExprPtr x) : op(o), operand(std::move(x)) { kind = Unary; }
    };

    /// `expr!!`：Kotlin 的非空断言。空指针会在运行期 abort。
    struct NonNullExpr : Expression {
        ExprPtr operand;
        explicit NonNullExpr(ExprPtr x) : operand(std::move(x)) { kind = NonNull; }
    };

    /// 命名实参：f(name = value)
    struct Argument {
        std::string name; // 空表示位置参数
        ExprPtr value;

        Argument() = default;
        Argument(std::string n, ExprPtr v) : name(std::move(n)), value(std::move(v)) {}
        Argument(const Argument &o) : name(o.name), value(clone_expr(o.value.get())) {}
        Argument &operator=(const Argument &o) {
            if (this != &o) { name = o.name; value = clone_expr(o.value.get()); }
            return *this;
        }
        Argument(Argument &&) = default;
        Argument &operator=(Argument &&) = default;
    };

    struct CallExpr : Expression {
        ExprPtr callee;
        std::vector<ExprPtr> arguments;
        std::vector<Argument> args;          // 命名参数支持
        std::vector<std::unique_ptr<TypeAnnotation> > type_args; // f<i32>(x)

        CallExpr(ExprPtr c, std::vector<ExprPtr> a)
            : callee(std::move(c)), arguments(std::move(a)) { kind = Call; }
        CallExpr() { kind = Call; }
    };

    struct MemberAccessExpr : Expression {
        ExprPtr object;
        std::string member;
        bool null_safe = false; // obj?.member
        MemberAccessExpr(ExprPtr o, std::string m)
            : object(std::move(o)), member(std::move(m)) { kind = MemberAccess; }
    };

    struct IndexAccessExpr : Expression {
        ExprPtr object, index;
        IndexAccessExpr(ExprPtr o, ExprPtr i)
            : object(std::move(o)), index(std::move(i)) { kind = IndexAccess; }
    };

    struct AssignmentExpr : Expression {
        ExprPtr target, value;
        TokenType op = TokenType::Equal;
        AssignmentExpr(ExprPtr t, ExprPtr v)
            : target(std::move(t)), value(std::move(v)) { kind = Assignment; }
    };

    struct Parameter {
        std::string name;
        std::unique_ptr<TypeAnnotation> type;
        ExprPtr default_value;
        bool is_val = false;   // struct/class 构造参数：只读成员
        bool is_var = false;   // 可写成员
        bool is_self = false;  // 方法隐式参数

        Parameter() = default;

        Parameter(const Parameter &o)
            : name(o.name), type(clone_type(o.type.get())),
              default_value(clone_expr(o.default_value.get())),
              is_val(o.is_val), is_var(o.is_var), is_self(o.is_self) {}

        Parameter &operator=(const Parameter &o) {
            if (this != &o) {
                name = o.name;
                type = clone_type(o.type.get());
                default_value = clone_expr(o.default_value.get());
                is_val = o.is_val; is_var = o.is_var; is_self = o.is_self;
            }
            return *this;
        }

        Parameter(Parameter &&) = default;
        Parameter &operator=(Parameter &&) = default;
    };

    struct LambdaExpr : Expression {
        std::vector<Parameter> params;
        std::variant<ExprPtr, StmtPtr> body;
        bool is_block = false;
        bool impure_capture = false; // 是否捕获外部变量
        LambdaExpr() { kind = Lambda; }
    };

    struct TernaryExpr : Expression {
        ExprPtr cond, then_b, else_b;
        bool is_elvis = false; // a ?: b

        TernaryExpr(ExprPtr c, ExprPtr t, ExprPtr e)
            : cond(std::move(c)), then_b(std::move(t)), else_b(std::move(e)) { kind = Ternary; }
    };

    struct RangeExpr : Expression {
        ExprPtr start, end;
        bool inclusive = true; // a..b 闭，a..<b 半开

        RangeExpr(ExprPtr s, ExprPtr e, const bool inc)
            : start(std::move(s)), end(std::move(e)), inclusive(inc) { kind = Range; }
    };

    struct CastExpr : Expression {
        ExprPtr expr;
        std::unique_ptr<TypeAnnotation> target;

        CastExpr(ExprPtr e, std::unique_ptr<TypeAnnotation> t)
            : expr(std::move(e)), target(std::move(t)) { kind = Cast; }
    };

    struct GroupingExpr : Expression {
        ExprPtr expr;
        explicit GroupingExpr(ExprPtr e) : expr(std::move(e)) { kind = Grouping; }
    };

    struct IfExpr : Expression {
        ExprPtr cond, then_b, else_b;

        IfExpr(ExprPtr c, ExprPtr t, ExprPtr e)
            : cond(std::move(c)), then_b(std::move(t)), else_b(std::move(e)) {
            kind = Kind::IfExpr;
        }
    };

    struct WhenExpr : Expression {
        ExprPtr subject;

        struct Pattern {
            enum Mode { Value, Range, TypeIs, Expr } mode = Value;
            ExprPtr value;      // Value / Expr
            ExprPtr lo, hi;     // Range
            std::string type_name; // TypeIs
            bool inclusive = true;
        };

        struct Branch {
            std::vector<Pattern> patterns;
            ExprPtr body;
            StmtPtr body_block;
        };

        std::vector<Branch> branches;
        ExprPtr else_branch;
        StmtPtr else_block;
        WhenExpr() { kind = Kind::WhenExpr; }
    };

    /// 管道： lhs |> f(a) / lhs |> f / lhs |> { it }
    struct PipelineExpr : Expression {
        ExprPtr left;
        ExprPtr right;   // 右侧表达式（Call / Identifier / MemberAccess / Lambda）
        PipelineExpr(ExprPtr l, ExprPtr r)
            : left(std::move(l)), right(std::move(r)) { kind = Pipeline; }
    };

    struct ArrayLiteralExpr : Expression {
        std::vector<ExprPtr> elements;
        ArrayLiteralExpr() { kind = ArrayLiteral; }
    };

    /// 字符串模板 "a${b}c"
    struct StringTemplateExpr : Expression {
        struct Part {
            std::string text;   // 非空则为字面文本
            ExprPtr expr;       // 非空则为插值表达式
            Part(std::string t) : text(std::move(t)) {}
            explicit Part(ExprPtr e) : expr(std::move(e)) {}
            Part(const Part &o) : text(o.text), expr(clone_expr(o.expr.get())) {}
            Part(Part &&) = default;
        };

        std::vector<Part> parts;
        StringTemplateExpr() { kind = StringTemplate; }
    };

    /// 作用域函数 obj.let { it -> ... }, obj.run { }, obj.apply{}, obj.also{}
    struct ScopeCallExpr : Expression {
        std::string member_name;   // with / let / run / apply / also
        enum KindSc { Let, Run, Apply, Also, With };

        KindSc kind_sc = Let;
        ExprPtr receiver;
        StmtPtr body;          // 块体
        ExprPtr body_expr;     // 单个表达式
        std::string it_name = "it";
        ScopeCallExpr() { kind = ScopeCall; }
    };

    struct BlockExpr : Expression {
        StmtPtr block;      // 语句块（最后一条表达式为值）
        ExprPtr result;     // 若只有一条表达式
        bool is_block = false;
        BlockExpr() { kind = BlockExprKind; }
    };

    // ==========================================================================
    // 语句
    // ==========================================================================
    struct Statement {
        enum Kind {
            Block, VarDecl, Destructure, ExprStmt, Return, If, While, For, DoWhile,
            Break, Continue, Function, Class, Struct, Interface, Enum,
            Import, Module, Alias, Comptime, BindCFunction, BindCHeader, CStructDecl, EmbedC, Del, PubBlock,
            ComptimeAssert, Try, Throw,
        };

        Kind kind;
        std::vector<sinter::Annotation> annotations;
        // 源位置（1 基）。0 表示未知。
        int line = 0;
        int col = 0;

        virtual ~Statement() = default;
    };

    struct BlockStmt : Statement {
        std::vector<StmtPtr> statements;
        bool no_raii = false; // no_raii { ... }
        BlockStmt() { kind = Block; }
    };

    struct VarDeclStmt : Statement {
        std::string name;
        std::unique_ptr<TypeAnnotation> type;
        ExprPtr init;
        bool is_mutable = false, is_const = false;
        bool is_static = false;
        bool is_pub = false;
        bool is_raw_c = false;  // embed C 之类
        VarDeclStmt() { kind = VarDecl; }
    };

    /// 解构：val (a, b) = pair / val Point(x, y) = p
    struct DestructureStmt : Statement {
        struct Target {
            enum Mode { Val, Var, Ignore } mode = Val;
            std::string name;
            std::unique_ptr<TypeAnnotation> type;
        };

        std::vector<Target> targets;
        std::string pattern_type; // 点名前缀：Point(x,y)
        ExprPtr init;
        bool is_mutable = false;

        DestructureStmt() { kind = Destructure; }
    };

    struct ExprStmt : Statement {
        ExprPtr expr;
        ExprStmt() { kind = Kind::ExprStmt; }
    };

    struct ReturnStmt : Statement {
        ExprPtr value;
        ReturnStmt() { kind = Return; }
    };

    /// `throw expr`：longjmp 到最近的 try/catch。
    struct ThrowStmt : Statement {
        ExprPtr value;
        ThrowStmt() { kind = Throw; }
    };

    /// `try { } catch (e: str) { } [finally { }]`
    struct TryStmt : Statement {
        StmtPtr body;
        std::string error_name;
        StmtPtr catch_body;
        StmtPtr finally_body;
        TryStmt() { kind = Try; }
    };

    struct IfStmt : Statement {
        ExprPtr cond;
        StmtPtr then_b, else_b;
        IfStmt() { kind = If; }
    };

    struct WhileStmt : Statement {
        ExprPtr cond;
        StmtPtr body;
        WhileStmt() { kind = While; }
    };

    struct ForStmt : Statement {
        std::vector<std::string> variables; // for ((k, v) in x)
        ExprPtr iterable;
        StmtPtr body;
        ForStmt() { kind = For; }
    };

    struct DoWhileStmt : Statement {
        StmtPtr body;
        ExprPtr cond;
        DoWhileStmt() { kind = DoWhile; }
    };

    struct BreakStmt : Statement {
        BreakStmt() { kind = Break; }
    };

    struct ContinueStmt : Statement {
        ContinueStmt() { kind = Continue; }
    };

    /// 泛型参数：`<T>` / `<T must number>` / `<Args...>`（函数、方法、类、结构体共用）
    struct TemplateParam {
        std::string name;
        std::unique_ptr<TypeAnnotation> constraint;   // must number / must any ...
        /// 变长类型参数包：`<T, Args...>`。只能是参数表最后一个。
        /// 实例化时按「从该位置起剩余的类型实参」依次绑定成
        /// `Args#0` / `Args#1` …，总数记作 `Args#count`。
        bool is_pack = false;

        TemplateParam() = default;
        TemplateParam(const TemplateParam &o) : name(o.name), is_pack(o.is_pack) {
            if (o.constraint) constraint = clone_type(o.constraint.get());
        }
        TemplateParam &operator=(const TemplateParam &o) {
            if (this != &o) {
                name = o.name;
                is_pack = o.is_pack;
                constraint = o.constraint ? clone_type(o.constraint.get()) : nullptr;
            }
            return *this;
        }
        TemplateParam(TemplateParam &&) = default;
        TemplateParam &operator=(TemplateParam &&) = default;
    };

    struct FunctionDecl : Statement {
        std::string name;
        std::vector<std::unique_ptr<TypeAnnotation> > template_params;

        using TemplateParam = sinter::TemplateParam;
        std::vector<TemplateParam> tparams;

        std::vector<Parameter> params;
        std::unique_ptr<TypeAnnotation> return_type;
        StmtPtr body;

        /// 可变参数：`fun f(xs: i64...)` 里的 `xs` 收下全部剩余实参。
        /// 实现方式是在每个调用点把实参逐个绑定成同类型局部变量，
        /// 再以「生成的数组变量」作为该参数的值传入 —— 这样不引入
        /// 任何运行期 ABI（没有 va_list），生成代码是纯 C11。
        bool is_variadic = false;
        /// 可变参数的名字与元素类型（从 params.back() 复制而来）
        std::string variadic_name;
        std::unique_ptr<TypeAnnotation> variadic_elem_type;

        /// 运算符重载：`operator+(other: T)`。name 记作 `operator+`，
        /// 后端据此生成 `T_op_add` 之类的 C 函数名，并在二元运算处改派。
        bool is_operator = false;

        bool is_override = false;
        bool is_pub = false;
        bool is_inline = false;
        bool is_static = false;
        bool is_virtual = false;
        bool is_abstract = false;
        bool is_default = false;   // = default

        /// `extern fun name(...)`：实现 C 头里已经声明的函数。
        ///
        /// 三个语义差别（其余全部复用普通函数路径）：
        ///   1. 生成的 C 符号名就是 `name`，不叠加模块前缀与签名（等同 `[name]`）；
        ///   2. 不写进模块头文件——声明由 C 头负责，重复声明会与 `int`/`int32_t`
        ///      之类的平台 typedef 打架；
        ///   3. 参数 / 返回类型必须是 C ABI 类型（`str` 会被拒绝，用 `cstr`）。
        bool is_extern_c = false;

        // 函数别名：fun f2 = f1，或多分支返回类型
        std::vector<std::string> aliases;

        // fun square[square]：方括号内是 Sinter 别名，可另写 square(...) 调用；
        // 生成的 C 名由编译器决定（模块 + 签名 + 别名片段）。
        std::string alias_name;

        // 扩展函数：ext fun T.name()
        std::string ext_receiver;

        bool no_raii = false;

        FunctionDecl() { kind = Function; }
    };

    struct ClassDecl : Statement {
        std::string name;
        /// 泛型类/结构体：class Box<T>(...) / struct Pair<A, B>(...)
        std::vector<sinter::TemplateParam> tparams;
        std::vector<Parameter> ctor_params;
        std::vector<std::string> bases;        // 基类 + 接口
        std::vector<std::vector<ExprPtr> > base_args; // super(...) per base

        std::vector<StmtPtr> members;          // 方法 / 字段 / 静态
        StmtPtr destructor;                    // ~T()

        bool is_pub = false;
        bool is_abstract = false;
        bool is_final = false;
        bool is_struct = false;                // 使用 struct 关键字，但支持方法/继承
        bool is_data = false;                  // data class / data struct
        bool is_annotation = false;            // annotation class / annotation struct

        // 复制/移动构造
        std::unique_ptr<FunctionDecl> copy_ctor;
        std::unique_ptr<FunctionDecl> move_ctor;

        ClassDecl() { kind = Class; }
    };

    struct StructDecl : Statement {
        std::string name;
        std::vector<Parameter> fields;
        std::vector<StmtPtr> methods;
        std::string base_class;
        bool is_pub = false;
        StructDecl() { kind = Struct; }
    };

    struct InterfaceDecl : Statement {
        std::string name;
        std::vector<StmtPtr> methods;
        std::vector<std::string> bases;
        bool is_pub = false;
        InterfaceDecl() { kind = Interface; }
    };

    struct EnumDecl : Statement {
        std::string name;
        std::vector<std::string> variants;
        std::vector<std::string> explicit_vals;
        bool is_pub = false;
        EnumDecl() { kind = Enum; }
    };

    struct ImportStmt : Statement {
        std::string path;
        ImportStmt() { kind = Import; }
    };

    struct ModuleStmt : Statement {
        std::string name;
        std::vector<StmtPtr> body;   // module foo { ... } 块式模块
        bool is_block = false;
        ModuleStmt() { kind = Module; }
    };

    struct AliasStmt : Statement {
        std::string name;
        std::unique_ptr<TypeAnnotation> type;
        bool is_pub = false;
        AliasStmt() { kind = Alias; }
    };

    struct ComptimeStmt : Statement {
        StmtPtr body;
        ComptimeStmt() { kind = Comptime; }
    };

    /// `comptime assert(cond, "msg")`：静态断言。
    ///
    /// 条件必须是编译期常量表达式（字面量、Target.*、源位置关键字，以及它们
    /// 之上的算术 / 比较 / 逻辑）。条件为假时在**编译期**产出诊断，条件为真时
    /// 整条语句在 `apply_comptime` 里被删除，不生成任何 C。
    struct ComptimeAssertStmt : Statement {
        ExprPtr cond;
        ExprPtr message;   // 可选，编译期求值的字符串
        ComptimeAssertStmt() { kind = ComptimeAssert; }
    };

    struct BindCFunctionStmt : Statement {
        std::string c_name;
        std::vector<Parameter> params;
        std::unique_ptr<TypeAnnotation> return_type;
        bool variadic = false;
        BindCFunctionStmt() { kind = BindCFunction; }
    };

    /// `bind C header "path.h"`：编译期用 C 前端解析头文件，把里面的函数原型
    /// 登记成 bind C 声明（Sinter 侧直接按名字调用），并让生成的 C 自动 #include。
    struct CBindHeaderStmt : Statement {
        std::string header;    // 原样写在语法里的路径（生成 #include 用）
        std::string resolved;  // 实际解析到的文件路径（诊断用）
        CBindHeaderStmt() { kind = BindCHeader; }
    };

    /// `bind C struct X`：不透明 C 结构体。只登记名字、不生成定义，
    /// 只能通过指针传递（C 侧布局未知，按值传递会报错）。
    struct CStructDeclStmt : Statement {
        std::string name;      // sinter 侧名字（`Foo` / `FILE`）
        std::string c_name;    // C 侧拼写（`struct Foo` / `FILE`），生成 C 用
        CStructDeclStmt() { kind = CStructDecl; }
    };

    struct EmbedCStmt : Statement {
        std::string code;
        // `embed C header { ... }`：这段 C 除了写进模块的 .c，还公开到 .h。
        // 其它编译单元能看见它，适合放 `static inline` 的共享助手。
        bool in_header = false;
        EmbedCStmt() { kind = EmbedC; }
    };

    struct DelStmt : Statement {
        ExprPtr target;
        DelStmt() { kind = Del; }
    };

    struct PubBlockStmt : Statement {
        std::vector<StmtPtr> statements;
        PubBlockStmt() { kind = PubBlock; }
    };

    struct Program {
        std::vector<StmtPtr> statements;
    };

    // ==========================================================================
    // 拷贝工具
    // ==========================================================================
    inline std::unique_ptr<TypeAnnotation> clone_type(const TypeAnnotation *t) {
        if (!t) return nullptr;
        auto c = std::make_unique<TypeAnnotation>();
        c->kind = t->kind;
        c->name = t->name;
        c->array_size = t->array_size;
        c->pack_index = t->pack_index;
        c->is_ref = t->is_ref;
        c->is_const_ref = t->is_const_ref;
        c->is_rvalue_ref = t->is_rvalue_ref;
        if (t->inner_type) c->inner_type = clone_type(t->inner_type.get());
        if (t->receiver_type) c->receiver_type = clone_type(t->receiver_type.get());
        for (auto &g: t->generic_args) c->generic_args.push_back(clone_type(g.get()));
        return c;
    }

    inline ExprPtr clone_expr_impl(const Expression *e) {
        if (!e) return nullptr;
        switch (e->kind) {
            case Expression::Literal: {
                auto *l = dynamic_cast<const LiteralExpr *>(e);
                auto c = std::make_unique<LiteralExpr>(l->lit, l->raw);
                c->value = l->value;
                c->suffix = l->suffix;
                return c;
            }
            case Expression::Identifier: {
                auto *i = dynamic_cast<const IdentifierExpr *>(e);
                return std::make_unique<IdentifierExpr>(i->name);
            }
            case Expression::Builtin: {
                auto *b = dynamic_cast<const BuiltinExpr *>(e);
                return std::make_unique<BuiltinExpr>(b->which);
            }
            case Expression::Binary: {
                auto *b = dynamic_cast<const BinaryExpr *>(e);
                return std::make_unique<BinaryExpr>(b->op, clone_expr(b->left.get()), clone_expr(b->right.get()));
            }
            case Expression::Unary: {
                auto *u = dynamic_cast<const UnaryExpr *>(e);
                return std::make_unique<UnaryExpr>(u->op, clone_expr(u->operand.get()));
            }
            case Expression::NonNull: {
                auto *n = dynamic_cast<const NonNullExpr *>(e);
                return std::make_unique<NonNullExpr>(clone_expr(n->operand.get()));
            }
            case Expression::Call: {
                auto *c = dynamic_cast<const CallExpr *>(e);
                auto n = std::make_unique<CallExpr>();
                n->callee = clone_expr(c->callee.get());
                for (auto &a: c->arguments) n->arguments.push_back(clone_expr(a.get()));
                for (auto &a: c->args) n->args.push_back(a);
                for (auto &t: c->type_args) n->type_args.push_back(clone_type(t.get()));
                return n;
            }
            case Expression::MemberAccess: {
                auto *m = dynamic_cast<const MemberAccessExpr *>(e);
                auto n = std::make_unique<MemberAccessExpr>(clone_expr(m->object.get()), m->member);
                n->null_safe = m->null_safe;
                return n;
            }
            case Expression::IndexAccess: {
                auto *i = dynamic_cast<const IndexAccessExpr *>(e);
                return std::make_unique<IndexAccessExpr>(clone_expr(i->object.get()), clone_expr(i->index.get()));
            }
            case Expression::Assignment: {
                auto *a = dynamic_cast<const AssignmentExpr *>(e);
                auto n = std::make_unique<AssignmentExpr>(clone_expr(a->target.get()), clone_expr(a->value.get()));
                n->op = a->op;
                return n;
            }
            case Expression::Lambda: {
                auto *l = dynamic_cast<const LambdaExpr *>(e);
                auto c = std::make_unique<LambdaExpr>();
                c->params = l->params;
                c->is_block = l->is_block;
                if (!l->is_block) {
                    if (auto *ex = std::get_if<ExprPtr>(&l->body)) c->body = clone_expr(ex->get());
                }
                return c;
            }
            case Expression::Ternary: {
                auto *t = dynamic_cast<const TernaryExpr *>(e);
                auto n = std::make_unique<TernaryExpr>(clone_expr(t->cond.get()), clone_expr(t->then_b.get()),
                                                       clone_expr(t->else_b.get()));
                n->is_elvis = t->is_elvis;
                return n;
            }
            case Expression::Range: {
                auto *r = dynamic_cast<const RangeExpr *>(e);
                return std::make_unique<RangeExpr>(clone_expr(r->start.get()), clone_expr(r->end.get()), r->inclusive);
            }
            case Expression::Cast: {
                auto *c = dynamic_cast<const CastExpr *>(e);
                return std::make_unique<CastExpr>(clone_expr(c->expr.get()), clone_type(c->target.get()));
            }
            case Expression::Grouping: {
                auto *g = dynamic_cast<const GroupingExpr *>(e);
                return std::make_unique<GroupingExpr>(clone_expr(g->expr.get()));
            }
            case Expression::IfExpr: {
                auto *i = dynamic_cast<const IfExpr *>(e);
                return std::make_unique<IfExpr>(clone_expr(i->cond.get()), clone_expr(i->then_b.get()),
                                                clone_expr(i->else_b.get()));
            }
            case Expression::WhenExpr: {
                auto *w = dynamic_cast<const WhenExpr *>(e);
                auto c = std::make_unique<WhenExpr>();
                c->subject = clone_expr(w->subject.get());
                for (const auto &br: w->branches) {
                    WhenExpr::Branch nb;
                    for (const auto &pt: br.patterns) {
                        WhenExpr::Pattern np;
                        np.mode = pt.mode;
                        np.value = clone_expr(pt.value.get());
                        np.lo = clone_expr(pt.lo.get());
                        np.hi = clone_expr(pt.hi.get());
                        np.type_name = pt.type_name;
                        np.inclusive = pt.inclusive;
                        nb.patterns.push_back(std::move(np));
                    }
                    nb.body = clone_expr(br.body.get());
                    c->branches.push_back(std::move(nb));
                }
                c->else_branch = clone_expr(w->else_branch.get());
                return c;
            }
            case Expression::ArrayLiteral: {
                auto *a = dynamic_cast<const ArrayLiteralExpr *>(e);
                auto c = std::make_unique<ArrayLiteralExpr>();
                for (auto &x: a->elements) c->elements.push_back(clone_expr(x.get()));
                return c;
            }
            case Expression::Pipeline: {
                auto *p = dynamic_cast<const PipelineExpr *>(e);
                return std::make_unique<PipelineExpr>(clone_expr(p->left.get()), clone_expr(p->right.get()));
            }
            case Expression::StringTemplate: {
                auto *s = dynamic_cast<const StringTemplateExpr *>(e);
                auto c = std::make_unique<StringTemplateExpr>();
                for (auto &pt: s->parts) c->parts.push_back(StringTemplateExpr::Part(pt));
                return c;
            }
            case Expression::ScopeCall: {
                auto *s = dynamic_cast<const ScopeCallExpr *>(e);
                auto c = std::make_unique<ScopeCallExpr>();
                c->kind_sc = s->kind_sc;
                c->receiver = clone_expr(s->receiver.get());
                c->body_expr = clone_expr(s->body_expr.get());
                c->it_name = s->it_name;
                return c;
            }
            case Expression::BlockExprKind: {
                auto *b = dynamic_cast<const BlockExpr *>(e);
                auto c = std::make_unique<BlockExpr>();
                c->result = clone_expr(b->result.get());
                c->is_block = b->is_block;
                return c;
            }
            default: return nullptr;
        }
    }

    /// 复制表达式，并保留源位置。递归调用走 clone_expr（带位置），
    /// 因此整棵复制出来的子树都有位置，诊断可以精确到子表达式。
    inline ExprPtr clone_expr(const Expression *e) {
        auto c = clone_expr_impl(e);
        if (c && e) {
            c->line = e->line;
            c->col = e->col;
        }
        return c;
    }
}
// cfront: c_ast.hpp —— C 的抽象语法树
//
// 设计目标：能表达 C11 的声明与语句，但只做语法结构与符号收集，
// 不做完整类型检查（那是 C 编译器的活，我们只需要为 LSP 提供符号与结构）。
//
// 声明符（declarator）是 C 语法里最反直觉的部分，这里按标准建模：
//   int (*f)(int)   →  Declarator{ kind=Paren, inner={kind=Pointer,
//                                inner={kind=Function, params=[int]}} }
// 即“从标识符往外剥”，指针/数组/函数的嵌套顺序被完整保留。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sinter::cfront {
    // ==========================================================================
    // 基础
    // ==========================================================================
    struct SourceRange {
        uint32_t begin_line = 1, begin_col = 1;
        uint32_t end_line = 1, end_col = 1;
        size_t begin_offset = 0, end_offset = 0;
    };

    /// 类型说明符的类别。C 的组合式写法（unsigned long long int）用 flags 表示。
    enum class TypeSpecKind : uint8_t {
        None, Void, Char, Short, Int, Long, Float, Double, Signed, Unsigned,
        Bool, Complex, Struct, Union, Enum, TypedefName, Typeof, Auto,
    };

    struct TypeSpec {
        std::vector<TypeSpecKind> kinds;   // 允许 "unsigned long long int" 多段
        std::string tag;                   // struct/union/enum 的标签名
        std::string typedef_name;          // typedef 名字（可能是用户类型）
        /// struct/union 的定义体（若在原地定义）
        std::shared_ptr<class CStructDef> struct_def;
        SourceRange range;

        bool empty() const { return kinds.empty(); }
        /// 拼成规范化的类型名，如 "unsigned long long" / "struct Foo"
        std::string canonical() const;
    };

    /// 声明符：从标识符往外一层层剥
    struct CDeclarator {
        enum class Kind : uint8_t {
            Identifier,  // 叶子：变量/函数名（可能为空，如抽象声明符 int(void)）
            Pointer,     // *inner（指向 inner）
            Reference,   // 仅 C++ 语义保留；C 中不用
            Array,       // inner[N]
            Function,    // inner(params)
            Paren,       // (inner) —— 用于表达 (*f)(int) 的优先级
        };

        Kind kind = Kind::Identifier;
        std::string name;
        SourceRange range;

        std::unique_ptr<CDeclarator> inner;   // Pointer/Array/Function/Paren 包装的目标

        // Pointer
        bool is_const_ptr = false;
        bool is_restrict_ptr = false;

        // Array
        std::string array_size_text;   // 原文（可能是宏或表达式）
        bool array_is_static = false;

        // Function
        std::vector<class CParam> params;
        bool variadic = false;

        /// 取最内层的标识符名（跳过所有包装）
        const std::string &leaf_name() const {
            const CDeclarator *d = this;
            while (d->inner && d->kind != Kind::Identifier) d = d->inner.get();
            if (d->kind == Kind::Identifier) return d->name;
            return name;
        }
    };

    struct CParam {
        TypeSpec spec;
        std::unique_ptr<CDeclarator> decl;
        std::string name() const { return decl ? decl->leaf_name() : std::string(); }
    };

    struct CStructDef;

    /// 单个成员（struct/union 内）
    struct CField {
        TypeSpec spec;
        std::unique_ptr<CDeclarator> decl;
        SourceRange range;
        /// 位域宽度原文（`unsigned x : 3` 的 "3"）；空表示不是位域
        std::string bit_width;
        /// enum 枚举值的初始化表达式原文（`A = 1 << 3` 的 "1 << 3"）
        std::string value_text;
        std::string name() const { return decl ? decl->leaf_name() : std::string(); }
    };

    struct CStructDef {
        std::string tag;
        bool is_union = false;
        bool is_enum = false;
        std::vector<CField> fields;
        std::vector<CField> enumerators;   // enum 的常量
        SourceRange range;
    };

    // ==========================================================================
    // 表达式
    // ==========================================================================
    struct CExpr;
    using CExprPtr = std::unique_ptr<CExpr>;

    /// 初始化列表里的单个元素：`{ .x = 1, [2] = 3, 4 }`
    struct CInitElem {
        /// `.field` 或 `[index]`；空表示位置初始化
        std::string designator;
        CExprPtr value;
    };

    struct CExpr {
        enum class Kind : uint8_t {
            IntLit, FloatLit, StringLit, CharLit, Identifier,
            Unary, Binary, Conditional, Assign, Call, Member, Index,
            Cast, Sizeof, Alignof, Comma, CompoundLiteral, Generic, InitList,
        };

        Kind kind = Kind::IntLit;
        SourceRange range;
        std::string text;          // 标识符名 / 字面量原文 / 运算符

        CExprPtr lhs, rhs, third;
        std::vector<CExprPtr> args; // Call 实参 / CompoundLiteral 初值 / Generic 分支值
        /// InitList 的元素（带指示符），`{ 1, .x = 2 }`
        std::vector<CInitElem> init_elems;
        /// `_Generic` 每个分支的关联类型（空串表示 `default`）
        std::vector<std::string> assoc_types;
        TypeSpec cast_type;         // Cast / Sizeof(type) / CompoundLiteral

        /// 若是 Member 引用，记录是否 '->'
        bool arrow = false;
    };

    // ==========================================================================
    // 语句
    // ==========================================================================
    struct CStmt;
    using CStmtPtr = std::unique_ptr<CStmt>;

    struct CStmt {
        enum class Kind : uint8_t {
            Compound, Decl, Expr, If, Switch, While, DoWhile, For, Return,
            Break, Continue, Goto, Case, Default, Label, Asm, Null,
        };

        Kind kind = Kind::Null;
        SourceRange range;

        CStmtPtr body, else_body, init;
        CExprPtr cond, step;
        std::vector<CStmtPtr> items;   // Compound / Switch
        std::string label;             // goto / label / case 表达式原文
        std::vector<CField> decls;     // Decl 语句里的声明（可能多个）
        TypeSpec decl_type;            // Decl 语句共用的类型说明符
    };

    // ==========================================================================
    // 顶层声明
    // ==========================================================================
    struct CFuncDef;

    struct CDeclItem {
        TypeSpec spec;
        std::unique_ptr<CDeclarator> decl;
        CExprPtr init;
        bool is_typedef = false;
        bool is_extern = false;
        bool is_static = false;
        bool is_inline = false;
        SourceRange range;

        std::string name() const { return decl ? decl->leaf_name() : std::string(); }
        bool is_function() const {
            return decl && (decl->kind == CDeclarator::Kind::Function ||
                            (decl->inner && decl->inner->kind == CDeclarator::Kind::Function));
        }
    };

    struct CFuncDef {
        CDeclItem decl;
        CStmtPtr body;
    };

    /// 顶层条目：声明或函数定义，或者一条预处理指令
    struct CUnitItem {
        enum class Kind : uint8_t {
            Declaration, FunctionDef, Preproc, Error, StaticAssert
        } kind = Kind::Declaration;
        CDeclItem decl;
        std::shared_ptr<CFuncDef> func;
        std::string preproc_text;
        std::string preproc_directive;
        /// `_Static_assert(cond, "msg")` 的条件原文与消息
        std::string assert_cond_text;
        std::string assert_msg;
        SourceRange range;
    };

    struct CTranslationUnit {
        std::string source_path;
        std::string source_text;
        std::vector<CUnitItem> items;

        /// 收集全部顶层函数定义的名字（LSP 文档符号 / 跳转用）
        std::vector<std::string> function_names() const;
        /// 收集全部 struct/union/enum 标签
        std::vector<std::string> tag_names() const;
        /// 收集 typedef 名字
        std::vector<std::string> typedef_names() const;
        /// 收集 #include 的目标
        std::vector<std::string> includes() const;
    };

    /// TypeSpec::canonical 的实现放在 c_ast.cpp
    std::string canonical_type(const TypeSpec &s);
} // namespace sinter::cfront
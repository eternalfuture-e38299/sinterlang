// cfront: c_parser.hpp —— C 语法分析器
//
// 定位：只做语法结构与符号收集，不做完整语义/类型检查，也不展开宏。
// 容错策略：遇到无法解析的语句时跳到下一个 ';' 或配对的 '}' 继续，
// 这样 LSP 在用户正在编辑（代码不完整）时仍能给出已有符号。
#pragma once
#include "cfront/c_ast.hpp"
#include "cfront/c_lexer.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sinter::cfront {
    struct CParseError {
        std::string message;
        uint32_t line = 1;
        uint32_t column = 1;
        /// 真正出错的文件（可能来自 `#include`）。空表示未知，回退到调用方给的路径。
        std::string file;
    };

    /// 预处理 / 包含路径选项。默认开启预处理（条件编译 + 宏展开 + #include）。
    struct CParseOptions {
        std::vector<std::string> include_dirs;                              ///< -I
        std::vector<std::pair<std::string, std::string> > predefined;        ///< -D name=body
        bool preprocess = true;
    };

    struct CParseResult {
        CTranslationUnit unit;
        /// 语法错误（带行列）
        std::vector<CParseError> errors;
        /// 预处理错误（已带 file:line:col 前缀的文本）
        std::vector<std::string> preproc_errors;
        /// 找不到的 #include 目标（由调用方决定是否硬错）
        std::vector<std::string> unresolved_includes;
        /// `CParseError::file` / `CToken::file_id` 对应的文件表（0 = 主文件）
        std::vector<std::string> files;
        std::set<std::string> macros;
        bool ok() const { return errors.empty() && preproc_errors.empty(); }
    };

    class CParser {
    public:
        /// 解析一段 C 源码（通常是 embed C 块的正文，或独立 .c/.h 文件）
        static CParseResult parse(const std::string &source, const std::string &path = "",
                                  const CParseOptions &options = {});

        /// 解析并额外给定已知 typedef 名字集合（跨块/跨文件时有帮助）
        static CParseResult parse_with_typedefs(const std::string &source,
                                                const std::set<std::string> &known,
                                                const std::string &path = "",
                                                const CParseOptions &options = {});

    private:
        CParser(CTokens toks, std::string source, std::string path);

        CTokens toks_;
        std::string src_;
        std::string path_;
        std::vector<std::string> files_;   ///< file_id → 路径（来自预处理）
        size_t pos_ = 0;
        std::vector<CParseError> errors_;
        std::set<std::string> typedefs_;
        std::vector<CUnitItem> unit_items_;   // parse_unit 的产出

        const CToken &peek(size_t k = 0) const;
        const CToken &advance();
        bool check(CTokKind k) const;
        bool at_end() const;
        bool match(CTokKind k);
        bool match_any(std::initializer_list<CTokKind> ks);
        const CToken *consume(CTokKind k, const char *what);

        SourceRange span_of(const CToken &t) const;
        void error_at(const CToken &t, const std::string &msg);
        void skip_to_recovery();

        // ---- 顶层 ----
        void parse_unit();
        bool parse_unit_item(CUnitItem &item);
        bool parse_decl_or_func(CUnitItem &item);
        bool parse_struct_or_enum(TypeSpec &spec);

        // ---- 声明 ----
        /// 解析类型说明符（可能为空，返回是否消费了内容）
        bool parse_type_spec(TypeSpec &spec);
        /// 解析声明符；allow_abstract=true 时允许没有名字（形参、抽象声明符）
        std::unique_ptr<CDeclarator> parse_declarator(bool allow_abstract);
        /// 解析声明符内层的后缀（数组/函数），被 parse_declarator 递归调用
        std::unique_ptr<CDeclarator> parse_direct_declarator(bool allow_abstract);
        bool parse_params(std::vector<CParam> &out, bool &variadic);
        bool looks_like_decl_start() const;

        // ---- 语句 ----
        CStmtPtr parse_statement();
        CStmtPtr parse_compound();
        CStmtPtr parse_if();
        CStmtPtr parse_while();
        CStmtPtr parse_do_while();
        CStmtPtr parse_for();
        CStmtPtr parse_switch();
        CStmtPtr parse_return();
        CStmtPtr parse_decl_statement();
        CStmtPtr parse_expr_statement();
        CStmtPtr parse_jump();
        CStmtPtr parse_labeled();

        // ---- 表达式（优先级从低到高）----
        CExprPtr parse_expression();
        CExprPtr parse_assign();
        CExprPtr parse_conditional();
        CExprPtr parse_binary(int min_prec);
        CExprPtr parse_unary();
        CExprPtr parse_postfix();
        CExprPtr parse_primary();
        bool is_type_start_here() const;
        /// 从当前游标偏移 off 处是否看起来像一个类型开头（用于 (type) 与 sizeof(type)）
        bool is_type_start_at(size_t off) const;
    };
} // namespace sinter::cfront
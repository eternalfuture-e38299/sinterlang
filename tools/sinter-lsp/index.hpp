// sinter-lsp: index.hpp —— lexical 
//
// ：**not semantic**。
// LSP    「、syntaxnot 」 code，AST not ；
// completion//hover/requires「name   declaration/reference」。
//  ： token，declaration and reference，offset。
//
//  and  cfront（C  ） ：
//   - Sinter  source → file（Sinter lexical + ）
//   - embed C { }   → cfront（ C syntax）
// passed embed_c_ranges ：position sinterd   cparse/symbols 。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "document.hpp"

namespace sinterlsp {
    enum class SymKind : uint8_t {
        Module,
        Function,
        Method,
        Class,
        Struct,
        Interface,
        Enum,
        EnumMember,
        Field,
        Variable,
        Constant,
        Parameter,
        Alias,
        TypeParam,
    };

    const char *sym_kind_name(SymKind k);
    /// to  LSP   SymbolKind （LSP  3.17）
    int sym_kind_lsp(SymKind k);

    struct Symbol {
        std::string name;
        SymKind kind = SymKind::Variable;
        size_t decl_offset = 0;    ///< name source  offset
        size_t name_len = 0;
        std::string detail;        ///< hover line/type
        std::string container;     ///< class/module（completion  ）
        bool is_public = true;
        int scope_depth = 0;       ///< 0 = ，number
        ///  type（`var x: Foo`、`fun f(r: Foo)`   Foo）。
        /// completion：`r.`   r  typecolumnthe type 。
        /// typenot  is 。
        std::string type_name;
    };

    /// reference：   name  Symbol（nameparse）
    struct Reference {
        std::string name;
        size_t offset = 0;
        size_t name_len = 0;
        std::string container;   ///< reference class（ `self.field` class）
    };

    struct DiagnosticLite {
        std::string message;
        int severity = 1;   ///< 1 = Error, 2 = Warning, 3 = Information
        size_t offset = 0;
        size_t length = 1;
    };

    /// embed C  positioninfo： range（not  ）
    struct EmbedRange {
        size_t body_begin = 0;
        size_t body_end = 0;
        bool found = false;

        [[nodiscard]] bool contains(const size_t off) const {
            return found && off >= body_begin && off < body_end;
        }
    };

    struct Index {
        TextBuffer buf{std::string()};
        std::vector<Symbol> symbols;
        std::vector<Reference> refs;
        std::vector<DiagnosticLite> diagnostics;
        EmbedRange embed_c;

        /// namedeclaration（ /not ；return all）
        [[nodiscard]] std::vector<const Symbol *> find_symbols(const std::string &name) const;
        /// positionreference   name（return name and the name offset；not to return ）
        [[nodiscard]] bool ident_at(size_t offset, std::string &name, size_t &beg,
                                    size_t &len) const;
        /// position     declaration
        [[nodiscard]] const Symbol *symbol_decl_at(size_t offset) const;
    };

    /// （lexical，）
    Index build_index(const std::string &uri, const std::string &text);
} // namespace sinterlsp
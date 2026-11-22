// sinter-lsp: index.cpp —— lexical 
//
// ： Sinter lexicalto  token （line/column offset），
// 「declaration-introducing keyword」declaration，its is reference。
// then （string/comment），  AST （not code ）。
#include "index.hpp"

#include "front/lexer.hpp"
#include "support/token.hpp"

#include <algorithm>
#include <unordered_map>

namespace sinterlsp {
    const char *sym_kind_name(const SymKind k) {
        switch (k) {
            case SymKind::Module: return "module";
            case SymKind::Function: return "function";
            case SymKind::Method: return "method";
            case SymKind::Class: return "class";
            case SymKind::Struct: return "struct";
            case SymKind::Interface: return "interface";
            case SymKind::Enum: return "enum";
            case SymKind::EnumMember: return "enumMember";
            case SymKind::Field: return "field";
            case SymKind::Variable: return "variable";
            case SymKind::Constant: return "constant";
            case SymKind::Parameter: return "parameter";
            case SymKind::Alias: return "typeAlias";
            case SymKind::TypeParam: return "typeParameter";
        }
        return "variable";
    }

    int sym_kind_lsp(const SymKind k) {
        //  LSP 3.17 SymbolKind enum
        switch (k) {
            case SymKind::Module: return 2;       // Module
            case SymKind::Function: return 12;    // Function
            case SymKind::Method: return 6;       // Method
            case SymKind::Class: return 5;        // Class
            case SymKind::Struct: return 23;      // Struct
            case SymKind::Interface: return 11;   // Interface
            case SymKind::Enum: return 10;        // Enum
            case SymKind::EnumMember: return 22;  // EnumMember
            case SymKind::Field: return 8;        // Field
            case SymKind::Variable: return 13;    // Variable
            case SymKind::Constant: return 14;    // Constant
            case SymKind::Parameter: return 13;   // Variable（LSP no  Parameter）
            case SymKind::Alias: return 26;       // TypeParameter  
            case SymKind::TypeParam: return 26;   // TypeParameter
        }
        return 13;
    }

    std::vector<const Symbol *> Index::find_symbols(const std::string &name) const {
        std::vector<const Symbol *> out;
        for (const auto &s: symbols)
            if (s.name == name) out.push_back(&s);
        return out;
    }

    bool Index::ident_at(const size_t offset, std::string &name, size_t &beg,
                         size_t &len) const {
        const std::string &t = buf.text();
        if (t.empty()) return false;
        const size_t off = std::min(offset, t.size() - 1);
        auto is_id = [](const char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_';
        };
        if (!is_id(t[off])) return false;
        size_t b = off;
        while (b > 0 && is_id(t[b - 1])) --b;
        size_t e = off;
        while (e < t.size() && is_id(t[e])) ++e;
        beg = b;
        len = e - b;
        name = t.substr(b, len);
        // numbernot 
        if (!name.empty() && name[0] >= '0' && name[0] <= '9') return false;
        return true;
    }

    const Symbol *Index::symbol_decl_at(const size_t offset) const {
        for (const auto &s: symbols) {
            if (offset >= s.decl_offset && offset < s.decl_offset + s.name_len) return &s;
        }
        return nullptr;
    }

    namespace {
        using sinter::Token;
        using sinter::TokenType;
        using sinter::Tokens;

        /// Token   line/column   1 、；offset。
        /// Sinter lexical line/column ，  TextBuffer 。
        size_t token_offset(const TextBuffer &buf, const Token &t) {
            // line/column from  1 start
            return buf.offset_at(Position{static_cast<int>(t.line) - 1,
                                          static_cast<int>(t.column) - 1});
        }

        bool is_ident_char(const char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_';
        }

        bool is_decl_keyword(const TokenType t) {
            switch (t) {
                case TokenType::Fun:
                case TokenType::Class:
                case TokenType::Struct:
                case TokenType::Interface:
                case TokenType::Enum:
                case TokenType::Module:
                case TokenType::Var:
                case TokenType::Val:
                case TokenType::Const:
                case TokenType::Alias:
                    return true;
                default: return false;
            }
        }

        SymKind kind_for_decl_keyword(const TokenType t, const bool in_class) {
            switch (t) {
                case TokenType::Fun: return in_class ? SymKind::Method : SymKind::Function;
                case TokenType::Class: return SymKind::Class;
                case TokenType::Struct: return SymKind::Struct;
                case TokenType::Interface: return SymKind::Interface;
                case TokenType::Enum: return SymKind::Enum;
                case TokenType::Module: return SymKind::Module;
                case TokenType::Val: return in_class ? SymKind::Field : SymKind::Constant;
                case TokenType::Const: return SymKind::Constant;
                case TokenType::Var: return in_class ? SymKind::Field : SymKind::Variable;
                case TokenType::Alias: return SymKind::Alias;
                default: return SymKind::Variable;
            }
        }

        ///    `embed C { ... }`  。
        ///
        /// is not  EmbedCBody token  position：Sinter lexical add_token   
        /// 「token end 」  line/column，cannot（）。
        ///  embed C  syntax，   `embed` + `C` + `{`，
        /// from  `{` start   ；string  textnot will ，
        /// is  failedwill  found  false，not its result。
        EmbedRange find_embed_c_body(const std::string &text) {
            EmbedRange r;
            const size_t n = text.size();
            size_t i = 0;
            while (i < n) {
                //  `embed` （  not  ）
                if (text.compare(i, 5, "embed") != 0) {
                    ++i;
                    continue;
                }
                const bool ok_before = i == 0 || !is_ident_char(text[i - 1]);
                const bool ok_after = i + 5 >= n || !is_ident_char(text[i + 5]);
                if (!ok_before || !ok_after) {
                    ++i;
                    continue;
                }
                // ， `C`
                size_t j = i + 5;
                while (j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r')) ++j;
                if (j >= n || text[j] != 'C') {
                    ++i;
                    continue;
                }
                ++j;
                if (j < n && is_ident_char(text[j])) {   // `embed CX` class，not   embed C
                    ++i;
                    continue;
                }
                //  and  comment/line，to  `{`
                while (j < n && text[j] != '{') {
                    if (text[j] == '}' || text[j] == ';') break;   //  `embed C` declaration，no 
                    ++j;
                }
                if (j >= n || text[j] != '{') {
                    ++i;
                    continue;
                }
                // from  `{` start  ；string and 、comment
                const size_t body_begin = j + 1;
                int depth = 1;
                size_t k = body_begin;
                while (k < n) {
                    const char c = text[k];
                    if (c == '/' && k + 1 < n && text[k + 1] == '/') {
                        while (k < n && text[k] != '\n') ++k;
                        continue;
                    }
                    if (c == '/' && k + 1 < n && text[k + 1] == '*') {
                        k += 2;
                        while (k + 1 < n && !(text[k] == '*' && text[k + 1] == '/')) ++k;
                        k = std::min(k + 2, n);
                        continue;
                    }
                    if (c == '"' || c == '\'') {
                        const char q = c;
                        ++k;
                        while (k < n && text[k] != q) {
                            if (text[k] == '\\') ++k;
                            ++k;
                        }
                        if (k < n) ++k;
                        continue;
                    }
                    if (c == '{') ++depth;
                    else if (c == '}') {
                        --depth;
                        if (depth == 0) {
                            r.body_begin = body_begin;
                            r.body_end = k;
                            r.found = true;
                            return r;
                        }
                    }
                    ++k;
                }
                //  （  code）：to 
                r.body_begin = body_begin;
                r.body_end = n;
                r.found = true;
                return r;
            }
            return r;
        }
    } // namespace

    Index build_index(const std::string &uri, const std::string &text) {
        (void) uri;   //  not dependency uri（file），args is will file/
        Index idx;
        idx.buf = TextBuffer(text);
        TextBuffer &buf = idx.buf;

        // 1) lexical。Sinter lexical to invalidinput will ， ：
        //     code，failednot the  LSP 。
        Tokens toks;
        try {
            sinter::Lexer lex(text);
            toks = lex.tokenize();
        } catch (const std::exception &e) {
            idx.diagnostics.push_back({std::string("lex error: ") + e.what(), 1, 0, 1});
            return idx;
        } catch (...) {
            idx.diagnostics.push_back({"lex error: cannot input", 1, 0, 1});
            return idx;
        }

        // 2) ：declaration，reference， embed C range
        int brace_depth = 0;
        int class_depth = -1;      //     class/struct/interface/enum 
        std::string container;     //  type
        // to type、to   `{`：name，etc `{`    class_depth 。
        // cannot to name  class_depth = brace_depth ——   `{` not  depth ，
        // exit  「depth <= class_depth」， not will  container not 。
        std::string pending_container;
        // a "declaration-introducing keyword"（ pub/inline/static etc）
        TokenType pending_decl = TokenType::Unknown;
        bool pending_pub = false;

        for (size_t i = 0; i < toks.size(); ++i) {
            const Token &t = toks[i];
            if (t.type == TokenType::EndOfFile) break;

            const size_t off = token_offset(buf, t);

            if (t.type == TokenType::LeftBrace) {
                //  not  「type 」：  pending_container   name。
                // must  ，is  self-declared depth   「」：
                //   class Res {   ←   brace_depth   0，to  `{`    1
                if (!pending_container.empty() && class_depth < 0) {
                    class_depth = brace_depth + 1;   // 
                    container = pending_container;
                }
                pending_container.clear();
                ++brace_depth;
                continue;
            }
            if (t.type == TokenType::RightBrace) {
                if (class_depth >= 0 && brace_depth <= class_depth) {
                    class_depth = -1;
                    container.clear();
                }
                if (brace_depth > 0) --brace_depth;
                continue;
            }
            if (t.type == TokenType::Newline) continue;

            // embed C：Sinter lexical  add_token   「token end 」  line/column，
// cannot EmbedCBody  position。 is     ，
//   Sinter language  embed C  definition，   。
            if (t.type == TokenType::EmbedCBody) {
                idx.embed_c = find_embed_c_body(text);
                continue;
            }

            if (t.type == TokenType::Pub) {
                pending_pub = true;
                continue;
            }
            if (t.type == TokenType::Inline || t.type == TokenType::Static ||
                t.type == TokenType::Abstract || t.type == TokenType::Final ||
                t.type == TokenType::Override) {
                continue;
            }

            if (is_decl_keyword(t.type)) {
                pending_decl = t.type;
                continue;
            }

            if (t.type == TokenType::Identifier) {
                // 「  type」：class_depth   type 「」，
                // so brace_depth to the i.e. （==  ，>  function）。
                const bool in_class = class_depth >= 0 && brace_depth >= class_depth;
                if (pending_decl != TokenType::Unknown) {
                    Symbol s;
                    s.name = t.lexeme;
                    s.kind = kind_for_decl_keyword(pending_decl, in_class);
                    s.decl_offset = off;
                    s.name_len = t.lexeme.size();
                    s.container = container;
                    s.is_public = pending_pub;
                    s.scope_depth = brace_depth;
                    s.detail = std::string(sym_kind_name(s.kind)) + " " + s.name;
                    idx.symbols.push_back(std::move(s));

                    // type： name，etc   `{`  
                    // （ LeftBrace ）， class_depth  and  brace_depth  。
                    if (pending_decl == TokenType::Class || pending_decl == TokenType::Struct ||
                        pending_decl == TokenType::Interface || pending_decl == TokenType::Enum) {
                        pending_container = t.lexeme;
                    }
                    // `fun f(...)`： args is  
                    if (pending_decl == TokenType::Fun) {
                        idx.symbols.back().detail = "fun " + t.lexeme + "(…)";
                    }
                    pending_decl = TokenType::Unknown;
                    pending_pub = false;

                    // `var x: Foo` / `val x: Foo`：  `:`    type。
                    //  is  completionfrom  `x.` type（lexicalcannottype）。
                    if (i + 2 < toks.size() && toks[i + 1].type == TokenType::Colon &&
                        toks[i + 2].type == TokenType::Identifier) {
                        idx.symbols.back().type_name = toks[i + 2].lexeme;
                        // detail  type，hover to  `variable total: i64`
                        if (idx.symbols.back().kind == SymKind::Variable ||
                            idx.symbols.back().kind == SymKind::Field ||
                            idx.symbols.back().kind == SymKind::Constant)
                            idx.symbols.back().detail += ": " + toks[i + 2].lexeme;
                    }
                    continue;
                }
                // args：`(`   column，  `,`  or  `)`，   `{`/`->`/line
                idx.refs.push_back({t.lexeme, off, t.lexeme.size(), container});
                continue;
            }

            // its  token not reference，if   `=` class ，pending_decl 
            // （e.g.  `var x: T = ...`   x already ； ）
        }

        // 3) args：「 、 、  」 
        //    args  AST  info， ，completion。
        for (size_t i = 1; i + 1 < toks.size(); ++i) {
            if (toks[i].type != TokenType::Identifier) continue;
            if (toks[i - 1].type != TokenType::LeftParen && toks[i - 1].type != TokenType::Comma)
                continue;
            if (toks[i + 1].type != TokenType::Colon) continue;
            const size_t off = token_offset(buf, toks[i]);
            //     type（`r: Res` → Res）。
            // /pointer/arrayclassnot ，completionrequiresto  name。
            std::string ty;
            if (i + 2 < toks.size() && toks[i + 2].type == TokenType::Identifier)
                ty = toks[i + 2].lexeme;
            // already   symbols  not duplicate
            bool dup = false;
            for (const auto &s: idx.symbols) {
                if (s.decl_offset == off) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            Symbol s;
            s.name = toks[i].lexeme;
            s.kind = SymKind::Parameter;
            s.decl_offset = off;
            s.name_len = s.name.size();
            s.type_name = ty;
            s.detail = ty.empty() ? "args " + s.name : "args " + s.name + ": " + ty;
            s.scope_depth = 0;
            idx.symbols.push_back(std::move(s));
        }

        return idx;
    }
} // namespace sinterlsp
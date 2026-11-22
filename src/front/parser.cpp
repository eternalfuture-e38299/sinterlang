// sinter parser (part 1: infrastructure + declarations)
#include "front/parser.hpp"
#include "front/lexer.hpp"
#include <iostream>
#include <cctype>
#include <cstdlib>

namespace sinter {
    Parser::Parser(const std::vector<Token> &t) : tokens(t) {}
    Parser::Parser(std::vector<Token> &&t) : tokens(std::move(t)) {}
    Parser::Parser(const std::vector<Token> &t, const int base_line, const int base_col)
        : tokens(t), base_line_(base_line), base_col_(base_col) {}
    Parser::Parser(std::vector<Token> &&t, const int base_line, const int base_col)
        : tokens(std::move(t)), base_line_(base_line), base_col_(base_col) {}

    /// 把子解析器里的相对位置平移回外层源码。
    /// 第一行的列要加上片段起点的列偏移；第 2 行起列本来就是绝对列。
    Token Parser::shift_position(Token t) const {
        if (base_line_ <= 0) return t;
        if (t.line == 1) t.column += static_cast<size_t>(base_col_ > 0 ? base_col_ - 1 : 0);
        t.line += static_cast<size_t>(base_line_ - 1);
        return t;
    }

    Token Parser::peek() const {
        return current < tokens.size() ? shift_position(tokens[current]) : Token(TokenType::EndOfFile);
    }
    Token Parser::peek_next() const {
        return current + 1 < tokens.size() ? shift_position(tokens[current + 1])
                                           : Token(TokenType::EndOfFile);
    }
    Token Parser::peek_at(size_t n) const {
        return current + n < tokens.size() ? shift_position(tokens[current + n])
                                           : Token(TokenType::EndOfFile);
    }
    Token Parser::previous() const {
        return current > 0 ? shift_position(tokens[current - 1]) : Token(TokenType::EndOfFile);
    }
    bool Parser::check(const TokenType t) const { return !is_at_end() && peek().type == t; }
    bool Parser::is_at_end() const { return peek().type == TokenType::EndOfFile; }

    Token Parser::advance() {
        if (!is_at_end()) {
            Token t = shift_position(tokens[current++]);
            line = static_cast<int>(t.line);
            column = static_cast<int>(t.column);
            return t;
        }
        return tokens.back();
    }
    bool Parser::match(TokenType t) {
        if (check(t)) { advance(); return true; }
        return false;
    }
    bool Parser::match_any(std::initializer_list<TokenType> ts) {
        for (const auto t: ts) if (match(t)) return true;
        return false;
    }
    Token Parser::consume(TokenType t, const std::string &m) {
        if (check(t)) return advance();
        error(m);
    }
    [[noreturn]] void Parser::error(const std::string &m) const {
        const Token t = peek();
        // 统一在这里补「实际看到了什么」：consume 的调用点只管说期望什么，
        // 不必每条消息自己拼 found。否则 146 个调用点里有几个漏拼，用户就会
        // 拿到「expected ';'」却不知道看到的是什么。
        std::string detail = m;
        if (t.type == TokenType::EndOfFile) detail += ", found end of file";
        else if (t.type == TokenType::Newline) detail += ", found end of line";
        else if (!t.lexeme.empty()) detail += ", found '" + t.lexeme + "'";
        if (diags_) {
            diags_->error_ex("E0100", detail, "", source_path_,
                             static_cast<int>(t.line), static_cast<int>(t.column), 0);
        } else {
            std::cerr << "parse error at " << t.line << ":" << t.column << ": " << detail << "\n";
        }
        throw ParseError(detail);
    }
    void Parser::consume_newlines() { while (match(TokenType::Newline)) {} }
    void Parser::skip_all_newlines_and_semis() {
        while (match(TokenType::Newline) || match(TokenType::Semicolon)) {}
    }
    void Parser::expect_statement_end(const std::string &m) {
        if (match(TokenType::Semicolon)) return;
        if (check(TokenType::Newline)) { consume_newlines(); return; }
        if (check(TokenType::RightBrace) || is_at_end()) return;
        error(m);
    }
    void Parser::parse_annotations(std::vector<Annotation> &into) {
        while (match(TokenType::At)) {
            Annotation a;
            a.name = consume(TokenType::Identifier, "expected annotation name").lexeme;
            if (match(TokenType::LeftParen)) {
                if (!check(TokenType::RightParen)) {
                    do { a.args.push_back(parse_expression()); } while (match(TokenType::Comma));
                }
                consume(TokenType::RightParen, "expected ')'");
            }
            into.push_back(std::move(a));
            consume_newlines();
        }
    }

    StmtPtr Parser::parse_declaration() {
        // 先跳过空行与分号，让位置落在真正的语句起点上。
        skip_all_newlines_and_semis();
        const Token start = peek();
        auto s = parse_declaration_impl();
        if (s && s->line == 0) {
            s->line = static_cast<int>(start.line);
            s->col = static_cast<int>(start.column);
        }
        return s;
    }

    StmtPtr Parser::parse_declaration_impl() {
        if (getenv("SINTER_DEBUG_DECL"))
            std::cerr << "[decl] line " << peek().line << " lexeme='" << peek().lexeme
                      << "' type=" << static_cast<int>(peek().type) << "\n";
        skip_all_newlines_and_semis();
        std::vector<Annotation> anns;
        parse_annotations(anns);
        bool is_pub = in_pub_block, is_inline = false, is_static = false;
        bool is_private = false;
        bool is_override = false, is_abstract = false, is_final = false;
        bool loop = true;
        while (loop) {
            loop = false;
            if (match(TokenType::Override)) { is_override = true; loop = true; }
            else if (match(TokenType::Pub)) { is_pub = true; loop = true; }
            else if (match(TokenType::Private)) { is_private = true; loop = true; }
            else if (match(TokenType::Inline)) { is_inline = true; loop = true; }
            else if (match(TokenType::Static)) { is_static = true; loop = true; }
            else if (match(TokenType::Abstract)) { is_abstract = true; loop = true; }
            else if (match(TokenType::Final)) { is_final = true; loop = true; }
            if (loop) consume_newlines();
        }

        if (match(TokenType::Module)) return parse_module();
        if (match(TokenType::Import)) return parse_import();

        // pub { ... } / priv { ... }
        if ((is_pub || is_private) && check(TokenType::LeftBrace)) {
            advance();
            auto blk = std::make_unique<PubBlockStmt>();
            blk->annotations = std::move(anns);
            consume_newlines();
            const bool saved = in_pub_block;
            in_pub_block = is_pub && !is_private;
            while (!check(TokenType::RightBrace) && !is_at_end()) {
                if (auto s = parse_declaration()) blk->statements.push_back(std::move(s));
                consume_newlines();
            }
            in_pub_block = saved;
            consume(TokenType::RightBrace, "expected '}'");
            return blk;
        }

        if (match(TokenType::Ext)) {
            consume_newlines();
            consume(TokenType::Fun, "expected 'fun' after 'ext'");
            // ext fun <接收者类型>.<名称>(params...)：先吃掉 "T." 前缀再按普通函数解析
            std::string receiver;
            if (check(TokenType::Identifier) && peek_next().type == TokenType::Dot
                && peek_at(2).type == TokenType::Identifier) {
                receiver = advance().lexeme;
                advance();   // '.'
            }
            auto f = parse_function(std::move(anns), is_pub && !is_private, is_inline, is_static);
            if (auto *fd = dynamic_cast<FunctionDecl *>(f.get())) {
                if (!receiver.empty()) {
                    fd->ext_receiver = receiver;
                    // 扩展函数的接收者即隐式第一个形参 self
                    if (fd->params.empty() || fd->params[0].name != "self") {
                        Parameter sp;
                        sp.name = "self";
                        sp.type = std::make_unique<TypeAnnotation>(receiver);
                        fd->params.insert(fd->params.begin(), std::move(sp));
                    }
                } else if (const auto dot = fd->name.find('.'); dot != std::string::npos) {
                    fd->ext_receiver = fd->name.substr(0, dot);
                    fd->name = fd->name.substr(dot + 1);
                    if (!fd->params.empty()) fd->params[0].name = "self";
                }
            }
            return f;
        }

        if (match(TokenType::Fun)) {
            // fun operator+(...)：把 `fun` 吃掉后转到运算符解析
            if (check(TokenType::Operator)) {
                advance();
                auto op = parse_operator(std::move(anns), is_pub);
                if (auto *fd = dynamic_cast<FunctionDecl *>(op.get())) {
                    fd->is_override = is_override;
                    fd->is_static = is_static;
                    fd->is_inline = is_inline;
                }
                return op;
            }
            auto f = parse_function(std::move(anns), is_pub && !is_private, is_inline, is_static);
            if (auto *fd = dynamic_cast<FunctionDecl *>(f.get())) {
                fd->is_override = is_override;
                if (is_abstract) fd->is_abstract = true;
            }
            return f;
        }
        if (match(TokenType::Operator)) return parse_operator(std::move(anns), is_pub);
        if (match(TokenType::Annotation)) {
            consume_newlines();
            const bool is_struct = match(TokenType::Struct);
            if (!is_struct) consume(TokenType::Class, "expected 'class' or 'struct' after 'annotation'");
            auto c = parse_class_like(std::move(anns), is_struct, is_pub);
            if (auto *cd = dynamic_cast<ClassDecl *>(c.get())) {
                cd->is_annotation = true;
                cd->is_final = true;
            }
            return c;
        }
        if (match(TokenType::Class) || match(TokenType::Struct)) {
            const bool is_struct = previous().type == TokenType::Struct;
            auto c = parse_class_like(std::move(anns), is_struct, is_pub);
            if (auto *cd = dynamic_cast<ClassDecl *>(c.get())) {
                if (is_abstract) cd->is_abstract = true;
                cd->is_final = is_final;
            }
            return c;
        }
        // `data class` / `data struct`：`data` 是上下文修饰符，不是关键字，
        // 因此 `val data = ...` 与字段名 `data` 都保持可用。
        if (check(TokenType::Identifier) && peek().lexeme == "data" &&
            (peek_next().type == TokenType::Class || peek_next().type == TokenType::Struct)) {
            advance();
            consume_newlines();
            const bool is_struct = check(TokenType::Struct);
            advance();   // class / struct
            auto c = parse_class_like(std::move(anns), is_struct, is_pub, true);
            if (auto *cd = dynamic_cast<ClassDecl *>(c.get())) {
                cd->is_data = true;
                if (is_abstract) cd->is_abstract = true;
                cd->is_final = is_final;
            }
            return c;
        }
        if (match(TokenType::Interface)) return parse_interface(std::move(anns), is_pub);
        if (match(TokenType::Enum)) return parse_enum(std::move(anns), is_pub);
        if (match(TokenType::Const)) return parse_var_decl(true, is_static, is_pub);
        if (match(TokenType::Val)) {
            if (check(TokenType::LeftParen)) { advance(); return parse_destructure(false); }
            return parse_var_decl(false, is_static, is_pub);
        }
        if (match(TokenType::Var)) {
            if (check(TokenType::LeftParen)) { advance(); return parse_destructure(true); }
            return parse_var_decl(false, is_static, is_pub);
        }
        if (match(TokenType::Alias)) return parse_alias(is_pub);
        if (match(TokenType::Comptime)) return parse_comptime(std::move(anns));
        if (match(TokenType::Bind)) return parse_bind_c();
        if (match(TokenType::Embed)) return parse_embed_c(std::move(anns));
        if (match(TokenType::Del)) return parse_del();

        if (match(TokenType::Ret)) return parse_return();
        if (match(TokenType::If)) return parse_if_stmt();
        if (match(TokenType::Try)) return parse_try();
        if (match(TokenType::Throw)) return parse_throw();
        if (match(TokenType::While)) return parse_while();
        if (match(TokenType::Do)) return parse_do_while();
        if (match(TokenType::For)) return parse_for();
        if (match(TokenType::Break)) return parse_break();
        if (match(TokenType::Continue)) return parse_continue();

        // extern fun name(...)：实现 C 头里已声明的函数。
        // `extern` 不是关键字（不占常用标识符），只在后面紧跟 `fun` 时按上下文识别；
        // `val extern = 1` 仍然合法（回退到普通声明路径）。
        if (check(TokenType::Identifier) && peek().lexeme == "extern") {
            const auto save = current;
            advance();
            consume_newlines();
            if (check(TokenType::Fun)) {
                advance();
                auto f = parse_function(std::move(anns), /*is_pub=*/false, is_inline, is_static);
                if (auto *fd = dynamic_cast<FunctionDecl *>(f.get())) {
                    fd->is_extern_c = true;
                    fd->is_pub = false;   // 声明由 C 头负责，这里只提供定义
                    fd->is_override = is_override;
                }
                return f;
            }
            current = save;
        }

        // no_raii { ... }：块内不做 RAII 清理（不生成析构调用）
        if (check(TokenType::Identifier) && peek().lexeme == "no_raii") {
            const auto save = current;
            advance();
            if (check(TokenType::LeftBrace)) { advance(); return parse_no_raii_block(); }
            current = save;
        }

        if (match(TokenType::LeftBrace)) return parse_block();
        return parse_expr_stmt();
    }

    StmtPtr Parser::parse_declaration_with_modifiers(std::vector<Annotation> anns, bool, bool, bool) {
        (void) anns;
        return parse_declaration();
    }

    StmtPtr Parser::parse_module() {
        auto m = std::make_unique<ModuleStmt>();
        m->name = consume(TokenType::Identifier, "expected module name").lexeme;
        while (match(TokenType::Dot)) m->name += "." + consume(TokenType::Identifier, "expected module name").lexeme;
        // module foo { ... } —— 块式模块，向当前模块导出内部声明
        if (check(TokenType::LeftBrace)) {
            advance();
            m->is_block = true;
            consume_newlines();
            while (!check(TokenType::RightBrace) && !is_at_end()) {
                if (auto s = parse_declaration()) m->body.push_back(std::move(s));
                consume_newlines();
            }
            consume(TokenType::RightBrace, "expected '}'");
            return m;
        }
        expect_statement_end("expected end of statement");
        return m;
    }
    StmtPtr Parser::parse_import() {
        auto i = std::make_unique<ImportStmt>();
        i->path = consume(TokenType::Identifier, "expected import path").lexeme;
        while (match(TokenType::Dot)) i->path += "." + consume(TokenType::Identifier, "expected module name").lexeme;
        expect_statement_end("expected end of statement");
        return i;
    }

    /// 运算符重载：`operator+(other: T): T { ... }`，可写在类体内（成员运算符）
/// 或模块顶层（自由运算符，此时第一个参数是左操作数）。
    /// 运算符符号被编码进函数名：`operator+`、`operator==`、`operator[]` 等，
    /// 语义层与后端都按这个前缀识别。
    StmtPtr Parser::parse_operator(std::vector<Annotation> anns, bool is_pub) {
        // 收集运算符符号：可能是一个或多个 token（如 `==`、`[]`、`<=>`）
        std::string sym;
        if (check(TokenType::LeftBracket)) {
            advance();
            consume(TokenType::RightBracket, "expected ']'");
            sym = "[]";
        } else if (match(TokenType::LeftParen)) {
            consume(TokenType::RightParen, "expected ')'");
            sym = "()";
        } else {
            const Token t = advance();
            if (t.type == TokenType::EndOfFile || t.type == TokenType::Unknown)
                error("expected operator symbol");
            sym = t.lexeme;
        }

        auto f = std::make_unique<FunctionDecl>();
        f->annotations = std::move(anns);
        f->is_pub = is_pub || in_pub_block;
        f->no_raii = no_raii_context;
        f->is_operator = true;
        f->name = "operator" + sym;

        // 参数列表
        if (check(TokenType::LeftParen)) {
            advance();
            consume_newlines();
            if (!check(TokenType::RightParen)) {
                f->params = parse_parameters();
                apply_variadic(*f);
            }
            consume_newlines();
            consume(TokenType::RightParen, "expected ')'");
        }
        if (match(TokenType::Colon)) {
            consume_newlines();
            f->return_type = parse_type();
        }
        if (match(TokenType::LeftBrace)) {
            f->body = parse_block();
        } else if (match(TokenType::Equal) && match(TokenType::Default)) {
            f->is_default = true;
            expect_statement_end("expected end of statement");
        } else {
            expect_statement_end("expected end of statement");
        }
        return f;
    }

    StmtPtr Parser::parse_function(std::vector<Annotation> anns, bool is_pub, bool is_inline, bool is_static) {
        auto f = std::make_unique<FunctionDecl>();
        f->annotations = std::move(anns);
        f->is_pub = is_pub || in_pub_block;
        f->is_inline = is_inline;
        f->is_static = is_static;
        f->no_raii = no_raii_context;
        for (auto &a: f->annotations) {
            if (a.name == "no_raii") f->no_raii = true;
            if (a.name == "inline") f->is_inline = true;
            if (a.name == "pub") f->is_pub = true;
        }

        f->name = consume(TokenType::Identifier, "expected function name").lexeme;
        while (check(TokenType::Dot) && peek_next().type == TokenType::Identifier) {
            advance();
            f->name += "." + advance().lexeme;
        }

        // 泛型 <T must number> / <T, Args...>
        if (match(TokenType::Less)) {
            while (check(TokenType::Greater)) break;   // 不允许 <>
            do {
                if (check(TokenType::Greater)) break;   // 尾随逗号：fun f<T,>
                FunctionDecl::TemplateParam tp;
                tp.name = consume(TokenType::Identifier, "expected generic parameter").lexeme;
                if (match(TokenType::Must)) tp.constraint = parse_type();
                // 变长类型参数包：<T, Args...>
                if (match(TokenType::DotDotDot)) tp.is_pack = true;
                f->tparams.push_back(std::move(tp));
            } while (match(TokenType::Comma));
            consume(TokenType::Greater, "expected '>'");
            validate_tparams(f->tparams);
        }

        // Sinter alias: `fun square[sq](...)` adds another callable name.
        // Generic functions may restate their type parameters:
        //     fun arrayOf<T>[anyArrayOf<T>](e: T...)
        // The alias parameter names are documentation only; codegen maps explicit
        // type arguments by position against the real `<T>` list.
        if (match(TokenType::LeftBracket)) {
            f->alias_name = consume(TokenType::Identifier, "expected alias name").lexeme;
            if (match(TokenType::Less)) {
                size_t alias_params = 0;
                do {
                    if (check(TokenType::Greater)) break;
                    consume(TokenType::Identifier, "expected alias generic parameter");
                    ++alias_params;
                    if (match(TokenType::Must)) (void) parse_type();
                    if (check(TokenType::DotDotDot)) advance();
                } while (match(TokenType::Comma));
                consume(TokenType::Greater, "expected '>' after alias type parameters");
                if (alias_params != f->tparams.size()) {
                    error("alias '" + f->alias_name + "' declares " + std::to_string(alias_params) +
                          " type parameter(s), but '" + f->name + "' has " +
                          std::to_string(f->tparams.size()));
                }
            }
            while (match(TokenType::Dot))
                f->alias_name += "_" + consume(TokenType::Identifier, "expected alias name").lexeme;
            consume(TokenType::RightBracket, "expected ']'");
        }

        // 重载分支：fun square : i64 = ...（无参数列表）
        if (check(TokenType::LeftParen)) {
            // 参数表与返回类型里都可能出现 `Args[i]`，先在作用域内登记包名。
            // 用守卫对象保证所有 return 路径都会恢复（函数体里也允许出现 Args[i]）。
            for (auto &tp: f->tparams)
                if (tp.is_pack) active_pack_names_.push_back(tp.name);
            pack_scope_.emplace(active_pack_names_, active_pack_names_.size());

            advance();
            consume_newlines();
            f->params = parse_parameters();
            apply_variadic(*f);
            consume_newlines();
            consume(TokenType::RightParen, "expected ')'");
        }

        if (match(TokenType::Colon)) {
            consume_newlines();
            f->return_type = parse_type();
        }

        // 尾置约束：fun f<T>(...): T must number
        // 无泛型参数时视为对首个泛型参数/返回类型的约束，仅做语法接收，语义层已按 number 校验。
        if (match(TokenType::Must)) {
            auto c = parse_type();
            if (f->tparams.empty()) {
                FunctionDecl::TemplateParam tp;
                tp.name = f->return_type ? f->return_type->name : "T";
                tp.constraint = std::move(c);
                f->tparams.push_back(std::move(tp));
            } else {
                f->tparams.back().constraint = std::move(c);
            }
        }

        if (match(TokenType::Semicolon)) return f;

        // 表达式函数体：fun f(): T = expr / fun f = expr
        // 与"function alias"（fun f = g）区分：若 `=` 之后紧接 Identifier 后跟 `(` 或 `<`，
        // 且该标识符不是已声明function alias目标（无法静态确定），这里统一按表达式解析，
        // 因为表达式函数是更常用的语义；显式别名请用 `fun f = alias(g)` 或类型声明。
        if (match(TokenType::Equal)) {
            if (match(TokenType::Default)) {
                f->is_default = true;
                expect_statement_end("expected end of statement");
                return f;
            }
            if (is_explicit_alias_form()) {
                auto target = consume(TokenType::Identifier, "expected alias target").lexeme;
                while (match(TokenType::Dot)) target += "." + consume(TokenType::Identifier, "expected name").lexeme;
                if (match(TokenType::Less)) {
                    int depth = 1;
                    while (depth > 0 && !is_at_end()) {
                        if (check(TokenType::Less)) depth++;
                        else if (check(TokenType::Greater)) depth--;
                        advance();
                    }
                }
                f->aliases.push_back(target);
                expect_statement_end("expected end of statement");
                return f;
            }
            // 表达式体
            auto ret = std::make_unique<ReturnStmt>();
            ret->value = parse_expression();
            auto block = std::make_unique<BlockStmt>();
            block->statements.push_back(std::move(ret));
            f->body = std::move(block);
            expect_statement_end("expected end of statement");
            return f;
        }
        if (match(TokenType::FatArrow)) {
            // `=>` 后允许换行：长表达式写在下一行是常见写法，
            // 否则解析器会在 `\n` 处报「expected expression」。
            consume_newlines();
            auto ret = std::make_unique<ReturnStmt>();
            ret->value = parse_expression();
            auto block = std::make_unique<BlockStmt>();
            block->statements.push_back(std::move(ret));
            f->body = std::move(block);
            expect_statement_end("expected end of statement");
            return f;
        }

        if (match(TokenType::LeftBrace)) { f->body = parse_block(); return f; }

        f->is_abstract = true;
        expect_statement_end("expected end of statement");
        return f;
    }

    void Parser::add_data_methods(ClassDecl &c) {
        // 泛型 data class 的 copy 需要写 Class<T>，当前泛型类型实参表达
        // 还没覆盖用户类，先只处理非泛型类；普通 equals/hashCode 仍由字段生成。
        auto has_method = [&](const std::string &name) {
            for (auto &m: c.members)
                if (auto *f = dynamic_cast<FunctionDecl *>(m.get()); f && f->name == name) return true;
            return false;
        };
        auto named = [](const std::string &n) {
            return std::make_unique<TypeAnnotation>(n);
        };
        auto self_field = [&](const std::string &name) -> ExprPtr {
            return std::make_unique<MemberAccessExpr>(
                std::make_unique<IdentifierExpr>("this"), name);
        };
        auto other_field = [&](const std::string &name) -> ExprPtr {
            return std::make_unique<MemberAccessExpr>(
                std::make_unique<IdentifierExpr>("other"), name);
        };
        auto ident = [](const std::string &n) -> ExprPtr {
            return std::make_unique<IdentifierExpr>(n);
        };
        auto literal_str = [](const std::string &s) -> ExprPtr {
            return std::make_unique<LiteralExpr>(LiteralExpr::String, "\"" + s + "\"");
        };
        auto literal_int = [](const std::string &s) -> ExprPtr {
            return std::make_unique<LiteralExpr>(LiteralExpr::Int, s);
        };
        auto bin = [](TokenType op, ExprPtr l, ExprPtr r) -> ExprPtr {
            return std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
        };
        auto call = [&](const std::string &name, std::vector<ExprPtr> args) -> ExprPtr {
            auto e = std::make_unique<CallExpr>();
            e->callee = std::make_unique<IdentifierExpr>(name);
            for (auto &a: args) e->args.emplace_back("", std::move(a));
            return e;
        };
        auto method = [&](const std::string &name, std::vector<Parameter> params,
                          std::unique_ptr<TypeAnnotation> ret, StmtPtr body,
                          const std::string &alias = {}) {
            auto f = std::make_unique<FunctionDecl>();
            f->name = name;
            f->params = std::move(params);
            f->return_type = std::move(ret);
            f->body = std::move(body);
            // Generated data-class members follow the class's own visibility.
            f->is_pub = c.is_pub;
            f->alias_name = alias;
            c.members.push_back(std::move(f));
        };
        auto block = [](std::vector<StmtPtr> stmts) {
            auto b = std::make_unique<BlockStmt>();
            b->statements = std::move(stmts);
            return StmtPtr(std::move(b));
        };
        auto ret = [](ExprPtr e) {
            auto r = std::make_unique<ReturnStmt>();
            r->value = std::move(e);
            return StmtPtr(std::move(r));
        };
        auto one = [&](StmtPtr st) {
            std::vector<StmtPtr> v;
            v.push_back(std::move(st));
            return block(std::move(v));
        };
        auto expr_stmt = [](ExprPtr e) {
            auto st = std::make_unique<ExprStmt>();
            st->expr = std::move(e);
            return StmtPtr(std::move(st));
        };

        const bool has_fields = !c.ctor_params.empty();
        const bool generic = !c.tparams.empty();

        if (!has_method("equals")) {
            Parameter other;
            other.name = "other";
            other.type = named(c.name);
            std::vector<StmtPtr> stmts;
            if (!has_fields) {
                stmts.push_back(ret(std::make_unique<LiteralExpr>(LiteralExpr::Bool, "true")));
            } else {
                ExprPtr cond;
                for (auto &p: c.ctor_params) {
                    ExprPtr eq = bin(TokenType::EqualEqual, self_field(p.name), other_field(p.name));
                    cond = cond ? bin(TokenType::DoubleAmpersand, std::move(cond), std::move(eq)) : std::move(eq);
                }
                stmts.push_back(ret(std::move(cond)));
            }
            method("equals", {std::move(other)}, named("bool"), block(std::move(stmts)));
        }

        if (!has_method("hashCode")) {
            std::vector<StmtPtr> stmts;
            auto h = std::make_unique<VarDeclStmt>();
            h->name = "h";
            h->type = named("i64");
            h->init = literal_int("17");
            stmts.push_back(std::move(h));
            for (auto &p: c.ctor_params) {
                ExprPtr fld = self_field(p.name);
                const std::string &tn = p.type ? p.type->name : "";
                ExprPtr hf;
                if (tn == "str" || tn == "String") {
                    auto m = std::make_unique<MemberAccessExpr>(self_field(p.name), "hashCode");
                    auto ce = std::make_unique<CallExpr>();
                    ce->callee = std::move(m);
                    hf = std::move(ce);
                } else if (tn == "i64" || tn == "i32" || tn == "i16" || tn == "i8" ||
                           tn == "u64" || tn == "u32" || tn == "u16" || tn == "u8" ||
                           tn == "f64" || tn == "f32" || tn == "bool" || tn == "char") {
                    hf = std::move(fld);
                } else {
                    hf = literal_int("0");
                }
                ExprPtr next = bin(TokenType::Plus,
                                   bin(TokenType::Star, ident("h"), literal_int("31")),
                                   std::move(hf));
                stmts.push_back(expr_stmt(
                    std::make_unique<AssignmentExpr>(ident("h"), std::move(next))));
            }
            stmts.push_back(ret(ident("h")));
            method("hashCode", {}, named("i64"), block(std::move(stmts)));
        }

        if (!has_method("toString")) {
            std::vector<StmtPtr> stmts;
            auto out = std::make_unique<VarDeclStmt>();
            out->name = "out";
            out->type = named("str");
            out->init = literal_str(c.name + "(");
            stmts.push_back(std::move(out));
            bool first = true;
            for (auto &p: c.ctor_params) {
                if (!first) {
                    stmts.push_back(expr_stmt(
                        std::make_unique<AssignmentExpr>(
                            ident("out"), bin(TokenType::Plus, ident("out"), literal_str(", ")))));
                }
                first = false;
                stmts.push_back(expr_stmt(
                    std::make_unique<AssignmentExpr>(
                        ident("out"),
                        bin(TokenType::Plus,
                            bin(TokenType::Plus, ident("out"), literal_str(p.name + "=")),
                            self_field(p.name)))));
            }
            stmts.push_back(expr_stmt(
                std::make_unique<AssignmentExpr>(
                    ident("out"), bin(TokenType::Plus, ident("out"), literal_str(")")))));
            stmts.push_back(ret(ident("out")));
            method("toString", {}, named("str"), block(std::move(stmts)), "to_string_impl");
        }

        if (!generic && !has_method("copy")) {
            // Kotlin copy：每个字段都有 `= self.field` 默认值；调用点缺省时由
            // codegen 在接收者上下文里求值（`p.copy()` / `p.copy(2)`）。
            std::vector<Parameter> params;
            std::vector<ExprPtr> args;
            for (auto &p: c.ctor_params) {
                Parameter cp;
                cp.name = p.name;
                cp.type = clone_type(p.type.get());
                cp.default_value = self_field(p.name);
                params.push_back(std::move(cp));
                args.push_back(ident(p.name));
            }
            method("copy", std::move(params), named(c.name),
                   one(ret(call(c.name, std::move(args)))), "copy_impl");
        }
        if (!generic && !has_method("copyWith")) {
            // 局部修改用 copyWith：Sinter 的默认参数在调用点求值，写不出
            // `copy(x = self.x)` 这种「默认值引用接收者」的语义。
            std::vector<Parameter> params;
            std::vector<ExprPtr> args;
            for (auto &p: c.ctor_params) {
                Parameter cp;
                cp.name = p.name;
                cp.type = clone_type(p.type.get());
                params.push_back(std::move(cp));
                args.push_back(ident(p.name));
            }
            method("copyWith", std::move(params), named(c.name),
                   one(ret(call(c.name, std::move(args)))));
        }

        if (!generic) {
            for (size_t i = 0; i < c.ctor_params.size(); ++i) {
                const std::string name = "component" + std::to_string(i + 1);
                if (has_method(name)) continue;
                method(name, {}, clone_type(c.ctor_params[i].type.get()),
                       one(ret(self_field(c.ctor_params[i].name))));
            }
        }
    }

    StmtPtr Parser::parse_class_like(std::vector<Annotation> anns, bool is_struct, bool is_pub, bool is_data) {
        auto c = std::make_unique<ClassDecl>();
        c->annotations = std::move(anns);
        c->is_struct = is_struct;
        c->is_pub = is_pub || in_pub_block;
        c->name = consume(TokenType::Identifier, "expected type name").lexeme;

        // 泛型参数：class Box<T> / struct Pair<A, B must number> / class Tup<T, Args...>
        if (match(TokenType::Less)) {
            do {
                if (check(TokenType::Greater)) break;   // 尾随逗号：class P<T,>
                sinter::TemplateParam tp;
                tp.name = consume(TokenType::Identifier, "expected generic parameter").lexeme;
                if (match(TokenType::Must)) tp.constraint = parse_type();
                if (match(TokenType::DotDotDot)) tp.is_pack = true;
                c->tparams.push_back(std::move(tp));
            } while (match(TokenType::Comma));
            consume(TokenType::Greater, "expected '>'");
            validate_tparams(c->tparams);
            // 类体里的字段/方法都可能出现 `Args[i]`：用守卫对象覆盖整个类体解析
            for (auto &tp: c->tparams)
                if (tp.is_pack) active_pack_names_.push_back(tp.name);
            pack_scope_.emplace(active_pack_names_, active_pack_names_.size());
        }

        if (match(TokenType::LeftParen)) {
            consume_newlines();
            c->ctor_params = parse_parameters();
            consume_newlines();
            consume(TokenType::RightParen, "expected ')'");
        }

        // 继承 : Base(args), Interface
        if (match(TokenType::Colon) || match(TokenType::Extends)) {
            bool more = true;
            while (more) {
                std::string base = consume(TokenType::Identifier, "expected base class name").lexeme;
                while (match(TokenType::Dot))
                    base += "." + consume(TokenType::Identifier, "expected base class name").lexeme;
                // 泛型基类：`class IntArray : Array<i64>`。实参在这里先按源码
                // 原文拼进名字（`Array<i64>`），语义层登记基类时再拆成模板短名
                // 并把实参编码进去，单态化阶段据此补一条实例化需求。
                // 只支持类型实参（不支持 `Array<T>` 里的嵌套实例化表达式）。
                if (match(TokenType::Less)) {
                    base += "<";
                    int depth = 1;
                    while (depth > 0 && !is_at_end()) {
                        const Token &t = advance();
                        if (t.type == TokenType::Less) ++depth;
                        else if (t.type == TokenType::Greater) { --depth; if (depth == 0) break; }
                        if (t.type == TokenType::Comma) {
                            base += ",";
                            continue;
                        }
                        base += t.lexeme;
                    }
                    base += ">";
                }
                c->bases.push_back(base);
                std::vector<ExprPtr> args;
                if (match(TokenType::LeftParen)) {
                    consume_newlines();
                    if (!check(TokenType::RightParen)) {
                        do {
                            if (check(TokenType::Identifier) && peek_next().type == TokenType::Equal) {
                                advance(); advance();
                            }
                            args.push_back(parse_expression());
                        } while (match(TokenType::Comma));
                    }
                    consume_newlines();
                    consume(TokenType::RightParen, "expected ')'");
                }
                c->base_args.push_back(std::move(args));
                more = match(TokenType::Comma);
                if (!more && match(TokenType::Implements)) more = true;
            }
        } else if (match(TokenType::Implements)) {
            do {
                std::string base = consume(TokenType::Identifier, "expected interface name").lexeme;
                c->bases.push_back(base);
                c->base_args.emplace_back();
            } while (match(TokenType::Comma));
        }

        consume_newlines();
        if (!check(TokenType::LeftBrace) && (!c->ctor_params.empty() || is_data)) {
            // Kotlin 风格：带主构造参数的 `data class Point(...)` / `struct Point(...)`
            // 可以省略空类体。
            c->is_data = is_data;
            if (is_data) add_data_methods(*c);
            return c;
        }
        consume(TokenType::LeftBrace, "expected '{'");
        consume_newlines();

        const bool saved_in_class = in_class_body;
        in_class_body = true;

        while (!check(TokenType::RightBrace) && !is_at_end()) {
            skip_all_newlines_and_semis();
            if (check(TokenType::RightBrace)) break;
            if (getenv("SINTER_DEBUG_CLASS")) {
                std::cerr << "[class body] lexeme='" << peek().lexeme << "' type="
                          << static_cast<int>(peek().type) << "\n";
            }

            // 析构 ~T()
            if (check(TokenType::Tilde)) {
                advance();
                c->destructor = parse_destructor();
                consume_newlines();
                continue;
            }

            // 构造函数 / 复制构造 / 移动构造 / 析构器（同名）统一处理
            if (check(TokenType::Identifier) && peek().lexeme == c->name
                && peek_next().type == TokenType::LeftParen) {
                const auto save = current;
                advance(); // Name
                advance(); // (

                bool is_copy = false, is_move = false;
                if (check(TokenType::Const)) advance();
                if (check(TokenType::Identifier) && peek().lexeme == c->name) advance();
                if (check(TokenType::Ampersand)) { advance(); is_copy = true; }
                else if (check(TokenType::DoubleAmpersand)) { advance(); is_move = true; }

                if (is_copy || is_move) {
                    std::string pname = "other";
                    if (check(TokenType::Identifier)) pname = advance().lexeme;
                    if (check(TokenType::RightParen)) {
                        advance(); // )
                        if (match(TokenType::Colon)) {
                            bool more = true;
                            while (more) {
                                consume(TokenType::Identifier, "expected base class name");
                                std::vector<ExprPtr> args;
                                if (match(TokenType::LeftParen)) {
                                    if (!check(TokenType::RightParen)) {
                                        do { args.push_back(parse_expression()); }
                                        while (match(TokenType::Comma));
                                    }
                                    consume(TokenType::RightParen, "expected ')'");
                                }
                                if (c->base_args.empty()) c->base_args.push_back(std::move(args));
                                more = match(TokenType::Comma);
                            }
                        }
                        auto f = std::make_unique<FunctionDecl>();
                        f->name = c->name;
                        Parameter prm;
                        prm.name = pname;
                        prm.type = std::make_unique<TypeAnnotation>(c->name);
                        if (is_copy) prm.type->is_const_ref = true;
                        else prm.type->is_rvalue_ref = true;
                        f->params.push_back(std::move(prm));
                        if (match(TokenType::Equal) && match(TokenType::Default)) {
                            f->is_default = true;
                        } else if (match(TokenType::LeftBrace)) {
                            f->body = parse_block();
                        } else {
                            f->is_default = true;
                            expect_statement_end("expected end of statement");
                        }
                        if (is_copy) c->copy_ctor = std::move(f);
                        else c->move_ctor = std::move(f);
                        // 规范化：用到的类名尽量按调用者书写解析（保持原名）
                        consume_newlines();
                        continue;
                    }
                    current = save;
                } else {
                    current = save;
                    // 普通构造：T() = default / T() { ... } / T(args) = default
                    advance(); // Name
                    advance(); // (
                    auto f = std::make_unique<FunctionDecl>();
                    f->name = c->name;
                    consume_newlines();
                    if (!check(TokenType::RightParen)) {
                        f->params = parse_parameters();
                        apply_variadic(*f);
                        // 同名构造函数既是「构造实现」也是「构造参数声明」，
                        // 二者取其一即可；这里同步到 ctor_params，保证生成 _new 的形参完整。
                        if (c->ctor_params.empty()) c->ctor_params = f->params;
                    }
                    consume_newlines();
                    consume(TokenType::RightParen, "expected ')'");
                    // 构造初始化列表：Name(params) : Base(args), Other(args)
                    if (getenv("SINTER_DEBUG_CLASS"))
                        std::cerr << "[ctor] init-list check peek='" << peek().lexeme
                                  << "' type=" << static_cast<int>(peek().type)
                                  << " (Colon=" << static_cast<int>(TokenType::Colon) << ")\n";
                    if (match(TokenType::Colon)) {
                        bool more = true;
                        while (more) {
                            // 允许 `Base` / `mod.Base` 形式（最后一段才是类名）
                            consume(TokenType::Identifier, "expected base class name");
                            while (match(TokenType::Dot))
                                consume(TokenType::Identifier, "expected base class name");
                            // 泛型基类的实参（`... : Array<i64>(d, n, true)`）：
                            // 类型实参在类头已经解析并编码进 `c->bases`，这里的
                            // 初始化列表只需要跳过 `<...>`，剩下的括号才是实参。
                            if (match(TokenType::Less)) {
                                int depth = 1;
                                while (depth > 0 && !is_at_end()) {
                                    const Token &t = advance();
                                    if (t.type == TokenType::Less) ++depth;
                                    else if (t.type == TokenType::Greater) --depth;
                                }
                            }
                            std::vector<ExprPtr> args;
                            if (match(TokenType::LeftParen)) {
                                consume_newlines();
                                if (!check(TokenType::RightParen)) {
                                    do {
                                        if (check(TokenType::Identifier) &&
                                            peek_next().type == TokenType::Equal) {
                                            advance(); advance();
                                        }
                                        args.push_back(parse_expression());
                                    } while (match(TokenType::Comma));
                                }
                                consume_newlines();
                                consume(TokenType::RightParen, "expected ')'");
                            }
                            c->base_args.clear();   // 构造初始化列表覆盖 super 调用
                            c->base_args.push_back(std::move(args));
                            more = match(TokenType::Comma);
                        }
                    }
                    if (getenv("SINTER_DEBUG_CLASS")) {
                        std::cerr << "[ctor] after ) peek='" << peek().lexeme
                                  << "' type=" << static_cast<int>(peek().type) << "\n";
                    }
                    if (match(TokenType::Equal)) {
                        if (getenv("SINTER_DEBUG_CLASS")) std::cerr << "[ctor] matched Equal\n";
                        if (match(TokenType::Default)) f->is_default = true;
                        else error("constructor supports only '= default' or a body");
                        expect_statement_end("expected end of statement");
                    } else if (match(TokenType::LeftBrace)) {
                        f->body = parse_block();
                    } else {
                        f->is_default = true;
                        expect_statement_end("expected end of statement");
                    }
                    // 无参构造且已由 ctor_params 表达时不重复登记
                    if (!f->params.empty() || f->body) {
                        c->members.push_back(std::move(f));
                    }
                    consume_newlines();
                    continue;
                }
            }

            auto m = parse_declaration();
            if (m) c->members.push_back(std::move(m));
            consume_newlines();
        }

        in_class_body = saved_in_class;
        consume(TokenType::RightBrace, "expected '}'");
        // Flatten `pub { ... }` / `priv { ... }` visibility blocks inside a
        // class body: the inner declarations already carry the right is_pub flag.
        {
            std::vector<StmtPtr> flat;
            for (auto &m: c->members) {
                if (auto *pb = dynamic_cast<PubBlockStmt *>(m.get())) {
                    for (auto &inner: pb->statements) flat.push_back(std::move(inner));
                } else {
                    flat.push_back(std::move(m));
                }
            }
            c->members = std::move(flat);
        }
        if (c->is_data) add_data_methods(*c);
        return c;
    }

    StmtPtr Parser::parse_destructor() {
        auto f = std::make_unique<FunctionDecl>();
        f->name = "~" + consume(TokenType::Identifier, "expected class name").lexeme;
        consume(TokenType::LeftParen, "expected '('");
        consume(TokenType::RightParen, "expected ')'");
        if (match(TokenType::Equal) && match(TokenType::Default)) {
            f->is_default = true;
            expect_statement_end("expected end of statement");
        } else if (match(TokenType::LeftBrace)) {
            f->body = parse_block();
        } else {
            expect_statement_end("expected end of statement");
        }
        return f;
    }

    StmtPtr Parser::parse_interface(std::vector<Annotation> anns, bool is_pub) {
        auto i = std::make_unique<InterfaceDecl>();
        i->annotations = std::move(anns);
        i->is_pub = is_pub || in_pub_block;
        i->name = consume(TokenType::Identifier, "expected interface name").lexeme;
        if (match(TokenType::Colon) || match(TokenType::Extends)) {
            do { i->bases.push_back(consume(TokenType::Identifier, "expected interface name").lexeme); }
            while (match(TokenType::Comma));
        }
        consume_newlines();
        consume(TokenType::LeftBrace, "expected '{'");
        consume_newlines();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            skip_all_newlines_and_semis();
            if (check(TokenType::RightBrace)) break;
            i->methods.push_back(parse_declaration());
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        return i;
    }

    StmtPtr Parser::parse_enum(std::vector<Annotation> anns, bool is_pub) {
        auto e = std::make_unique<EnumDecl>();
        e->annotations = std::move(anns);
        e->is_pub = is_pub || in_pub_block;
        e->name = consume(TokenType::Identifier, "expected enum name").lexeme;
        consume_newlines();
        consume(TokenType::LeftBrace, "expected '{'");
        consume_newlines();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            skip_all_newlines_and_semis();
            if (check(TokenType::RightBrace)) break;
            e->variants.push_back(consume(TokenType::Identifier, "expected variant name").lexeme);
            std::string val;
            if (match(TokenType::Equal)) {
                if (check(TokenType::IntegerLiteral)) val = advance().lexeme;
            }
            e->explicit_vals.push_back(val);
            match(TokenType::Comma);
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        return e;
    }

    StmtPtr Parser::parse_const() { return parse_var_decl(true, false, false); }

    StmtPtr Parser::parse_var_decl(bool is_const, bool is_static, bool is_pub) {
        auto v = std::make_unique<VarDeclStmt>();
        v->is_mutable = (previous().type == TokenType::Var);
        v->is_const = is_const;
        v->is_static = is_static;
        v->is_pub = is_pub || in_pub_block;
        v->name = consume(TokenType::Identifier, "expected variable name").lexeme;
        if (match(TokenType::Colon)) v->type = parse_type();
        if (match(TokenType::Equal)) v->init = parse_expression();
        else if (is_const) error("const declaration requires an initializer");
        expect_statement_end("expected end of statement");
        return v;
    }

    StmtPtr Parser::parse_destructure(bool is_mutable) {
        auto d = std::make_unique<DestructureStmt>();
        d->is_mutable = is_mutable;
        while (!check(TokenType::RightParen) && !is_at_end()) {
            DestructureStmt::Target t;
            t.mode = is_mutable ? DestructureStmt::Target::Var : DestructureStmt::Target::Val;
            if (check(TokenType::Identifier) && peek().lexeme == "_") {
                advance();
                t.mode = DestructureStmt::Target::Ignore;
            } else {
                t.name = consume(TokenType::Identifier, "expected binding name").lexeme;
                if (match(TokenType::Colon)) t.type = parse_type();
            }
            d->targets.push_back(std::move(t));
            if (!match(TokenType::Comma)) break;
        }
        consume(TokenType::RightParen, "expected ')'");
        consume(TokenType::Equal, "destructuring requires an initializer");
        d->init = parse_expression();
        expect_statement_end("expected end of statement");
        return d;
    }

    StmtPtr Parser::parse_alias(bool is_pub) {
        auto a = std::make_unique<AliasStmt>();
        a->is_pub = is_pub || in_pub_block;
        a->name = consume(TokenType::Identifier, "expected alias target").lexeme;
        consume(TokenType::Equal, "expected '='");
        a->type = parse_type();
        expect_statement_end("expected end of statement");
        return a;
    }

    StmtPtr Parser::parse_comptime(std::vector<Annotation> anns) {
        auto c = std::make_unique<ComptimeStmt>();
        c->annotations = std::move(anns);
        // 形式一：comptime if <cond> { ... } else { ... } / else if ...
        // 直接复用普通 if 的解析，把结果当作 comptime 体交给编译期裁剪。
        if (match(TokenType::If)) {
            c->body = parse_if_stmt();
            return c;
        }
        // 形式二：comptime assert(<cond>[, "msg"]) —— 静态断言
        // assert 不是关键字（避免占掉一个常用标识符），只在 comptime 之后按
        // 上下文识别；这样 `val assert = 1` 仍然合法。
        if (check(TokenType::Identifier) && peek().lexeme == "assert") {
            const Token kw = advance();
            auto a = std::make_unique<ComptimeAssertStmt>();
            a->line = static_cast<int>(kw.line);
            a->col = static_cast<int>(kw.column);
            consume(TokenType::LeftParen, "comptime assert requires its condition in '(' ')'");
            a->cond = parse_expression();
            if (match(TokenType::Comma)) a->message = parse_expression();
            consume(TokenType::RightParen, "expected ')'");
            expect_statement_end("expected end of statement");
            c->body = std::move(a);
            return c;
        }
        // 形式三：comptime { ... }
        consume(TokenType::LeftBrace, "expected '{', 'if' or 'assert'");
        c->body = parse_block();
        return c;
    }

    StmtPtr Parser::parse_bind_c() {
        consume(TokenType::Identifier, "expected 'C'");
        // `bind C header "x.h"`：编译期解析 C 头，自动登记函数原型
        if (check(TokenType::Identifier) && peek().lexeme == "header") {
            const auto kw = advance();
            if (!check(TokenType::StringLiteral))
                error("expected a quoted header path after 'bind C header'");
            const auto lit = advance();
            std::string path = lit.lexeme;
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                path = path.substr(1, path.size() - 2);
            if (path.empty()) error("header path cannot be empty");
            auto h = std::make_unique<CBindHeaderStmt>();
            h->header = path;
            h->line = static_cast<int>(kw.line);
            h->col = static_cast<int>(kw.column);
            expect_statement_end("expected end of statement");
            return h;
        }
        // `bind C struct X`：声明不透明 C 结构体
        if (check(TokenType::Identifier) && peek().lexeme == "struct") {
            advance();
            const auto n = consume(TokenType::Identifier, "expected a C struct name after 'bind C struct'");
            auto s = std::make_unique<CStructDeclStmt>();
            s->name = n.lexeme;
            s->line = static_cast<int>(n.line);
            s->col = static_cast<int>(n.column);
            expect_statement_end("expected end of statement");
            return s;
        }
        auto b = std::make_unique<BindCFunctionStmt>();
        consume(TokenType::Fun, "expected 'function', 'header' or 'struct'");
        b->c_name = consume(TokenType::Identifier, "expected C function name").lexeme;
        consume(TokenType::LeftParen, "expected '('");
        if (!check(TokenType::RightParen)) {
            b->params = parse_parameters();
            if (check(TokenType::DotDotDot)) { advance(); b->variadic = true; }
        }
        consume(TokenType::RightParen, "expected ')'");
        if (match(TokenType::Colon) || match(TokenType::Arrow)) b->return_type = parse_type();
        expect_statement_end("expected end of statement");
        return b;
    }

    StmtPtr Parser::parse_embed_c(std::vector<Annotation> anns) {
        auto e = std::make_unique<EmbedCStmt>();
        e->annotations = std::move(anns);
        if (check(TokenType::Identifier)) advance(); // lexer 已消费 C
        consume(TokenType::LeftBrace, "expected '{'");
        e->code = consume(TokenType::EmbedCBody, "expected C code block").lexeme;
        // 词法器用 '\x02' 前缀标记「同时公开到头文件」的块。
        if (!e->code.empty() && e->code.front() == '\x02') {
            e->code.erase(e->code.begin());
            e->in_header = true;
        }
        consume(TokenType::RightBrace, "expected '}'");
        expect_statement_end("expected end of statement");
        return e;
    }

    StmtPtr Parser::parse_pub_block(std::vector<Annotation> anns) {
        auto blk = std::make_unique<PubBlockStmt>();
        blk->annotations = std::move(anns);
        consume_newlines();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (auto s = parse_declaration()) blk->statements.push_back(std::move(s));
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        return blk;
    }

    StmtPtr Parser::parse_no_raii_block() {
        auto b = std::make_unique<BlockStmt>();
        b->no_raii = true;
        const bool saved = no_raii_context;
        no_raii_context = true;
        consume_newlines();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (auto s = parse_declaration()) b->statements.push_back(std::move(s));
            consume_newlines();
        }
        no_raii_context = saved;
        consume(TokenType::RightBrace, "expected '}'");
        return b;
    }

    StmtPtr Parser::parse_return() {
        auto r = std::make_unique<ReturnStmt>();
        if (!check(TokenType::Semicolon) && !check(TokenType::Newline)
            && !check(TokenType::RightBrace) && !is_at_end())
            r->value = parse_expression();
        expect_statement_end("expected end of statement");
        return r;
    }

    StmtPtr Parser::parse_throw() {
        auto t = std::make_unique<ThrowStmt>();
        t->value = parse_expression();
        expect_statement_end("expected end of statement");
        return t;
    }

    StmtPtr Parser::parse_try() {
        auto t = std::make_unique<TryStmt>();
        consume_newlines();
        consume(TokenType::LeftBrace, "expected '{' after 'try'");
        t->body = parse_block();
        consume_newlines();
        consume(TokenType::Catch, "expected 'catch' after try block");
        if (match(TokenType::LeftParen)) {
            t->error_name = consume(TokenType::Identifier, "expected catch variable name").lexeme;
            if (match(TokenType::Colon)) {
                auto ty = parse_type();
                if (ty && ty->kind == TypeAnnotation::Named && ty->name != "str" && ty->name != "cstr")
                    error("catch variables are strings; use `catch (" + t->error_name + ": str)`");
            }
            consume(TokenType::RightParen, "expected ')'");
        }
        consume_newlines();
        consume(TokenType::LeftBrace, "expected '{' after catch");
        t->catch_body = parse_block();
        consume_newlines();
        if (match(TokenType::Finally)) {
            consume_newlines();
            consume(TokenType::LeftBrace, "expected '{' after finally");
            t->finally_body = parse_block();
        }
        return t;
    }

    StmtPtr Parser::parse_if_stmt() {
        auto i = std::make_unique<IfStmt>();
        // 括号可选：`if (x) {}` 与 `if x {}` 都接受（标准库两种风格都有）。
        const bool paren = match(TokenType::LeftParen);
        ++suppress_trailing_lambda_;
        i->cond = parse_expression();
        --suppress_trailing_lambda_;
        if (paren) consume(TokenType::RightParen, "expected ')'");
        consume_newlines();
        if (match(TokenType::LeftBrace)) i->then_b = parse_block();
        else i->then_b = parse_declaration();
        consume_newlines();
        if (match(TokenType::Else)) {
            consume_newlines();
            if (match(TokenType::If)) i->else_b = parse_if_stmt();
            else if (match(TokenType::LeftBrace)) i->else_b = parse_block();
            else i->else_b = parse_declaration();
        }
        return i;
    }

    StmtPtr Parser::parse_while() {
        auto w = std::make_unique<WhileStmt>();
        const bool paren = match(TokenType::LeftParen);
        ++suppress_trailing_lambda_;
        w->cond = parse_expression();
        --suppress_trailing_lambda_;
        if (paren) consume(TokenType::RightParen, "expected ')'");
        consume_newlines();
        if (match(TokenType::LeftBrace)) w->body = parse_block();
        else w->body = parse_declaration();
        return w;
    }

    StmtPtr Parser::parse_do_while() {
        auto d = std::make_unique<DoWhileStmt>();
        consume_newlines();
        if (match(TokenType::LeftBrace)) d->body = parse_block();
        else d->body = parse_declaration();
        consume_newlines();
        consume(TokenType::While, "expected 'while'");
        consume(TokenType::LeftParen, "expected '('");
        ++suppress_trailing_lambda_;
        d->cond = parse_expression();
        --suppress_trailing_lambda_;
        consume(TokenType::RightParen, "expected ')'");
        expect_statement_end("expected end of statement");
        return d;
    }

    StmtPtr Parser::parse_for() {
        auto f = std::make_unique<ForStmt>();
        consume(TokenType::LeftParen, "for header must be wrapped in '(' ')'");
        if (match(TokenType::LeftParen)) {
            do { f->variables.push_back(consume(TokenType::Identifier, "expected loop variable").lexeme); }
            while (match(TokenType::Comma));
            consume(TokenType::RightParen, "expected ')'");
        } else {
            f->variables.push_back(consume(TokenType::Identifier, "expected loop variable").lexeme);
        }
        consume(TokenType::In, "expected 'in'");
        ++suppress_trailing_lambda_;
        f->iterable = parse_expression();
        --suppress_trailing_lambda_;
        consume(TokenType::RightParen, "expected ')'");
        consume_newlines();
        if (match(TokenType::LeftBrace)) f->body = parse_block();
        else f->body = parse_declaration();
        return f;
    }

    StmtPtr Parser::parse_break() {
        auto b = std::make_unique<BreakStmt>();
        expect_statement_end("expected end of statement");
        return b;
    }

    StmtPtr Parser::parse_continue() {
        auto c = std::make_unique<ContinueStmt>();
        expect_statement_end("expected end of statement");
        return c;
    }

    StmtPtr Parser::parse_del() {
        auto d = std::make_unique<DelStmt>();
        d->target = parse_expression();
        expect_statement_end("expected end of statement");
        return d;
    }

    StmtPtr Parser::parse_block() {
        auto b = std::make_unique<BlockStmt>();
        b->no_raii = no_raii_context;
        consume_newlines();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (auto s = parse_declaration()) b->statements.push_back(std::move(s));
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        return b;
    }

    StmtPtr Parser::parse_expr_stmt() {
        auto e = std::make_unique<ExprStmt>();
        e->expr = parse_expression();
        expect_statement_end("expected end of statement");
        return e;
    }

    // ==========================================================================
    // 表达式
    // ==========================================================================
    ExprPtr Parser::parse_expression() {
        // 记录表达式起点，供类型检查与诊断定位到源码行。
        // 若下层已经填过更精确的位置（例如 parse_primary 里的字面量），
        // 这里不覆盖。
        const Token start = peek();
        auto e = parse_assignment();
        while (match(TokenType::As)) {
            auto ty = parse_type();
            auto cast = std::make_unique<CastExpr>(std::move(e), std::move(ty));
            cast->line = static_cast<int>(start.line);
            cast->col = static_cast<int>(start.column);
            e = std::move(cast);
        }
        if (e && e->line == 0) {
            e->line = static_cast<int>(start.line);
            e->col = static_cast<int>(start.column);
        }
        return e;
    }

    ExprPtr Parser::parse_assignment() {
        auto e = parse_pipeline();
        if (match(TokenType::Equal)) {
            auto v = parse_assignment();
            auto a = std::make_unique<AssignmentExpr>(std::move(e), std::move(v));
            a->op = TokenType::Equal;
            return a;
        }
        if (match_any({TokenType::PlusEqual, TokenType::MinusEqual,
                       TokenType::StarEqual, TokenType::SlashEqual, TokenType::PercentEqual})) {
            auto op = previous().type;
            auto v = parse_assignment();
            TokenType bop = (op == TokenType::PlusEqual) ? TokenType::Plus
                            : (op == TokenType::MinusEqual) ? TokenType::Minus
                            : (op == TokenType::StarEqual) ? TokenType::Star
                            : (op == TokenType::SlashEqual) ? TokenType::Slash
                                                            : TokenType::Percent;
            auto lc = clone_expr(e.get());
            auto binary = std::make_unique<BinaryExpr>(bop, std::move(lc), std::move(v));
            auto a = std::make_unique<AssignmentExpr>(std::move(e), std::move(binary));
            a->op = TokenType::Equal;
            return a;
        }
        return e;
    }

    ExprPtr Parser::parse_pipeline() {
        auto e = parse_ternary();
        while (match(TokenType::PipeGt)) {
            consume_newlines();
            auto right = parse_ternary();
            e = std::make_unique<PipelineExpr>(std::move(e), std::move(right));
        }
        return e;
    }

    ExprPtr Parser::parse_ternary() {
        auto e = parse_range();
        if (match(TokenType::Elvis) || match(TokenType::QuestionQuestion)) {
            auto f = parse_ternary();
            auto t = std::make_unique<TernaryExpr>(clone_expr(e.get()), std::move(e), std::move(f));
            t->is_elvis = true;
            return t;
        }
        if (match(TokenType::Question)) {
            auto t = parse_expression();
            consume(TokenType::Colon, "expected ':'");
            auto f = parse_ternary();
            auto node = std::make_unique<TernaryExpr>(std::move(e), std::move(t), std::move(f));
            node->is_elvis = false;
            return node;
        }
        return e;
    }

    ExprPtr Parser::parse_range() {
        auto e = parse_logical_or();
        if (match(TokenType::DoubleDot)) {
            auto r = parse_logical_or();
            return std::make_unique<RangeExpr>(std::move(e), std::move(r), true);
        }
        if (match(TokenType::RangeOpen)) {
            auto r = parse_logical_or();
            return std::make_unique<RangeExpr>(std::move(e), std::move(r), false);
        }
        return e;
    }

    ExprPtr Parser::parse_logical_or() {
        auto e = parse_logical_and();
        while (match(TokenType::DoublePipe)) {
            auto r = parse_logical_and();
            e = std::make_unique<BinaryExpr>(TokenType::DoublePipe, std::move(e), std::move(r));
        }
        return e;
    }

    ExprPtr Parser::parse_logical_and() {
        auto e = parse_equality();
        while (match(TokenType::DoubleAmpersand)) {
            auto r = parse_equality();
            e = std::make_unique<BinaryExpr>(TokenType::DoubleAmpersand, std::move(e), std::move(r));
        }
        return e;
    }

    ExprPtr Parser::parse_equality() {
        auto e = parse_comparison();
        while (match_any({TokenType::EqualEqual, TokenType::NotEqual})) {
            auto op = previous().type;
            auto r = parse_comparison();
            e = std::make_unique<BinaryExpr>(op, std::move(e), std::move(r));
        }
        return e;
    }

    ExprPtr Parser::parse_comparison() {
        auto e = parse_term();
        while (match_any({TokenType::Less, TokenType::LessEqual,
                          TokenType::Greater, TokenType::GreaterEqual})) {
            auto op = previous().type;
            auto r = parse_term();
            e = std::make_unique<BinaryExpr>(op, std::move(e), std::move(r));
        }
        return e;
    }

    ExprPtr Parser::parse_term() {
        auto e = parse_factor();
        while (match_any({TokenType::Plus, TokenType::Minus})) {
            auto op = previous().type;
            auto r = parse_factor();
            e = std::make_unique<BinaryExpr>(op, std::move(e), std::move(r));
        }
        return e;
    }

    ExprPtr Parser::parse_factor() {
        auto e = parse_unary();
        while (match_any({TokenType::Star, TokenType::Slash, TokenType::Percent})) {
            auto op = previous().type;
            auto r = parse_unary();
            e = std::make_unique<BinaryExpr>(op, std::move(e), std::move(r));
        }
        return e;
    }

    ExprPtr Parser::parse_unary() {
        // `*p` 解引用、`&x` 取地址：与 C 一致的一元运算符。
        if (match_any({TokenType::Minus, TokenType::Plus, TokenType::Bang,
                       TokenType::Star, TokenType::Ampersand})) {
            auto op = previous().type;
            auto r = parse_unary();
            return std::make_unique<UnaryExpr>(op, std::move(r));
        }
        return parse_call();
    }

    ExprPtr Parser::parse_call() {
        auto e = parse_primary();
        // 显式泛型调用：f<i32>(x) / math.square<i64>(x)
        std::vector<std::unique_ptr<TypeAnnotation> > pending_type_args;
        // 显式类型实参：f<T>(...) / obj.method<T>(...) / Mod.fn<T>(...)
        if ((is_identifier_expr(e.get()) || dynamic_cast<MemberAccessExpr *>(e.get())) &&
            looks_like_type_arg_list()) {
            consume(TokenType::Less, "expected '<'");
            do { pending_type_args.push_back(parse_type()); } while (match(TokenType::Comma));
            consume(TokenType::Greater, "expected '>'");
        }
        while (true) {
            if (match(TokenType::BangBang)) {
                e = std::make_unique<NonNullExpr>(std::move(e));
                continue;
            }
            // Kotlin 风格尾随 lambda：`button { ... }` / `column(1) { ... }`
            // 只在同一行时启用，避免把下一行的普通块语句吞成 lambda。
            if (suppress_trailing_lambda_ == 0 && check(TokenType::LeftBrace) &&
                previous().line == peek().line &&
                (is_identifier_expr(e.get()) || dynamic_cast<MemberAccessExpr *>(e.get()) ||
                 dynamic_cast<CallExpr *>(e.get()))) {
                auto lam = parse_block_lambda();
                if (auto *call = dynamic_cast<CallExpr *>(e.get())) {
                    call->args.emplace_back("", std::move(lam));
                } else {
                    auto call2 = std::make_unique<CallExpr>();
                    call2->callee = std::move(e);
                    call2->args.emplace_back("", std::move(lam));
                    e = std::move(call2);
                }
                continue;
            }
            if (match(TokenType::LeftParen)) {
                auto call = std::make_unique<CallExpr>();
                call->callee = std::move(e);
                for (auto &ta: pending_type_args) call->type_args.push_back(std::move(ta));
                pending_type_args.clear();
                consume_newlines();
                if (!check(TokenType::RightParen)) {
                    do {
                        consume_newlines();
                        if (check(TokenType::RightParen)) break;
                        // 命名参数 name = value
                        if (check(TokenType::Identifier) && peek_next().type == TokenType::Equal) {
                            std::string n = advance().lexeme;
                            advance(); // =
                            call->args.emplace_back(n, parse_expression());
                        } else {
                            call->args.emplace_back("", parse_expression());
                        }
                        consume_newlines();
                    } while (match(TokenType::Comma));
                }
                consume_newlines();
                consume(TokenType::RightParen, "expected ')'");
                e = std::move(call);
                // 块式作用域函数 with(recv) { ... }
                if (auto *ci = dynamic_cast<CallExpr *>(e.get());
                    ci && ci->args.size() == 1 &&
                    dynamic_cast<IdentifierExpr *>(ci->callee.get()) &&
                    dynamic_cast<IdentifierExpr *>(ci->callee.get())->name == "with" &&
                    check(TokenType::LeftBrace)) {
                    auto sc = std::make_unique<ScopeCallExpr>();
                    sc->kind_sc = ScopeCallExpr::With;
                    sc->member_name = "with";
                    sc->receiver = std::move(ci->args[0].value);
                    consume(TokenType::LeftBrace, "expected '{'");
                    consume_newlines();
                    if (check(TokenType::Identifier) &&
                        (peek_next().type == TokenType::Arrow ||
                         peek_next().type == TokenType::FatArrow)) {
                        sc->it_name = advance().lexeme;
                        advance();
                    }
                    auto blk = std::make_unique<BlockStmt>();
                    while (!check(TokenType::RightBrace) && !is_at_end()) {
                        if (auto st = parse_declaration()) blk->statements.push_back(std::move(st));
                        consume_newlines();
                    }
                    consume(TokenType::RightBrace, "expected '}'");
                    sc->body = std::move(blk);
                    e = std::move(sc);
                }
            } else if (match(TokenType::Dot) || match(TokenType::QuestionDot)) {
                const bool null_safe = previous().type == TokenType::QuestionDot;
                // 成员名允许是关键字（s.del() / s.copy() / s.move() / s.type ...），
                // 只排除标点与运算符 token。
                std::string nm;
                {
                    const auto tt = static_cast<int>(peek().type);
                    const bool is_punct = tt <= static_cast<int>(TokenType::DoubleColon);
                    if (!is_punct) nm = advance().lexeme;
                    else nm = consume(TokenType::Identifier, "expected member name").lexeme;
                }
                auto m = std::make_unique<MemberAccessExpr>(std::move(e), nm);
                m->null_safe = null_safe;
                e = std::move(m);
                // 作用域函数：let/run/apply/also
                if (nm == "let" || nm == "run" || nm == "apply" || nm == "also") {
                    if (check(TokenType::LeftBrace)) {
                        e = parse_scope_call(std::move(e));
                    }
                }
                // 泛型方法调用：obj.method<T>(...) / Class.method<T>(...)
                // 先吃掉 <...>，下一次循环的 '(' 分支会用 pending_type_args 建 CallExpr。
                if (looks_like_type_arg_list()) {
                    consume(TokenType::Less, "expected '<'");
                    do { pending_type_args.push_back(parse_type()); } while (match(TokenType::Comma));
                    consume(TokenType::Greater, "expected '>'");
                }
            } else if (match(TokenType::LeftBracket)) {
                auto i = parse_expression();
                consume(TokenType::RightBracket, "expected ']'");
                e = std::make_unique<IndexAccessExpr>(std::move(e), std::move(i));
            } else {
                break;
            }
        }
        return e;
    }

    ExprPtr Parser::parse_block_lambda() {
        auto l = std::make_unique<LambdaExpr>();
        l->is_block = true;
        consume(TokenType::LeftBrace, "expected '{'");
        consume_newlines();
        if (check(TokenType::Identifier) &&
            (peek_next().type == TokenType::Arrow || peek_next().type == TokenType::FatArrow)) {
            Parameter p;
            p.name = advance().lexeme;
            advance();
            l->params.push_back(std::move(p));
        }
        auto blk = std::make_unique<BlockStmt>();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (auto st = parse_declaration()) blk->statements.push_back(std::move(st));
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        l->body = std::move(blk);
        return l;
    }

    ExprPtr Parser::parse_scope_call(ExprPtr receiver) {
        auto sc = std::make_unique<ScopeCallExpr>();
        const std::string nm = dynamic_cast<MemberAccessExpr *>(receiver.get())->member;
        if (nm == "let") sc->kind_sc = ScopeCallExpr::Let;
        else if (nm == "run") sc->kind_sc = ScopeCallExpr::Run;
        else if (nm == "apply") sc->kind_sc = ScopeCallExpr::Apply;
        else if (nm == "also") sc->kind_sc = ScopeCallExpr::Also;
        else sc->kind_sc = ScopeCallExpr::With;

        // 去掉外层 MemberAccess，取真正的接收者
        if (auto *ma = dynamic_cast<MemberAccessExpr *>(receiver.get()))
            sc->receiver = std::move(ma->object);
        else
            sc->receiver = std::move(receiver);

        consume(TokenType::LeftBrace, "expected '{'");
        consume_newlines();
        // 可选 lambda 形参：it -> / x ->
        if (check(TokenType::Identifier) && (peek_next().type == TokenType::Arrow ||
                                             peek_next().type == TokenType::FatArrow)) {
            sc->it_name = advance().lexeme;
            advance();
        }
        auto blk = std::make_unique<BlockStmt>();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            if (auto s = parse_declaration()) blk->statements.push_back(std::move(s));
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        sc->body = std::move(blk);
        return sc;
    }

    ExprPtr Parser::parse_primary_impl() {
        if (match(TokenType::If)) return parse_if_expr();
        if (match(TokenType::When)) return parse_when_expr();
        if (match(TokenType::Fun)) return parse_lambda();
        if (match(TokenType::Super)) {
            if (match(TokenType::LeftParen)) {
                auto args = std::vector<ExprPtr>{};
                if (!check(TokenType::RightParen)) {
                    do { args.push_back(parse_expression()); } while (match(TokenType::Comma));
                }
                consume(TokenType::RightParen, "expected ')'");
                auto c = std::make_unique<CallExpr>();
                c->callee = std::make_unique<IdentifierExpr>("super");
                for (auto &a: args) c->args.emplace_back("", std::move(a));
                return c;
            }
            return std::make_unique<IdentifierExpr>("super");
        }
        if (match(TokenType::Self)) return std::make_unique<IdentifierExpr>("this");

        if (match(TokenType::IntegerLiteral)) {
            const std::string raw = previous().lexeme;
            // 拆分 后缀 i32 / u8 / f64
            size_t cut = raw.size();
            while (cut > 0 && std::isalpha(static_cast<unsigned char>(raw[cut - 1]))) --cut;
            const std::string digits = raw.substr(0, cut);
            const std::string suf = raw.substr(cut);
            auto lit = std::make_unique<LiteralExpr>(LiteralExpr::Int, digits);
            lit->suffix = suf;
            return lit;
        }
        if (match(TokenType::FloatLiteral)) {
            const std::string raw = previous().lexeme;
            size_t cut = raw.size();
            while (cut > 0 && std::isalpha(static_cast<unsigned char>(raw[cut - 1]))) --cut;
            auto lit = std::make_unique<LiteralExpr>(LiteralExpr::Float, raw.substr(0, cut));
            lit->suffix = raw.substr(cut);
            return lit;
        }
        if (match(TokenType::StringLiteral)) {
            return parse_string_template(previous().lexeme, previous());
        }
        if (match(TokenType::CharLiteral)) return std::make_unique<LiteralExpr>(LiteralExpr::Char, previous().lexeme);
        if (match(TokenType::True)) return std::make_unique<LiteralExpr>(LiteralExpr::Bool, "true");
        if (match(TokenType::False)) return std::make_unique<LiteralExpr>(LiteralExpr::Bool, "false");
        if (match(TokenType::Null)) return std::make_unique<LiteralExpr>(LiteralExpr::Null, "null");
        if (match(TokenType::Identifier)) return std::make_unique<IdentifierExpr>(previous().lexeme);

        // 编译期位置关键字：只记下「哪一个」，值交给 comptime pass。
        // 位置由外层 parse_primary 按这个 token 的起点补上，因此
        // `__LINE__` / `__COLUMN__` 报的是关键字自己所在的行列。
        if (match(TokenType::BuiltinFile)) return std::make_unique<BuiltinExpr>(BuiltinExpr::File);
        if (match(TokenType::BuiltinModule)) return std::make_unique<BuiltinExpr>(BuiltinExpr::Module);
        if (match(TokenType::BuiltinLine)) return std::make_unique<BuiltinExpr>(BuiltinExpr::Line);
        if (match(TokenType::BuiltinColumn)) return std::make_unique<BuiltinExpr>(BuiltinExpr::Column);
        if (match(TokenType::BuiltinFunction)) return std::make_unique<BuiltinExpr>(BuiltinExpr::Function);
        if (match(TokenType::BuiltinClass)) return std::make_unique<BuiltinExpr>(BuiltinExpr::Class);
        if (match(TokenType::BuiltinStruct)) return std::make_unique<BuiltinExpr>(BuiltinExpr::Struct);

        if (match(TokenType::LeftParen)) {
            auto e = parse_expression();
            consume(TokenType::RightParen, "expected ')'");
            return std::make_unique<GroupingExpr>(std::move(e));
        }
        if (match(TokenType::LeftBrace)) {
            // 块表达式 { ... }
            auto bx = std::make_unique<BlockExpr>();
            auto blk = std::make_unique<BlockStmt>();
            consume_newlines();
            while (!check(TokenType::RightBrace) && !is_at_end()) {
                if (auto s = parse_declaration()) blk->statements.push_back(std::move(s));
                consume_newlines();
            }
            consume(TokenType::RightBrace, "expected '}'");
            bx->block = std::move(blk);
            bx->is_block = true;
            return bx;
        }
        if (match(TokenType::LeftBracket)) {
            auto a = std::make_unique<ArrayLiteralExpr>();
            if (!check(TokenType::RightBracket)) {
                do {
                    if (check(TokenType::RightBracket)) break;   // 尾随逗号：[1, 2, 3,]
                    a->elements.push_back(parse_expression());
                } while (match(TokenType::Comma));
            }
            consume(TokenType::RightBracket, "expected ']'");
            return a;
        }
        error("expected expression");
    }

    ExprPtr Parser::parse_primary() {
        // 一次性给 parse_primary 的所有返回点补上起点位置：
        // 叶子节点（字面量、标识符、字面量数组…）由此拿到精确行列。
        const Token start = peek();
        auto e = parse_primary_impl();
        if (e && e->line == 0) {
            e->line = static_cast<int>(start.line);
            e->col = static_cast<int>(start.column);
        }
        return e;
    }

    /// 字符串模板："a${b}c" → parts
    ExprPtr Parser::parse_string_template(const std::string &raw, const Token &literal) {
        // raw 已含两端引号
        std::string body = raw;
        if (body.size() >= 2 && body.front() == '"' && body.back() == '"')
            body = body.substr(1, body.size() - 2);

        if (body.find("${") == std::string::npos) {
            return std::make_unique<LiteralExpr>(LiteralExpr::String, "\"" + body + "\"");
        }

        // body 里第 k 个字符在外层源码中的位置（1 基）。
        // 引号占一个字符，所以 body[0] 在字面量 token 起点右边一列。
        // 逐次重扫 body 的前缀：字符串字面量本来就短，这里不做额外索引，
        // 换来的是「插值表达式里的 __LINE__ / __COLUMN__ 指向真实源码」。
        const auto pos_at = [&body, &literal](const size_t idx) {
            int ln = static_cast<int>(literal.line);
            int col = static_cast<int>(literal.column) + 1;
            for (size_t k = 0; k < idx && k < body.size(); ++k) {
                if (body[k] == '\n') {
                    ++ln;
                    col = 1;
                } else {
                    ++col;
                }
            }
            return std::pair<int, int>{ln, col};
        };

        auto tpl = std::make_unique<StringTemplateExpr>();
        std::string cur;
        size_t i = 0;
        while (i < body.size()) {
            if (body[i] == '\\' && i + 1 < body.size()) {
                cur += body[i];
                cur += body[i + 1];
                i += 2;
                continue;
            }
            if (body[i] == '$' && i + 1 < body.size() && body[i + 1] == '{') {
                if (!cur.empty()) { tpl->parts.emplace_back(cur); cur.clear(); }
                size_t depth = 1;
                size_t j = i + 2;
                while (j < body.size() && depth > 0) {
                    if (body[j] == '{') depth++;
                    else if (body[j] == '}') depth--;
                    if (depth == 0) break;
                    j++;
                }
                const std::string expr_src = body.substr(i + 2, j - (i + 2));
                // 解析插值表达式：子解析器带基准位置，否则片段里的
                // token 全是 1:1 的相对坐标。
                const auto [base_line, base_col] = pos_at(i + 2);
                Lexer sub(expr_src);
                Parser sp(sub.tokenize(), base_line, base_col);
                auto ex = sp.parse_expression();
                if (ex) tpl->parts.emplace_back(std::move(ex));
                i = j + 1;
                continue;
            }
            cur += body[i];
            i++;
        }
        if (!cur.empty()) tpl->parts.emplace_back(cur);
        if (tpl->parts.empty()) return std::make_unique<LiteralExpr>(LiteralExpr::String, "\"\"");
        return tpl;
    }

    ExprPtr Parser::parse_lambda() {
        auto l = std::make_unique<LambdaExpr>();
        if (match(TokenType::LeftParen)) {
            l->params = parse_parameters();
            consume(TokenType::RightParen, "expected ')'");
        } else if (match(TokenType::Identifier)) {
            Parameter p;
            p.name = previous().lexeme;
            if (match(TokenType::Colon)) p.type = parse_type();
            l->params.push_back(std::move(p));
        }
        // 可选返回类型
        if (match(TokenType::Colon)) parse_type();
        if (match(TokenType::FatArrow)) {
            consume_newlines();
            l->body = parse_expression();
            l->is_block = false;
        } else if (match(TokenType::Arrow)) {
            consume_newlines();
            l->body = parse_expression();
            l->is_block = false;
        } else if (match(TokenType::LeftBrace)) {
            l->body = parse_block();
            l->is_block = true;
        } else {
            // 无参单表达式 lambda
            l->body = parse_expression();
            l->is_block = false;
        }
        return l;
    }

    ExprPtr Parser::parse_if_expr() {
        const bool paren = match(TokenType::LeftParen);
        auto c = parse_expression();
        if (paren) consume(TokenType::RightParen, "expected ')'");
        consume_newlines();
        ExprPtr t;
        if (match(TokenType::LeftBrace)) {
            auto bx = std::make_unique<BlockExpr>();
            auto blk = std::make_unique<BlockStmt>();
            consume_newlines();
            while (!check(TokenType::RightBrace) && !is_at_end()) {
                if (auto s = parse_declaration()) blk->statements.push_back(std::move(s));
                consume_newlines();
            }
            consume(TokenType::RightBrace, "expected '}'");
            bx->block = std::move(blk);
            bx->is_block = true;
            t = std::move(bx);
        } else t = parse_expression();
        consume_newlines();
        consume(TokenType::Else, "expected 'else'");
        consume_newlines();
        ExprPtr e;
        if (match(TokenType::LeftBrace)) {
            auto bx = std::make_unique<BlockExpr>();
            auto blk = std::make_unique<BlockStmt>();
            consume_newlines();
            while (!check(TokenType::RightBrace) && !is_at_end()) {
                if (auto s = parse_declaration()) blk->statements.push_back(std::move(s));
                consume_newlines();
            }
            consume(TokenType::RightBrace, "expected '}'");
            bx->block = std::move(blk);
            bx->is_block = true;
            e = std::move(bx);
        } else e = parse_expression();
        return std::make_unique<IfExpr>(std::move(c), std::move(t), std::move(e));
    }

    ExprPtr Parser::parse_when_expr() {
        auto w = std::make_unique<WhenExpr>();
        if (!check(TokenType::LeftBrace)) w->subject = parse_expression();
        consume_newlines();
        consume(TokenType::LeftBrace, "expected '{'");
        consume_newlines();
        while (!check(TokenType::RightBrace) && !is_at_end()) {
            consume_newlines();
            if (check(TokenType::RightBrace)) break;

            if (match(TokenType::Else)) {
                match(TokenType::FatArrow);
                match(TokenType::Arrow);
                if (match(TokenType::LeftBrace)) w->else_block = parse_block();
                else w->else_branch = parse_expression();
                match(TokenType::Comma);
                consume_newlines();
                continue;
            }

            WhenExpr::Branch b;
            do {
                consume_newlines();
                WhenExpr::Pattern p;
                if (match(TokenType::Is)) {
                    p.mode = WhenExpr::Pattern::TypeIs;
                    p.type_name = consume(TokenType::Identifier, "expected type name").lexeme;
                } else if (match(TokenType::In)) {
                    p.mode = WhenExpr::Pattern::Range;
                    auto lo = parse_logical_or();
                    bool inc = match(TokenType::DoubleDot);
                    if (!inc) match(TokenType::RangeOpen);
                    auto hi = parse_logical_or();
                    p.lo = std::move(lo);
                    p.hi = std::move(hi);
                    p.inclusive = inc;
                } else {
                    p.mode = WhenExpr::Pattern::Value;
                    p.value = parse_expression();
                }
                b.patterns.push_back(std::move(p));
                consume_newlines();
            } while (match(TokenType::Comma));

            match(TokenType::FatArrow);
            match(TokenType::Arrow);
            consume_newlines();
            if (match(TokenType::LeftBrace)) b.body_block = parse_block();
            else b.body = parse_expression();
            w->branches.push_back(std::move(b));
            match(TokenType::Comma);
            consume_newlines();
        }
        consume(TokenType::RightBrace, "expected '}'");
        return w;
    }

    std::vector<std::unique_ptr<TypeAnnotation> > Parser::parse_type_args() {
        std::vector<std::unique_ptr<TypeAnnotation> > out;
        if (!match(TokenType::Less)) return out;
        do { out.push_back(parse_type()); } while (match(TokenType::Comma));
        consume(TokenType::Greater, "expected '>'");
        return out;
    }

    std::unique_ptr<TypeAnnotation> Parser::parse_type() {
        if (match(TokenType::Star)) {
            auto p = std::make_unique<TypeAnnotation>();
            p->kind = TypeAnnotation::Pointer;
            p->inner_type = parse_type();
            return p;
        }
        // Kotlin 接收者函数类型：Receiver.() -> R / Box<T>.(i64) -> R
        if (check(TokenType::Identifier) && peek_next().type == TokenType::Dot &&
            peek_at(2).type == TokenType::LeftParen) {
            auto recv = parse_basic_type();
            consume(TokenType::Dot, "expected '.'");
            auto ft = std::make_unique<TypeAnnotation>();
            ft->kind = TypeAnnotation::FunctionType;
            ft->receiver_type = std::move(recv);
            consume(TokenType::LeftParen, "expected '('");
            while (!check(TokenType::RightParen) && !is_at_end()) {
                ft->generic_args.push_back(parse_type());
                if (!check(TokenType::RightParen)) consume(TokenType::Comma, "expected ','");
            }
            consume(TokenType::RightParen, "expected ')'");
            consume(TokenType::Arrow, "expected '->'");
            ft->inner_type = parse_type();
            return ft;
        }
        auto t = parse_basic_type();

        // 引用 & / &&：Sinter 里类/结构体本身就是指针，引用与指针同构，
        // 所以包一层 Pointer 并**保留 inner_type**。早先只抄了 name、丢掉 inner，
        // 语义层解析时拿不到被引用类型，`operator=(other: T&&)` 会退化成 `long long*`。
        if (check(TokenType::DoubleAmpersand)) {
            advance();
            auto r = std::make_unique<TypeAnnotation>();
            r->kind = TypeAnnotation::Pointer;
            r->inner_type = std::move(t);
            r->is_rvalue_ref = true;
            return r;
        }
        if (check(TokenType::Ampersand)) {
            advance();
            t->is_ref = true;
            return t;
        }
        if (match(TokenType::Star)) {
            auto p = std::make_unique<TypeAnnotation>();
            p->kind = TypeAnnotation::Pointer;
            p->inner_type = std::move(t);
            return p;
        }
        if (match(TokenType::Question)) {
            auto o = std::make_unique<TypeAnnotation>();
            o->kind = TypeAnnotation::Optional;
            o->inner_type = std::move(t);
            return o;
        }
        // 数组 T[N]
        if (match(TokenType::LeftBracket)) {
            int n = 0;
            if (check(TokenType::IntegerLiteral))
                n = std::atoi(advance().lexeme.c_str());
            consume(TokenType::RightBracket, "expected ']'");
            auto a = std::make_unique<TypeAnnotation>();
            a->kind = TypeAnnotation::Array;
            a->inner_type = std::move(t);
            a->array_size = n;
            return a;
        }
        return t;
    }

void Parser::validate_tparams(const std::vector<TemplateParam> &tps) {
        for (size_t i = 0; i < tps.size(); ++i) {
            if (!tps[i].is_pack) continue;
            if (i + 1 != tps.size())
                error("type parameter pack `" + tps[i].name + "...` must be the last type parameter");
        }
    }

    bool Parser::is_active_pack(const std::string &n) const {
        for (auto &p: active_pack_names_)
            if (p == n) return true;
        return false;
    }

    std::unique_ptr<TypeAnnotation> Parser::parse_basic_type() {
        if (match(TokenType::Identifier)) {
            auto t = std::make_unique<TypeAnnotation>(previous().lexeme);
            // 类型参数包索引：Args[0]（仅当 Args 确实是当前泛型参数包名时）
            if (is_active_pack(t->name) && check(TokenType::LeftBracket)) {
                const auto save = current;
                advance(); // [
                if (check(TokenType::IntegerLiteral)) {
                    const int idx = std::atoi(advance().lexeme.c_str());
                    if (match(TokenType::RightBracket)) {
                        t->pack_index = idx;
                        return t;
                    }
                }
                current = save;   // 不是索引形式，回退成普通类型（数组在下面处理）
            }
            if (match(TokenType::Less)) {
                t->kind = TypeAnnotation::Generic;
                do { t->generic_args.push_back(parse_type()); } while (match(TokenType::Comma));
                consume(TokenType::Greater, "expected '>'");
            }
            return t;
        }
        if (match(TokenType::LeftParen)) {
            auto ft = std::make_unique<TypeAnnotation>();
            ft->kind = TypeAnnotation::FunctionType;
            while (!check(TokenType::RightParen) && !is_at_end()) {
                ft->generic_args.push_back(parse_type());
                if (!check(TokenType::RightParen)) consume(TokenType::Comma, "expected ','");
            }
            consume(TokenType::RightParen, "expected ')'");
            consume(TokenType::Arrow, "expected '->'");
            ft->inner_type = parse_type();
            return ft;
        }
        error("expected type");
    }

    /// 把 parse_parameters() 期间记下的可变参数信息写入 FunctionDecl。
    /// parse_parameters() 本身不接触 FunctionDecl，便于被构造函数等复用。
    void Parser::apply_variadic(FunctionDecl &f) {
        if (!variadic_flag_) return;
        if (f.params.empty() || f.params.back().name != variadic_param_name_) return;
        f.is_variadic = true;
        f.variadic_name = variadic_param_name_;
        f.variadic_elem_type = clone_type(f.params.back().type.get());
        variadic_flag_ = false;
        variadic_param_name_.clear();
    }

    std::vector<Parameter> Parser::parse_parameters() {
        std::vector<Parameter> ps;
        variadic_param_name_.clear();
        variadic_flag_ = false;
        if (check(TokenType::RightParen) || check(TokenType::DotDotDot)) return ps;
        do {
            consume_newlines();
            if (check(TokenType::RightParen)) break;
            // C 风格的裸 `...`：接收但不支持（真正的可变参数要写成 `xs: T...`）
            if (check(TokenType::DotDotDot)) { advance(); break; }
            Parameter p;
            if (match(TokenType::Val)) p.is_val = true;
            else if (match(TokenType::Var)) p.is_var = true;
            if (match(TokenType::Self)) {
                p.name = "this";
                p.is_self = true;
            } else {
                p.name = consume(TokenType::Identifier, "expected parameter name").lexeme;
            }
            if (match(TokenType::Colon)) p.type = parse_type();
            // 可变参数：`xs: i64...` —— 必须放在参数表最后
            if (match(TokenType::DotDotDot)) {
                variadic_flag_ = true;
                variadic_param_name_ = p.name;
                ps.push_back(std::move(p));
                break;
            }
            if (match(TokenType::Equal)) p.default_value = parse_expression();
            ps.push_back(std::move(p));
            if (check(TokenType::RightParen)) break;   // 尾随逗号：fun f(a: i64,)
        } while (match(TokenType::Comma));
        return ps;
    }

    bool Parser::looks_like_type_arg_list() {
        if (!check(TokenType::Less)) return false;
        size_t j = 1;
        size_t depth = 1;
        while (depth > 0) {
            const auto t = peek_at(j++).type;
            if (t == TokenType::EndOfFile) return false;
            if (t == TokenType::Less) depth++;
            else if (t == TokenType::Greater) depth--;
            else if (t == TokenType::Newline) return false;
        }
        return peek_at(j).type == TokenType::LeftParen;
    }
    bool Parser::looks_like_ctor_call() { return false; }

    bool Parser::is_identifier_expr(Expression *e) {
        if (dynamic_cast<IdentifierExpr *>(e)) return true;
        if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) return is_identifier_expr(m->object.get());
        return false;
    }

    /// `fun f = g` / `fun f = g<T>` 视为function alias；`fun f = expr` 视为表达式函数。
    /// 判定：`=` 后第一个 token 是标识符，且其后的 token 是语句结束（分号/换行/右括号/文件尾）
    /// 或泛型实参列表后紧跟语句结束。
    bool Parser::is_explicit_alias_form() {
        if (!check(TokenType::Identifier)) return false;
        size_t k = 1;
        while (peek_at(k).type == TokenType::Dot && peek_at(k + 1).type == TokenType::Identifier) k += 2;
        auto is_end = [](TokenType t) {
            return t == TokenType::Semicolon || t == TokenType::Newline ||
                   t == TokenType::RightBrace || t == TokenType::RightParen ||
                   t == TokenType::EndOfFile;
        };
        if (is_end(peek_at(k).type)) return true;
        if (peek_at(k).type == TokenType::Less) {
            int depth = 0;
            size_t m = k;
            while (m < k + 16) {
                const auto t = peek_at(m).type;
                if (t == TokenType::EndOfFile) return false;
                if (t == TokenType::Less) depth++;
                else if (t == TokenType::Greater) { depth--; if (depth == 0) return is_end(peek_at(m + 1).type); }
                ++m;
            }
        }
        return false;
    }
    bool Parser::looks_like_destructure() { return false; }

    Program Parser::parse() {
        Program p;
        consume_newlines();
        while (!is_at_end()) {
            auto s = parse_declaration();
            if (s) p.statements.push_back(std::move(s));
            consume_newlines();
        }
        return p;
    }
} // namespace sinter
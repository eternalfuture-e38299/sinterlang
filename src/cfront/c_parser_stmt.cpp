// cfront: c_parser_stmt.cpp —— C 语句解析
//
// 覆盖：复合语句、if/else、while、do-while、for（含 C99 声明式 init）、
//       switch（含 case/default）、return、break、continue、goto、标签、
//       声明语句（用 looks_like_decl_start 与表达式语句区分）。
#include "cfront/c_parser.hpp"

namespace sinter::cfront {
    namespace {
        bool stmt_starts_with_type(const CTokKind k) {
            switch (k) {
                case CTokKind::KwVoid:
                case CTokKind::KwChar:
                case CTokKind::KwShort:
                case CTokKind::KwInt:
                case CTokKind::KwLong:
                case CTokKind::KwFloat:
                case CTokKind::KwDouble:
                case CTokKind::KwSigned:
                case CTokKind::KwUnsigned:
                case CTokKind::KwBool:
                case CTokKind::KwStruct:
                case CTokKind::KwUnion:
                case CTokKind::KwEnum:
                case CTokKind::KwConst:
                case CTokKind::KwVolatile:
                case CTokKind::KwStatic:
                case CTokKind::KwExtern:
                case CTokKind::KwRegister:
                case CTokKind::KwTypedef:
                case CTokKind::KwAtomic:
                case CTokKind::KwAlignas:
                case CTokKind::Attribute:
                case CTokKind::KwAuto: return true;
                default: return false;
            }
        }
    } // namespace

    CStmtPtr CParser::parse_statement() {
        const CTokKind k = peek().kind;
        switch (k) {
            case CTokKind::LBrace: return parse_compound();
            // 语句级扩展说明符：跳过它，解析被修饰的那条语句
            case CTokKind::Attribute: {
                while (check(CTokKind::Attribute)) advance();
                return parse_statement();
            }
            // `_Static_assert(...)` 在运行时没有语义，复用 Asm 槽位占位
            case CTokKind::KwStaticAssert: {
                auto s = std::make_unique<CStmt>();
                s->kind = CStmt::Kind::Asm;
                s->range = span_of(peek());
                advance();
                if (match(CTokKind::LParen)) {
                    int depth = 0;
                    while (!at_end()) {
                        if (check(CTokKind::LParen)) ++depth;
                        else if (check(CTokKind::RParen)) {
                            if (depth == 0) { advance(); break; }
                            --depth;
                        }
                        advance();
                    }
                }
                match(CTokKind::Semicolon);
                return s;
            }
            case CTokKind::KwIf: return parse_if();
            case CTokKind::KwWhile: return parse_while();
            case CTokKind::KwDo: return parse_do_while();
            case CTokKind::KwFor: return parse_for();
            case CTokKind::KwSwitch: return parse_switch();
            case CTokKind::KwReturn: return parse_return();
            case CTokKind::KwBreak:
            case CTokKind::KwContinue:
            case CTokKind::KwGoto: return parse_jump();
            case CTokKind::KwCase:
            case CTokKind::KwDefault: return parse_labeled();
            case CTokKind::Semicolon: {
                auto s = std::make_unique<CStmt>();
                s->kind = CStmt::Kind::Null;
                s->range = span_of(peek());
                advance();
                return s;
            }
            case CTokKind::Identifier:
                // 标签：ident ':'
                if (peek(1).kind == CTokKind::Colon) return parse_labeled();
                break;
            default: break;
        }
        if (stmt_starts_with_type(k) || looks_like_decl_start()) return parse_decl_statement();
        return parse_expr_statement();
    }

    CStmtPtr CParser::parse_compound() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::Compound;
        s->range = span_of(peek());
        if (!consume(CTokKind::LBrace, "'{'")) return s;
        // 进入新作用域：块级 typedef 可见性简化处理（解析完即丢，够用）
        while (!at_end() && !check(CTokKind::RBrace)) {
            const size_t before = pos_;
            auto child = parse_statement();
            if (child) s->items.push_back(std::move(child));
            if (pos_ == before) advance();   // 防死循环
        }
        match(CTokKind::RBrace);
        return s;
    }

    CStmtPtr CParser::parse_if() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::If;
        s->range = span_of(peek());
        advance();   // if
        if (consume(CTokKind::LParen, "'('")) {
            s->cond = parse_expression();
            consume(CTokKind::RParen, "')'");
        }
        s->body = parse_statement();
        if (match(CTokKind::KwElse)) s->else_body = parse_statement();
        return s;
    }

    CStmtPtr CParser::parse_while() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::While;
        s->range = span_of(peek());
        advance();
        if (consume(CTokKind::LParen, "'('")) {
            s->cond = parse_expression();
            consume(CTokKind::RParen, "')'");
        }
        s->body = parse_statement();
        return s;
    }

    CStmtPtr CParser::parse_do_while() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::DoWhile;
        s->range = span_of(peek());
        advance();   // do
        s->body = parse_statement();
        if (match(CTokKind::KwWhile)) {
            if (consume(CTokKind::LParen, "'('")) {
                s->cond = parse_expression();
                consume(CTokKind::RParen, "')'");
            }
            match(CTokKind::Semicolon);
        }
        return s;
    }

    CStmtPtr CParser::parse_for() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::For;
        s->range = span_of(peek());
        advance();   // for
        if (!consume(CTokKind::LParen, "'('")) {
            s->body = parse_statement();
            return s;
        }
        // C99: for (int i = 0; ...)
        if (!check(CTokKind::Semicolon)) {
            if (stmt_starts_with_type(peek().kind) || looks_like_decl_start())
                s->init = parse_decl_statement();
            else {
                auto e = std::make_unique<CStmt>();
                e->kind = CStmt::Kind::Expr;
                e->cond = parse_expression();
                match(CTokKind::Semicolon);
                s->init = std::move(e);
            }
        } else {
            advance();   // 空 init
        }
        if (!check(CTokKind::Semicolon)) s->cond = parse_expression();
        match(CTokKind::Semicolon);
        if (!check(CTokKind::RParen)) s->step = parse_expression();
        consume(CTokKind::RParen, "')'");
        s->body = parse_statement();
        return s;
    }

    CStmtPtr CParser::parse_switch() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::Switch;
        s->range = span_of(peek());
        advance();   // switch
        if (consume(CTokKind::LParen, "'('")) {
            s->cond = parse_expression();
            consume(CTokKind::RParen, "')'");
        }
        s->body = parse_statement();
        return s;
    }

    CStmtPtr CParser::parse_return() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::Return;
        s->range = span_of(peek());
        advance();   // return
        if (!check(CTokKind::Semicolon)) s->cond = parse_expression();
        match(CTokKind::Semicolon);
        return s;
    }

    CStmtPtr CParser::parse_jump() {
        auto s = std::make_unique<CStmt>();
        s->range = span_of(peek());
        if (check(CTokKind::KwBreak)) {
            s->kind = CStmt::Kind::Break;
            advance();
        } else if (check(CTokKind::KwContinue)) {
            s->kind = CStmt::Kind::Continue;
            advance();
        } else {
            s->kind = CStmt::Kind::Goto;
            advance();
            if (check(CTokKind::Identifier)) s->label = advance().text;
        }
        match(CTokKind::Semicolon);
        return s;
    }

    CStmtPtr CParser::parse_labeled() {
        auto s = std::make_unique<CStmt>();
        s->range = span_of(peek());
        if (check(CTokKind::KwCase)) {
            s->kind = CStmt::Kind::Case;
            advance();
            if (auto e = parse_conditional()) s->label = e->text;
        } else if (check(CTokKind::KwDefault)) {
            s->kind = CStmt::Kind::Default;
            advance();
        } else {
            s->kind = CStmt::Kind::Label;
            if (check(CTokKind::Identifier)) s->label = advance().text;
        }
        consume(CTokKind::Colon, "':'");
        // case/default 后面可以紧跟语句
        if (!check(CTokKind::RBrace) && !at_end()) s->body = parse_statement();
        return s;
    }

    CStmtPtr CParser::parse_decl_statement() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::Decl;
        s->range = span_of(peek());
        parse_type_spec(s->decl_type);

        // struct Foo { ... }; 或仅声明类型
        if (check(CTokKind::Semicolon)) {
            advance();
            return s;
        }

        while (!at_end()) {
            CField f;
            f.spec = s->decl_type;
            f.range = span_of(peek());
            f.decl = parse_declarator(true);
            if (!f.decl) break;
            if (match(CTokKind::Assign)) parse_assign();
            s->decls.push_back(std::move(f));
            if (!match(CTokKind::Comma)) break;
        }
        match(CTokKind::Semicolon);
        return s;
    }

    CStmtPtr CParser::parse_expr_statement() {
        auto s = std::make_unique<CStmt>();
        s->kind = CStmt::Kind::Expr;
        s->range = span_of(peek());
        s->cond = parse_expression();
        match(CTokKind::Semicolon);
        return s;
    }
} // namespace sinter::cfront
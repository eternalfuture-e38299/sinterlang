// cfront: c_parser_expr.cpp —— C 表达式解析（优先级爬升）
//
// 优先级表（低→高）：|| && | ^ & == != < > <= >= << >> + - * / %
// 赋值与条件表达式在 parse_assign / parse_conditional 里处理，
// 因为赋值是右结合、条件表达式是三元的，都不适合用纯二元表。
#include "cfront/c_parser.hpp"

namespace sinter::cfront {
    namespace {
        int bin_prec(const CTokKind k) {
            switch (k) {
                case CTokKind::PipePipe: return 1;
                case CTokKind::AmpAmp: return 2;
                case CTokKind::Pipe: return 3;
                case CTokKind::Caret: return 4;
                case CTokKind::Amp: return 5;
                case CTokKind::EqualEqual:
                case CTokKind::NotEqual: return 6;
                case CTokKind::Less:
                case CTokKind::Greater:
                case CTokKind::LessEqual:
                case CTokKind::GreaterEqual: return 7;
                case CTokKind::Shl:
                case CTokKind::Shr: return 8;
                case CTokKind::Plus:
                case CTokKind::Minus: return 9;
                case CTokKind::Star:
                case CTokKind::Slash:
                case CTokKind::Percent: return 10;
                default: return 0;
            }
        }

        bool is_assign(const CTokKind k) {
            switch (k) {
                case CTokKind::Assign:
                case CTokKind::PlusAssign:
                case CTokKind::MinusAssign:
                case CTokKind::StarAssign:
                case CTokKind::SlashAssign:
                case CTokKind::PercentAssign:
                case CTokKind::AmpAssign:
                case CTokKind::PipeAssign:
                case CTokKind::CaretAssign:
                case CTokKind::ShlAssign:
                case CTokKind::ShrAssign: return true;
                default: return false;
            }
        }
    } // namespace

    CExprPtr CParser::parse_expression() {
        CExprPtr e = parse_assign();
        // 逗号表达式
        while (check(CTokKind::Comma)) {
            const CToken &op = advance();
            auto n = std::make_unique<CExpr>();
            n->kind = CExpr::Kind::Comma;
            n->text = op.text;
            n->range = span_of(op);
            n->lhs = std::move(e);
            n->rhs = parse_assign();
            e = std::move(n);
        }
        return e;
    }

    CExprPtr CParser::parse_assign() {
        CExprPtr lhs = parse_conditional();
        if (!lhs) return lhs;
        if (is_assign(peek().kind)) {
            const CToken &op = advance();
            auto n = std::make_unique<CExpr>();
            n->kind = CExpr::Kind::Assign;
            n->text = op.text;
            n->range = span_of(op);
            n->lhs = std::move(lhs);
            n->rhs = parse_assign();   // 右结合
            return n;
        }
        return lhs;
    }

    CExprPtr CParser::parse_conditional() {
        CExprPtr cond = parse_binary(1);
        if (!cond) return cond;
        if (check(CTokKind::Question)) {
            advance();
            auto n = std::make_unique<CExpr>();
            n->kind = CExpr::Kind::Conditional;
            n->range = cond->range;
            n->lhs = std::move(cond);
            n->rhs = parse_expression();
            consume(CTokKind::Colon, "':'");
            n->third = parse_conditional();
            return n;
        }
        return cond;
    }

    CExprPtr CParser::parse_binary(const int min_prec) {
        CExprPtr lhs = parse_unary();
        if (!lhs) return lhs;
        while (true) {
            const int prec = bin_prec(peek().kind);
            if (prec == 0 || prec < min_prec) break;
            const CToken &op = advance();
            auto rhs = parse_binary(prec + 1);   // 左结合
            auto n = std::make_unique<CExpr>();
            n->kind = CExpr::Kind::Binary;
            n->text = op.text;
            n->range = span_of(op);
            n->lhs = std::move(lhs);
            n->rhs = std::move(rhs);
            lhs = std::move(n);
        }
        return lhs;
    }

    CExprPtr CParser::parse_unary() {
        const CTokKind k = peek().kind;
        // 前缀运算符
        if (k == CTokKind::Plus || k == CTokKind::Minus || k == CTokKind::Bang ||
            k == CTokKind::Tilde || k == CTokKind::Star || k == CTokKind::Amp ||
            k == CTokKind::PlusPlus || k == CTokKind::MinusMinus) {
            const CToken &op = advance();
            auto n = std::make_unique<CExpr>();
            n->kind = CExpr::Kind::Unary;
            n->text = op.text;
            n->range = span_of(op);
            n->lhs = parse_unary();
            return n;
        }
        // sizeof / _Alignof
        if (k == CTokKind::KwSizeof || k == CTokKind::KwAlignof) {
            const bool is_sizeof = (k == CTokKind::KwSizeof);
            advance();
            auto n = std::make_unique<CExpr>();
            n->kind = is_sizeof ? CExpr::Kind::Sizeof : CExpr::Kind::Alignof;
            n->range = span_of(peek());
            if (check(CTokKind::LParen) && is_type_start_at(1)) {
                advance();
                parse_type_spec(n->cast_type);
                if (auto d = parse_declarator(true)) n->text = d->leaf_name();
                consume(CTokKind::RParen, "')'");
            } else {
                n->lhs = parse_unary();
            }
            return n;
        }
        // 强制类型转换 (type)expr
        if (k == CTokKind::LParen && is_type_start_at(1)) {
            const size_t save = pos_;
            advance();
            TypeSpec ts;
            parse_type_spec(ts);
            std::unique_ptr<CDeclarator> d;
            if (check(CTokKind::Star)) d = parse_declarator(true);
            if (check(CTokKind::RParen) && !ts.empty()) {
                advance();
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::Cast;
                n->cast_type = std::move(ts);
                n->range = span_of(peek());
                n->lhs = parse_unary();
                return n;
            }
            pos_ = save;   // 不是转换，退回按 primary 处理
        }
        return parse_postfix();
    }

    CExprPtr CParser::parse_postfix() {
        CExprPtr e = parse_primary();
        if (!e) return e;
        while (true) {
            if (check(CTokKind::LBracket)) {
                advance();
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::Index;
                n->lhs = std::move(e);
                n->rhs = parse_expression();
                consume(CTokKind::RBracket, "']'");
                e = std::move(n);
                continue;
            }
            if (check(CTokKind::LParen)) {
                advance();
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::Call;
                n->lhs = std::move(e);
                while (!at_end() && !check(CTokKind::RParen)) {
                    n->args.push_back(parse_assign());
                    if (!match(CTokKind::Comma)) break;
                }
                consume(CTokKind::RParen, "')'");
                e = std::move(n);
                continue;
            }
            if (check(CTokKind::Dot) || check(CTokKind::Arrow)) {
                const bool arrow = check(CTokKind::Arrow);
                advance();
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::Member;
                n->arrow = arrow;
                n->lhs = std::move(e);
                if (check(CTokKind::Identifier)) n->text = advance().text;
                e = std::move(n);
                continue;
            }
            if (check(CTokKind::PlusPlus) || check(CTokKind::MinusMinus)) {
                const CToken &op = advance();
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::Unary;
                n->text = op.text;   // 后缀运算：lhs 已完整
                n->range = span_of(op);
                n->lhs = std::move(e);
                e = std::move(n);
                continue;
            }
            break;
        }
        return e;
    }

    CExprPtr CParser::parse_primary() {
        const CToken &t = peek();
        switch (t.kind) {
            // 初始化列表：`{ 1, 2 }` / `{ .x = 1, .y = 2 }` / `{ [3] = 4 }`
            case CTokKind::LBrace: {
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::InitList;
                n->range = span_of(t);
                advance();
                while (!at_end() && !check(CTokKind::RBrace)) {
                    CInitElem el;
                    if (check(CTokKind::Dot)) {
                        advance();
                        std::string d = ".";
                        if (check(CTokKind::Identifier)) d += advance().text;
                        el.designator = d;
                        match(CTokKind::Assign);
                    } else if (check(CTokKind::LBracket)) {
                        advance();
                        std::string d = "[";
                        int depth = 0;
                        while (!at_end()) {
                            if (check(CTokKind::LBracket)) { ++depth; advance(); continue; }
                            if (check(CTokKind::RBracket)) {
                                if (depth == 0) { advance(); break; }
                                --depth; advance(); continue;
                            }
                            d += peek().text;
                            advance();
                        }
                        d += "]";
                        el.designator = d;
                        match(CTokKind::Assign);
                    }
                    el.value = parse_assign();
                    if (el.value) n->init_elems.push_back(std::move(el));
                    if (!match(CTokKind::Comma)) break;
                }
                match(CTokKind::RBrace);
                return n;
            }
            // `_Generic(ctrl, T: e, default: e)`：关联类型原文留在 assoc_types 里
            case CTokKind::KwGeneric: {
                advance();
                auto n = std::make_unique<CExpr>();
                n->kind = CExpr::Kind::Generic;
                n->range = span_of(t);
                if (match(CTokKind::LParen)) {
                    n->lhs = parse_assign();
                    while (match(CTokKind::Comma)) {
                        if (check(CTokKind::KwDefault)) {
                            advance();
                            match(CTokKind::Colon);
                            n->assoc_types.emplace_back();
                            n->args.push_back(parse_assign());
                            continue;
                        }
                        TypeSpec ts;
                        parse_type_spec(ts);
                        if (check(CTokKind::Star)) parse_declarator(true);
                        match(CTokKind::Colon);
                        n->assoc_types.push_back(canonical_type(ts));
                        n->args.push_back(parse_assign());
                    }
                    match(CTokKind::RParen);
                }
                return n;
            }
            case CTokKind::IntLiteral:
            case CTokKind::FloatLiteral:
            case CTokKind::CharLiteral:
            case CTokKind::StringLiteral:
            case CTokKind::Identifier: {
                const CToken &tk = advance();
                auto n = std::make_unique<CExpr>();
                n->kind = tk.kind == CTokKind::Identifier
                              ? CExpr::Kind::Identifier
                              : (tk.kind == CTokKind::IntLiteral
                                     ? CExpr::Kind::IntLit
                                     : (tk.kind == CTokKind::FloatLiteral
                                            ? CExpr::Kind::FloatLit
                                            : (tk.kind == CTokKind::CharLiteral
                                                   ? CExpr::Kind::CharLit
                                                   : CExpr::Kind::StringLit)));
                n->text = tk.text;
                n->range = span_of(tk);
                return n;
            }
            case CTokKind::LParen: {
                advance();
                // 复合字面量 / 普通括号
                auto inner = parse_expression();
                consume(CTokKind::RParen, "')'");
                if (!inner) {
                    auto n = std::make_unique<CExpr>();
                    n->kind = CExpr::Kind::IntLit;
                    n->text = "0";
                    return n;
                }
                return inner;
            }
            default: break;
        }
        // 无法解析：报错并吞掉一个 token，避免死循环
        error_at(t, std::string("expected expression, found '") + (t.text.empty() ? c_tok_name(t.kind) : t.text) + "'");
        if (!at_end()) advance();
        auto n = std::make_unique<CExpr>();
        n->kind = CExpr::Kind::IntLit;
        n->text = "0";
        n->range = span_of(t);
        return n;
    }

    bool CParser::is_type_start_here() const { return is_type_start_at(0); }

    bool CParser::is_type_start_at(const size_t off) const {
        const CTokKind k = peek(off).kind;
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
            case CTokKind::KwConst:
            case CTokKind::KwVolatile:
            case CTokKind::KwStruct:
            case CTokKind::KwUnion:
            case CTokKind::KwEnum: return true;
            case CTokKind::Identifier: return typedefs_.count(peek(off).text) > 0;
            default: return false;
        }
    }
} // namespace sinter::cfront
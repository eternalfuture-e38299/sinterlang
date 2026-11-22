#include "c2_lexer.hpp"
#include "c2_parser.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace sinter::cfront2 {
    namespace {
        struct Specs {
            std::string text;
            bool is_typedef = false;
            bool has_type = false;
        };

        struct Declarator {
            std::string name;
            std::string type;
            std::string return_type;
            std::vector<Parameter> params;
            bool variadic = false;
            bool function = false;
            bool definition = false;
        };

        class Parser {
        public:
            explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {
                // 内置 typedef：生成 C 的 runtime 头里这些名字很常见，第二前端
                // 不解析 #include，所以先把它们登记为类型名，避免把
                // `sinter_string x` 误判成表达式。
                static const char *const kBuiltin[] = {
                    "int8_t", "int16_t", "int32_t", "int64_t",
                    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
                    "mode_t", "pid_t", "uid_t", "gid_t", "off_t", "dev_t", "ino_t",
                    "nlink_t", "time_t", "clock_t", "socklen_t",
                    "sinter_string", "sinter_vfn", "std_size_t", "std_ssize_t",
                    "FILE", "va_list", "bool",
                };
                for (const char *t: kBuiltin) typedefs_.insert(t);
            }

            Unit run() {
                while (!at_eof()) {
                    if (peek().kind == TokKind::Preproc) {
                        record_preproc(peek().text);
                        advance();
                        continue;
                    }
                    if (accept(";")) continue;
                    parse_external();
                }
                unit_.diagnostics = std::move(errors_);
                return std::move(unit_);
            }

        private:
            std::vector<Token> toks_;            size_t pos_ = 0;
            Unit unit_;
            std::vector<Diagnostic> errors_;
            std::unordered_set<std::string> typedefs_;

            const Token &peek(size_t k = 0) const {
                const size_t i = pos_ + k;
                return i < toks_.size() ? toks_[i] : toks_.back();
            }
            bool at(const char *text) const { return peek().text == text; }
            bool at_eof() const { return peek().kind == TokKind::Eof; }
            const Token &advance() {
                const Token &t = peek();
                if (!at_eof()) ++pos_;
                return t;
            }
            bool accept(const char *text) {
                if (!at(text)) return false;
                advance();
                return true;
            }
            bool expect(const char *text) {
                if (accept(text)) return true;
                error(std::string("expected '") + text + "', found '" + peek().text + "'");
                if (!at_eof()) advance();
                return false;
            }
            void error(const std::string &message) {
                errors_.push_back({peek().line, peek().col, message});
            }

            static bool is_type_start(const Token &t) {
                static const std::unordered_set<std::string> k = {
                    "void", "char", "short", "int", "long", "float", "double", "signed",
                    "unsigned", "_Bool", "_Complex", "struct", "union", "enum", "const",
                    "volatile", "restrict", "typedef", "extern", "static", "auto", "register",
                    "inline",
                };
                return t.kind == TokKind::Keyword && k.contains(t.text);
            }

            bool is_type_start_here() const {
                const Token &t = peek();
                if (is_type_start(t)) return true;
                return t.kind == TokKind::Identifier && typedefs_.contains(t.text);
            }

            void record_preproc(const std::string &line) {
                const size_t inc = line.find("#include");
                if (inc == std::string::npos) return;
                size_t i = inc + 8;
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
                if (i >= line.size()) return;
                if (line[i] == '"' || line[i] == '<') {
                    const char close = line[i] == '"' ? '"' : '>';
                    const size_t end = line.find(close, i + 1);
                    if (end != std::string::npos) unit_.includes.push_back(line.substr(i + 1, end - i - 1));
                }
            }

            /// GCC 扩展：`__attribute__((...))` 跳过；`__extension__` 只跳过关键字。
            void skip_attribute() {
                if (!at("__attribute__") && !at("__attribute")) return;
                advance();
                if (!accept("(")) return;
                int depth = 1;
                while (depth > 0 && !at_eof()) {
                    if (at("(")) ++depth;
                    else if (at(")")) --depth;
                    advance();
                }
            }

            // ---------------- declarations ----------------
            void parse_external() {
                Specs specs;
                if (!parse_specs(specs)) {
                    // 生成 C 里可能出现未登记的 typedef（来自 runtime 头）。
                    // 顶层出现 `Type name` 时把第一个标识符当类型名，避免整段崩掉。
                    if (peek().kind == TokKind::Identifier) {
                        specs.text = advance().text;
                        specs.has_type = true;
                    } else {
                        error("expected a declaration");
                        if (!at_eof()) advance();
                        return;
                    }
                }
                if (accept(";")) return;

                Declarator first = parse_declarator(specs);
                if (first.function && at("{")) {
                    first.definition = true;
                    add_declarator(specs, first);
                    parse_compound();
                    return;
                }
                add_declarator(specs, first);
                parse_initializer_opt();
                while (accept(",")) {
                    Declarator d = parse_declarator(specs);
                    add_declarator(specs, d);
                    parse_initializer_opt();
                }
                expect(";");
            }

            bool parse_specs(Specs &specs) {
                bool any = false;
                while (!at_eof()) {
                    const Token &t = peek();
                    if (t.text == "__extension__") { advance(); continue; }
                    if (t.text == "__attribute__" || t.text == "__attribute") { skip_attribute(); continue; }
                    if (t.text == "typedef") {
                        specs.is_typedef = true;
                        any = true;
                        advance();
                        continue;
                    }
                    if (t.text == "struct" || t.text == "union") {
                        const std::string kind = t.text;
                        advance();
                        while (at("__attribute__") || at("__attribute")) skip_attribute();
                        std::string name;
                        if (peek().kind == TokKind::Identifier) name = advance().text;
                        while (at("__attribute__") || at("__attribute")) skip_attribute();
                        if (at("{")) parse_record_body(kind, name);
                        if (!specs.text.empty()) specs.text += " ";
                        specs.text += kind;
                        if (!name.empty()) specs.text += " " + name;
                        specs.has_type = true;
                        any = true;
                        continue;
                    }
                    if (t.text == "enum") {
                        advance();
                        while (at("__attribute__") || at("__attribute")) skip_attribute();
                        std::string name;
                        if (peek().kind == TokKind::Identifier) name = advance().text;
                        while (at("__attribute__") || at("__attribute")) skip_attribute();
                        RecordDecl rec;
                        rec.kind = "enum";
                        rec.name = name;
                        rec.line = t.line;
                        if (at("{")) {
                            advance();
                            while (!at("}") && !at_eof()) {
                                if (peek().kind != TokKind::Identifier) {
                                    error("expected enum constant");
                                    if (!at_eof()) advance();
                                    continue;
                                }
                                rec.variants.push_back(advance().text);
                                if (accept("=")) parse_conditional();
                                if (!accept(",")) break;
                            }
                            expect("}");
                        }
                        if (!name.empty()) unit_.records.push_back(std::move(rec));
                        if (!specs.text.empty()) specs.text += " ";
                        specs.text += "enum";
                        if (!name.empty()) specs.text += " " + name;
                        specs.has_type = true;
                        any = true;
                        continue;
                    }
                    if (is_type_start(t) || (t.kind == TokKind::Identifier && typedefs_.contains(t.text))) {
                        if (!specs.text.empty()) specs.text += " ";
                        specs.text += t.text;
                        specs.has_type = true;
                        any = true;
                        advance();
                        continue;
                    }
                    // 未登记 typedef 的启发式：`Type name` / `Type *name`
                    if (t.kind == TokKind::Identifier) {
                        const bool next_ident = peek(1).kind == TokKind::Identifier;
                        const bool ptr_decl = peek(1).text == "*" && peek(2).kind == TokKind::Identifier;
                        if (next_ident || ptr_decl) {
                            if (!specs.text.empty()) specs.text += " ";
                            specs.text += t.text;
                            specs.has_type = true;
                            any = true;
                            advance();
                            continue;
                        }
                    }
                    break;
                }
                return any;
            }

            void parse_record_body(const std::string &kind, const std::string &name) {
                RecordDecl rec;
                rec.kind = kind;
                rec.name = name;
                rec.line = peek().line;
                expect("{");
                while (!at("}") && !at_eof()) {
                    if (accept(";")) continue;
                    if (peek().kind == TokKind::Preproc) { advance(); continue; }
                    Specs fspec;
                    if (!parse_specs(fspec)) {
                        error("expected a struct member");
                        if (!at_eof()) advance();
                        continue;
                    }
                    if (accept(";")) continue;   // anonymous struct/union
                    do {
                        const size_t before = pos_;
                        std::string fname;
                        std::string ftype = fspec.text;
                        // 成员声明符：收集到 , ; : 为止，记录第一个标识符
                        std::string text;
                        int depth = 0;
                        while (!at_eof()) {
                            const Token &t = peek();
                            if (depth == 0 && (t.text == "," || t.text == ";" || t.text == ":")) break;
                            if (t.text == "(" || t.text == "[") ++depth;
                            else if (t.text == ")" || t.text == "]") --depth;
                            if (fname.empty() && t.kind == TokKind::Identifier) fname = t.text;
                            if (!text.empty() && t.kind != TokKind::Punct) text += " ";
                            text += t.text;
                            advance();
                        }
                        if (fname.empty() && pos_ == before) {
                            error("expected a member declarator");
                            break;
                        }
                        if (!text.empty()) ftype += " " + text;
                        if (accept(":")) parse_conditional();   // 位域
                        rec.fields.push_back({fname, ftype, peek().line});
                    } while (accept(","));
                    expect(";");
                }
                expect("}");
                if (!name.empty()) unit_.records.push_back(std::move(rec));
            }

            Declarator parse_declarator(const Specs &specs) {
                Declarator d;
                std::string text;
                std::string name;
                int depth = 0;
                while (!at_eof()) {
                    const Token &t = peek();
                    if (t.text == "__extension__") { advance(); continue; }
                    if (t.text == "__attribute__" || t.text == "__attribute") { skip_attribute(); continue; }
                    if (depth == 0 && (t.text == "," || t.text == ";" || t.text == "=" || t.text == "{"))
                        break;
                    if (t.text == "(" || t.text == "[") ++depth;
                    else if (t.text == ")" || t.text == "]") --depth;
                    if (name.empty() && t.kind == TokKind::Identifier) name = t.text;
                    if (!text.empty() && t.kind != TokKind::Punct) text += " ";
                    text += t.text;
                    advance();
                }
                d.name = name;
                d.type = specs.text;
                if (!text.empty()) d.type += " " + text;

                // 函数声明：名字后面紧跟 '('（`int (*fp)(int)` 的 '(' 在名字前面，不算）
                // 用声明符原文的紧凑形式判断 `name(`。
                const std::string needle = name + "(";
                std::string compact;
                for (const char ch: text) if (ch != ' ' && ch != '\t') compact += ch;
                if (!name.empty() && compact.find(needle) != std::string::npos) {
                    // 重新扫描声明的 token 序列已不可得；用文本形式解析参数
                    const size_t open = compact.find(needle) + name.size();
                    const size_t close = compact.rfind(')');
                    d.function = true;
                    d.return_type = specs.text;
                    if (open < close) {
                        const std::string inside = compact.substr(open + 1, close - open - 1);
                        if (!inside.empty() && inside != "void") {
                            if (inside.find("...") != std::string::npos) d.variadic = true;
                            // 参数只做展示：按顶层逗号切分
                            size_t start = 0;
                            int pd = 0;
                            for (size_t i = 0; i <= inside.size(); ++i) {
                                if (i == inside.size() || (inside[i] == ',' && pd == 0)) {
                                    const std::string part = inside.substr(start, i - start);
                                    if (!part.empty() && part != "...") {
                                        Parameter prm;
                                        std::string pt = part;
                                        size_t sp = pt.find(' ');
                                        // 最后一个标识符当参数名（类型保留整段）
                                        (void)sp;
                                        prm.type = part;
                                        d.params.push_back(prm);
                                    }
                                    start = i + 1;
                                } else if (inside[i] == '(' || inside[i] == '[') {
                                    ++pd;
                                } else if (inside[i] == ')' || inside[i] == ']') {
                                    --pd;
                                }
                            }
                        }
                    }
                }
                return d;
            }

            void add_declarator(const Specs &specs, const Declarator &d) {
                if (d.name.empty()) return;
                if (specs.is_typedef) {
                    typedefs_.insert(d.name);
                    unit_.typedefs.push_back({d.name, d.type, peek().line});
                    return;
                }
                if (d.function) {
                    FunctionDecl f;
                    f.name = d.name;
                    f.return_type = d.return_type.empty() ? specs.text : d.return_type;
                    f.params = d.params;
                    f.variadic = d.variadic;
                    f.definition = d.definition;
                    f.line = peek().line;
                    unit_.functions.push_back(std::move(f));
                }
            }

            // ---------------- statements ----------------
            void parse_compound() {
                expect("{");
                while (!at("}") && !at_eof()) {
                    if (peek().kind == TokKind::Preproc) { advance(); continue; }
                    parse_statement();
                }
                expect("}");
            }

            void parse_statement() {
                if (is_type_start_here()) {
                    // C99 允许块内声明
                    Specs s;
                    if (parse_specs(s)) {
                        if (!accept(";")) {
                            do {
                                Declarator d = parse_declarator(s);
                                add_declarator(s, d);
                                parse_initializer_opt();
                            } while (accept(","));
                            expect(";");
                        }
                        return;
                    }
                }
                if (at("{")) { parse_compound(); return; }
                if (accept(";")) return;
                if (accept("if")) {
                    expect("("); parse_expression(); expect(")");
                    parse_statement();
                    if (accept("else")) parse_statement();
                    return;
                }
                if (accept("switch")) {
                    expect("("); parse_expression(); expect(")");
                    parse_statement();
                    return;
                }
                if (accept("while")) {
                    expect("("); parse_expression(); expect(")");
                    parse_statement();
                    return;
                }
                if (accept("do")) {
                    parse_statement();
                    expect("while");
                    expect("("); parse_expression(); expect(")");
                    expect(";");
                    return;
                }
                if (accept("for")) {
                    expect("(");
                    if (is_type_start_here()) {
                        Specs s;
                        parse_specs(s);
                        if (!accept(";")) {
                            parse_declarator(s);
                            parse_initializer_opt();
                            while (accept(",")) {
                                parse_declarator(s);
                                parse_initializer_opt();
                            }
                            expect(";");
                        }
                    } else if (!accept(";")) {
                        parse_expression();
                        expect(";");
                    }
                    if (!at(";")) parse_expression();
                    expect(";");
                    if (!at(")")) parse_expression();
                    expect(")");
                    parse_statement();
                    return;
                }
                if (accept("return")) {
                    if (!at(";")) parse_expression();
                    expect(";");
                    return;
                }
                if (accept("break") || accept("continue")) { expect(";"); return; }
                if (accept("goto")) { if (!at_eof()) advance(); expect(";"); return; }
                if (accept("case")) { parse_conditional(); expect(":"); parse_statement(); return; }
                if (accept("default")) { expect(":"); parse_statement(); return; }
                // 标签：ident ':'
                if (peek().kind == TokKind::Identifier && peek(1).text == ":") {
                    advance(); advance(); parse_statement(); return;
                }
                parse_expression();
                expect(";");
            }

            void parse_initializer_opt() {
                if (!accept("=")) return;
                parse_initializer();
            }

            void parse_initializer() {
                if (accept("{")) {
                    while (!at("}") && !at_eof()) {
                        // 设计器 .field = / [idx] =
                        if (accept(".")) { if (!at_eof()) advance(); if (accept("=")) parse_initializer(); }
                        else if (accept("[")) { parse_conditional(); expect("]"); if (accept("=")) parse_initializer(); }
                        else parse_initializer();
                        if (!accept(",")) break;
                    }
                    expect("}");
                    return;
                }
                parse_assignment();
            }

            // ---------------- expressions ----------------
            void parse_expression() {
                parse_assignment();
                while (accept(",")) parse_assignment();
            }

            void parse_assignment() {
                parse_conditional();
                static const std::unordered_set<std::string> ops = {
                    "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|=",
                };
                if (ops.contains(peek().text)) {
                    advance();
                    parse_assignment();
                }
            }

            void parse_conditional() {
                parse_binary(0);
                if (accept("?")) {
                    parse_expression();
                    expect(":");
                    parse_conditional();
                }
            }

            static int precedence(const std::string &op) {
                if (op == "||") return 1;
                if (op == "&&") return 2;
                if (op == "|") return 3;
                if (op == "^") return 4;
                if (op == "&") return 5;
                if (op == "==" || op == "!=") return 6;
                if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
                if (op == "<<" || op == ">>") return 8;
                if (op == "+" || op == "-") return 9;
                if (op == "*" || op == "/" || op == "%") return 10;
                return -1;
            }

            void parse_binary(int min_prec) {
                parse_unary();
                for (;;) {
                    const int prec = precedence(peek().text);
                    if (prec < min_prec) break;
                    advance();
                    parse_binary(prec + 1);
                }
            }

            void parse_unary() {
                if (at("++") || at("--") || at("&") || at("*") || at("+") || at("-") ||
                    at("~") || at("!")) {
                    advance();
                    parse_unary();
                    return;
                }
                if (accept("sizeof")) {
                    if (accept("(")) {
                        if (is_type_start_here()) {
                            parse_type_name();
                            expect(")");
                        } else {
                            parse_expression();
                            expect(")");
                        }
                    } else {
                        parse_unary();
                    }
                    return;
                }
                // 强制转换 / 复合字面量
                if (at("(")) {
                    const size_t save = pos_;
                    advance();
                    if (is_type_start_here()) {
                        parse_type_name();
                        if (accept(")")) {
                            if (at("{")) parse_initializer();
                            else parse_unary();
                            return;
                        }
                    }
                    pos_ = save;
                }
                parse_postfix();
            }

            void parse_postfix() {
                parse_primary();
                for (;;) {
                    if (accept("[")) { parse_expression(); expect("]"); }
                    else if (accept("(")) {
                        if (!at(")")) {
                            do { parse_assignment(); } while (accept(","));
                        }
                        expect(")");
                    } else if (accept(".") || accept("->")) {
                        if (!at_eof()) advance();
                    } else if (accept("++") || accept("--")) {
                    } else break;
                }
            }

            void parse_primary() {
                const Token &t = peek();
                if (t.kind == TokKind::IntLiteral || t.kind == TokKind::FloatLiteral ||
                    t.kind == TokKind::StringLiteral || t.kind == TokKind::CharLiteral ||
                    t.kind == TokKind::Identifier) {
                    advance();
                    return;
                }
                if (accept("(")) {
                    parse_expression();
                    expect(")");
                    return;
                }
                error("expected an expression, found '" + t.text + "'");
                if (!at_eof()) advance();
            }

            void parse_type_name() {
                Specs s;
                if (!parse_specs(s)) {
                    error("expected a type name");
                    return;
                }
                // 抽象声明符：* / [ ] / ( )
                int depth = 0;
                while (!at_eof()) {
                    const Token &t = peek();
                    if (depth == 0 && (t.text == ")" || t.text == "," || t.text == ";" || t.text == "{"))
                        break;
                    if (t.text == "(" || t.text == "[") ++depth;
                    else if (t.text == ")" || t.text == "]") --depth;
                    advance();
                }
            }
        };
    } // namespace

    Unit parse(const std::string &source) {
        Parser parser(lex(source));
        return parser.run();
    }
} // namespace sinter::cfront2

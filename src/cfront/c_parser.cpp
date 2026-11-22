// cfront: c_parser.cpp —— 解析器主体：游标、顶层、类型说明符、声明符
//
// 声明符解析是 C 语法的难点。核心思路（标准做法）：
//   parse_declarator := '*'* parse_direct_declarator
//   parse_direct_declarator :=
//       '(' parse_declarator ')'       <- 带括号，改变优先级（(*f)）
//     | identifier?
//     | epsilon
//   然后循环吃后缀 '[' ... ']' 与 '(' 形参 ')'
// 这样 int (*f)(int) 会得到 Paren{Pointer{Identifier(f)}} 再包一层 Function，
// 语义上就是“f 是指向「返回 int 的函数」的指针”。
#include "cfront/c_parser.hpp"

#include "cfront/c_preproc.hpp"

#include <cstdio>
#include <cstdlib>

namespace sinter::cfront {
    namespace {
        bool is_type_spec_kind(const CTokKind k) {
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
                case CTokKind::KwComplex:
                case CTokKind::KwStruct:
                case CTokKind::KwUnion:
                case CTokKind::KwEnum:
                case CTokKind::KwConst:
                case CTokKind::KwVolatile:
                case CTokKind::KwRestrict:
                case CTokKind::KwStatic:
                case CTokKind::KwExtern:
                case CTokKind::KwInline:
                case CTokKind::KwRegister:
                case CTokKind::KwAuto:
                case CTokKind::KwTypedef:
                case CTokKind::KwAlignas:
                case CTokKind::KwAtomic:
                case CTokKind::KwThreadLocal:
                case CTokKind::KwNoreturn:
                case CTokKind::Attribute: return true;
                default: return false;
            }
        }
    } // namespace

    // ---------------------------------------------------------------- 生命周期
    CParser::CParser(CTokens toks, std::string source, std::string path)
        : toks_(std::move(toks)), src_(std::move(source)), path_(std::move(path)) {
        // 游标依赖「末尾永远是 EndOfFile」这个不变量（peek 越界时返回 back()）。
        // 预处理后的 token 流特意不带 EOF，这里补一个；否则 at_end() 恒为假，
        // parse_unit 会在最后一个 token 上死循环（这个 bug 真的发生过）。
        if (toks_.empty() || toks_.back().kind != CTokKind::EndOfFile) {
            CToken eof;
            eof.kind = CTokKind::EndOfFile;
            eof.offset = src_.size();
            eof.line = toks_.empty() ? 1 : toks_.back().line;
            eof.column = toks_.empty() ? 1 : toks_.back().column;
            toks_.push_back(eof);
        }
    }

    CParseResult CParser::parse(const std::string &source, const std::string &path,
                                const CParseOptions &options) {
        return parse_with_typedefs(source, {}, path, options);
    }

    CParseResult CParser::parse_with_typedefs(const std::string &source,
                                              const std::set<std::string> &known,
                                              const std::string &path,
                                              const CParseOptions &options) {
        CLexer lx(source);
        lx.keep_comments(false);
        lx.merge_preproc(true);
        CLexResult lex = lx.tokenize();

        // 预处理：条件编译 + 宏展开 + #include。关掉时退回「预处理行原样跳过」的旧行为
        // （格式化器与「只要结构」的 embed C 块需要它）。
        CPreprocResult pre;
        if (options.preprocess) {
            CPreprocOptions po;
            po.include_dirs = options.include_dirs;
            po.predefined = options.predefined;
            pre = preprocess_c(source, path, po);
        }

        CParser p(options.preprocess ? pre.tokens : std::move(lex.tokens), source, path);
        p.files_ = pre.files;
        // 未开预处理时 files 为空，但主文件仍然要占 0 号（否则错误没有文件归属）。
        if (p.files_.empty()) p.files_.push_back(path);
        // 预置标准头里的常用 typedef 名字：不做 #include 时也能认得 int64_t，
        // 否则它会被当成变量名。
        static const char *kBuiltinTypedefs[] = {
            "int8_t", "int16_t", "int32_t", "int64_t",
            "uint8_t", "uint16_t", "uint32_t", "uint64_t",
            "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
            "size_t", "ssize_t", "ptrdiff_t", "wchar_t", "wint_t",
            "FILE", "va_list", "time_t", "off_t", "bool",
            "int_least8_t", "int_fast8_t", "int_fast32_t", "int_fast64_t",
            "uint_least8_t", "uint_fast8_t", "uint_fast32_t", "uint_fast64_t",
            // 编译器预置类型：sinter 后端会自行 typedef 出这些名字，
            // 它们不在 embed C 块里但对块内代码可见。
            "sinter_string",
            // 编译器预置的「类型名」，头文件里会直接拿来 typedef 或声明参数：
            //   typedef __builtin_va_list va_list;          （musl 的 bits/alltypes.h）
            //   void f(__int128 x);  long double _Float128;
            // 不认这些名字，解析器会把它们当声明符，报「expected ';'」。
            "__builtin_va_list", "__builtin_ms_va_list", "__builtin_va_list_tag",
            "__int128", "__int128_t", "__uint128_t",
            "__float128", "_Float16", "_Float32", "_Float64", "_Float128", "__fp16",
        };
        p.typedefs_ = known;
        for (const char *t: kBuiltinTypedefs) p.typedefs_.insert(t);
        p.parse_unit();

        CParseResult r;
        r.unit.source_path = path;
        r.unit.source_text = source;
        r.unit.items = std::move(p.unit_items_);
        r.errors = std::move(p.errors_);
        r.preproc_errors = std::move(pre.errors);
        r.unresolved_includes = std::move(pre.unresolved_includes);
        r.macros = std::move(pre.macros);
        r.files = pre.files;
        // 未开预处理时，词法错误需单独并进来；开启时 preprocess_c 已经收过一遍。
        if (!options.preprocess)
            for (const auto &e: lex.errors) {
                CParseError ce;
                ce.message = e.message;
                ce.line = e.line;
                ce.column = e.column;
                ce.file = path;
                r.errors.push_back(std::move(ce));
            }
        return r;
    }

    // ---------------------------------------------------------------- 游标
    const CToken &CParser::peek(const size_t k) const {
        const size_t i = pos_ + k;
        if (i < toks_.size()) return toks_[i];
        return toks_.back();   // 末尾永远是 EndOfFile
    }

    const CToken &CParser::advance() {
        const CToken &t = peek();
        if (pos_ + 1 < toks_.size()) ++pos_;
        return t;
    }

    bool CParser::check(const CTokKind k) const { return peek().kind == k; }
    bool CParser::at_end() const { return peek().kind == CTokKind::EndOfFile; }

    bool CParser::match(const CTokKind k) {
        if (!check(k)) return false;
        advance();
        return true;
    }

    bool CParser::match_any(std::initializer_list<CTokKind> ks) {
        for (const auto k: ks)
            if (check(k)) {
                advance();
                return true;
            }
        return false;
    }

    const CToken *CParser::consume(const CTokKind k, const char *what) {
        if (check(k)) return &advance();
        error_at(peek(), std::string("expected") + what);
        return nullptr;
    }

    SourceRange CParser::span_of(const CToken &t) const {
        SourceRange r;
        r.begin_line = t.line;
        r.begin_col = t.column;
        r.begin_offset = t.offset;
        r.end_line = t.line;
        r.end_col = t.column + static_cast<uint32_t>(t.text.size());
        r.end_offset = t.offset + t.text.size();
        return r;
    }

    void CParser::error_at(const CToken &t, const std::string &msg) {
        // 同一个位置只报一次，避免容错时刷屏
        if (!errors_.empty() && errors_.back().line == t.line &&
            errors_.back().column == t.column &&
            errors_.back().file == (t.file_id < files_.size() ? files_[t.file_id] : std::string()))
            return;
        CParseError e;
        e.message = msg;
        e.line = t.line;
        e.column = t.column;
        if (t.file_id < files_.size()) e.file = files_[t.file_id];
        errors_.push_back(std::move(e));
    }

    void CParser::skip_to_recovery() {
        int depth = 0;
        while (!at_end()) {
            const CTokKind k = peek().kind;
            if (k == CTokKind::LBrace) ++depth;
            else if (k == CTokKind::RBrace) {
                if (depth == 0) return;   // 交给上层吃掉
                --depth;
                if (depth == 0) {
                    advance();
                    return;
                }
            } else if (k == CTokKind::Semicolon && depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    // ---------------------------------------------------------------- 顶层
    void CParser::parse_unit() {
        // parse_unit 把结果写进 items_，由 parse_with_typedefs 取走
        unit_items_.clear();
        while (!at_end()) {
            const size_t before = pos_;
            CUnitItem item;
            if (!parse_unit_item(item)) {
                if (pos_ == before) advance();
                continue;
            }
            unit_items_.push_back(std::move(item));
        }
    }

    bool CParser::parse_unit_item(CUnitItem &item) {
        if (check(CTokKind::PreprocLine)) {
            const CToken &t = advance();
            item.kind = CUnitItem::Kind::Preproc;
            item.preproc_text = t.text;
            item.preproc_directive = t.directive;
            item.range = span_of(t);
            return true;
        }
        // 声明前的扩展说明符：`__attribute__((...)) __declspec(...)`
        while (check(CTokKind::Attribute)) advance();

        // `extern "C" { ... }`：C++ 头里包 C 声明的标准形态。
        // 里面的东西仍然是 C 声明，所以直接递归解析并把结果展平到当前单元。
        if (check(CTokKind::KwExtern) && peek(1).kind == CTokKind::StringLiteral) {
            if (peek(2).kind == CTokKind::LBrace) {
                advance();   // extern
                advance();   // "C"
                advance();   // {
                while (!at_end() && !check(CTokKind::RBrace)) {
                    const size_t before = pos_;
                    CUnitItem inner;
                    if (parse_unit_item(inner)) unit_items_.push_back(std::move(inner));
                    if (pos_ == before) advance();
                }
                match(CTokKind::RBrace);
                return false;
            }
            // 单条 `extern "C" int f(void);`
            advance();
            advance();
        }

        // `_Static_assert(cond, "msg");`：只收集条件与消息，不进入运行时
        if (check(CTokKind::KwStaticAssert)) {
            const CToken &kw = advance();
            item.kind = CUnitItem::Kind::StaticAssert;
            item.range = span_of(kw);
            if (match(CTokKind::LParen)) {
                std::string cond;
                int depth = 0;
                while (!at_end()) {
                    const CTokKind kk = peek().kind;
                    if (kk == CTokKind::LParen) { ++depth; advance(); continue; }
                    if (kk == CTokKind::RParen) { if (depth == 0) break; --depth; advance(); continue; }
                    if (kk == CTokKind::Comma && depth == 0) break;
                    if (!cond.empty()) cond += ' ';
                    cond += peek().text;
                    advance();
                }
                item.assert_cond_text = cond;
                if (match(CTokKind::Comma) && check(CTokKind::StringLiteral))
                    item.assert_msg = advance().text;
                int depth2 = 0;
                while (!at_end()) {
                    if (check(CTokKind::LParen)) ++depth2;
                    else if (check(CTokKind::RParen)) {
                        if (depth2 == 0) { advance(); break; }
                        --depth2;
                    }
                    advance();
                }
            }
            match(CTokKind::Semicolon);
            return true;
        }

        if (match(CTokKind::Semicolon)) return false;   // 空声明
        return parse_decl_or_func(item);
    }

    bool CParser::parse_decl_or_func(CUnitItem &item) {
        CDeclItem di;
        di.range = span_of(peek());

        // 存储类可能出现在任意位置，先扫一遍记录语义标志，再正式解析类型
        for (size_t i = pos_; i < toks_.size(); ++i) {
            const CTokKind k = toks_[i].kind;
            if (k == CTokKind::KwTypedef) di.is_typedef = true;
            if (k == CTokKind::KwExtern) di.is_extern = true;
            if (k == CTokKind::KwStatic) di.is_static = true;
            if (k == CTokKind::KwInline) di.is_inline = true;
            // 只在声明头部扫描，遇到 ; { } = 停
            if (k == CTokKind::Semicolon || k == CTokKind::LBrace ||
                k == CTokKind::Assign || k == CTokKind::RBrace)
                break;
        }
        parse_type_spec(di.spec);

        if (check(CTokKind::Semicolon)) {
            // struct Foo { ... }; 这种仅定义类型的声明
            advance();
            item.kind = CUnitItem::Kind::Declaration;
            item.decl = std::move(di);
            return true;
        }

        di.decl = parse_declarator(false);
        if (!di.decl) {
            error_at(peek(), "expected declarator");
            skip_to_recovery();
            return false;
        }
        di.range.end_offset = peek().offset;

        if (di.is_typedef) typedefs_.insert(di.name());

        // 声明符之后的扩展说明符：`int f(void) __attribute__((noreturn));`。
        // 属性对结构收集没有影响，跳过即可；不跳过会被当成「缺分号」而报错。
        while (check(CTokKind::Attribute)) advance();

        // 函数定义：声明符是 Function 且后面紧跟 '{'
        if (check(CTokKind::LBrace)) {
            auto fd = std::make_shared<CFuncDef>();
            fd->decl = std::move(di);
            fd->body = parse_compound();
            item.kind = CUnitItem::Kind::FunctionDef;
            item.func = std::move(fd);
            return true;
        }

        // 普通声明：可能有初值，逗号分隔多个
        if (match(CTokKind::Assign)) di.init = parse_assign();

        item.kind = CUnitItem::Kind::Declaration;
        item.decl = std::move(di);
        item.range = item.decl.range;

        // 同一行声明多个变量（int a, b;）：逐个记录，但共用类型
        while (match(CTokKind::Comma)) {
            CUnitItem extra;
            extra.kind = CUnitItem::Kind::Declaration;
            extra.decl.spec = item.decl.spec;
            extra.decl.range = span_of(peek());
            extra.decl.decl = parse_declarator(false);
            if (match(CTokKind::Assign)) extra.decl.init = parse_assign();
            if (extra.decl.decl) unit_items_.push_back(std::move(extra));
        }

        if (!match(CTokKind::Semicolon)) {
            error_at(peek(), "expected ';'");
            skip_to_recovery();
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- 类型说明符
    bool CParser::parse_type_spec(TypeSpec &spec) {
        bool any = false;
        spec.range = span_of(peek());
        while (true) {
            const CTokKind k = peek().kind;
            if (!is_type_spec_kind(k)) {
                // 可能是 typedef 名字
                if (k == CTokKind::Identifier && spec.kinds.empty() &&
                    typedefs_.count(peek().text)) {
                    spec.kinds.push_back(TypeSpecKind::TypedefName);
                    spec.typedef_name = advance().text;
                    any = true;
                    continue;
                }
                break;
            }
            switch (k) {
                case CTokKind::KwConst:
                case CTokKind::KwVolatile:
                case CTokKind::KwRestrict:
                    advance();   // 限定符不影响我们收集的符号
                    any = true;
                    break;
                case CTokKind::KwStatic: advance(); any = true; break;
                case CTokKind::KwExtern: advance(); any = true; break;
                case CTokKind::KwInline: advance(); any = true; break;
                case CTokKind::KwRegister: advance(); any = true; break;
                case CTokKind::KwTypedef: advance(); any = true; break;
                case CTokKind::KwThreadLocal: advance(); any = true; break;
                case CTokKind::KwNoreturn: advance(); any = true; break;
                case CTokKind::KwAlignas:
                    advance();
                    if (match(CTokKind::LParen)) {
                        int d = 1;
                        while (!at_end() && d > 0) {
                            if (check(CTokKind::LParen)) ++d;
                            else if (check(CTokKind::RParen)) --d;
                            advance();
                        }
                    }
                    any = true;
                    break;
                case CTokKind::Attribute: advance(); any = true; break;
                case CTokKind::KwAtomic:
                    advance();
                    // `_Atomic(T)` 与 `_Atomic T`：括号里的类型才是真正的类型
                    if (match(CTokKind::LParen)) {
                        parse_type_spec(spec);
                        while (check(CTokKind::Star)) advance();
                        match(CTokKind::RParen);
                    }
                    any = true;
                    break;
                case CTokKind::KwVoid: advance(); spec.kinds.push_back(TypeSpecKind::Void); any = true; break;
                case CTokKind::KwChar: advance(); spec.kinds.push_back(TypeSpecKind::Char); any = true; break;
                case CTokKind::KwShort: advance(); spec.kinds.push_back(TypeSpecKind::Short); any = true; break;
                case CTokKind::KwInt: advance(); spec.kinds.push_back(TypeSpecKind::Int); any = true; break;
                case CTokKind::KwLong: advance(); spec.kinds.push_back(TypeSpecKind::Long); any = true; break;
                case CTokKind::KwFloat: advance(); spec.kinds.push_back(TypeSpecKind::Float); any = true; break;
                case CTokKind::KwDouble: advance(); spec.kinds.push_back(TypeSpecKind::Double); any = true; break;
                case CTokKind::KwSigned: advance(); spec.kinds.push_back(TypeSpecKind::Signed); any = true; break;
                case CTokKind::KwUnsigned: advance(); spec.kinds.push_back(TypeSpecKind::Unsigned); any = true; break;
                case CTokKind::KwBool: advance(); spec.kinds.push_back(TypeSpecKind::Bool); any = true; break;
                case CTokKind::KwComplex: advance(); spec.kinds.push_back(TypeSpecKind::Complex); any = true; break;
                case CTokKind::KwAuto: advance(); spec.kinds.push_back(TypeSpecKind::Auto); any = true; break;
                case CTokKind::KwStruct:
                case CTokKind::KwUnion:
                case CTokKind::KwEnum:
                    parse_struct_or_enum(spec);
                    any = true;
                    break;
                default: break;
            }
        }
        return any;
    }

    bool CParser::parse_struct_or_enum(TypeSpec &spec) {
        const bool is_union = check(CTokKind::KwUnion);
        const bool is_enum = check(CTokKind::KwEnum);
        advance();
        spec.kinds.push_back(is_enum ? TypeSpecKind::Enum
                                     : (is_union ? TypeSpecKind::Union : TypeSpecKind::Struct));
        if (check(CTokKind::Identifier)) spec.tag = advance().text;

        if (!check(CTokKind::LBrace)) return true;   // 前向声明 / 仅引用

        advance();   // {
        auto def = std::make_shared<CStructDef>();
        def->tag = spec.tag;
        def->is_union = is_union;
        def->is_enum = is_enum;
        def->range = span_of(peek());

        if (is_enum) {
            while (!at_end() && !check(CTokKind::RBrace)) {
                if (check(CTokKind::Identifier)) {
                    CField f;
                    f.range = span_of(peek());
                    f.decl = std::make_unique<CDeclarator>();
                    f.decl->kind = CDeclarator::Kind::Identifier;
                    f.decl->name = advance().text;
                    if (match(CTokKind::Assign)) {
                        std::string v;
                        int depth = 0;
                        while (!at_end()) {
                            const CTokKind kk = peek().kind;
                            if (kk == CTokKind::LParen) ++depth;
                            else if (kk == CTokKind::RParen) { if (depth == 0) break; --depth; }
                            else if (kk == CTokKind::Comma && depth == 0) break;
                            // 最后一项没有尾逗号时，'}' / ';' 就是结束信号
                            else if (depth == 0 &&
                                     (kk == CTokKind::RBrace || kk == CTokKind::Semicolon)) break;
                            if (!v.empty()) v += ' ';
                            v += peek().text;
                            advance();
                        }
                        f.value_text = v;
                    }
                    def->enumerators.push_back(std::move(f));
                }
                if (!match(CTokKind::Comma)) break;
            }
            match(CTokKind::RBrace);
        } else {
            while (!at_end() && !check(CTokKind::RBrace)) {
                const size_t before = pos_;
                CField f;
                f.range = span_of(peek());
                parse_type_spec(f.spec);
                if (check(CTokKind::Semicolon)) {
                    advance();   // 匿名 struct 成员
                    continue;
                }
                f.decl = parse_declarator(true);
                if (!f.decl) {
                    if (pos_ == before) advance();
                    continue;
                }
                // 位域 : N（宽度原文要留给 Sinter 侧的 ABI 映射）
                if (match(CTokKind::Colon)) {
                    std::string w;
                    int depth = 0;
                    while (!at_end()) {
                        const CTokKind kk = peek().kind;
                        if (kk == CTokKind::LParen || kk == CTokKind::LBracket) { ++depth; advance(); continue; }
                        if (kk == CTokKind::RParen || kk == CTokKind::RBracket) {
                            if (depth == 0) break;
                            --depth; advance(); continue;
                        }
                        if ((kk == CTokKind::Comma || kk == CTokKind::Semicolon ||
                             kk == CTokKind::RBrace) && depth == 0) break;
                        if (!w.empty()) w += ' ';
                        w += peek().text;
                        advance();
                    }
                    f.bit_width = w;
                }
                def->fields.push_back(std::move(f));
                while (match(CTokKind::Comma)) {
                    CField g;
                    g.spec = def->fields.back().spec;
                    g.range = span_of(peek());
                    g.decl = parse_declarator(true);
                    if (match(CTokKind::Colon)) {
                        std::string w;
                        int depth = 0;
                        while (!at_end()) {
                            const CTokKind kk = peek().kind;
                            if (kk == CTokKind::LParen || kk == CTokKind::LBracket) { ++depth; advance(); continue; }
                            if (kk == CTokKind::RParen || kk == CTokKind::RBracket) {
                                if (depth == 0) break;
                                --depth; advance(); continue;
                            }
                            if ((kk == CTokKind::Comma || kk == CTokKind::Semicolon) && depth == 0) break;
                            if (!w.empty()) w += ' ';
                            w += peek().text;
                            advance();
                        }
                        g.bit_width = w;
                    }
                    if (g.decl) def->fields.push_back(std::move(g));
                }
                if (!match(CTokKind::Semicolon)) {
                    if (pos_ == before) advance();
                    else break;
                }
            }
            match(CTokKind::RBrace);
        }
        spec.struct_def = std::move(def);
        return true;
    }

    // ---------------------------------------------------------------- 声明符
    std::unique_ptr<CDeclarator> CParser::parse_declarator(const bool allow_abstract) {
        if (check(CTokKind::Star)) {
            auto ptr = std::make_unique<CDeclarator>();
            ptr->kind = CDeclarator::Kind::Pointer;
            ptr->range = span_of(peek());
            advance();
            // 指针限定符
            while (check(CTokKind::KwConst) || check(CTokKind::KwRestrict) ||
                   check(CTokKind::KwVolatile)) {
                if (check(CTokKind::KwConst)) ptr->is_const_ptr = true;
                if (check(CTokKind::KwRestrict)) ptr->is_restrict_ptr = true;
                advance();
            }
            // 指针也可以是多级：**p
            ptr->inner = parse_declarator(allow_abstract);
            return ptr;
        }
        return parse_direct_declarator(allow_abstract);
    }

    std::unique_ptr<CDeclarator> CParser::parse_direct_declarator(const bool allow_abstract) {
        std::unique_ptr<CDeclarator> base;

        if (check(CTokKind::LParen)) {
            // 区分：'( 形参表 )' 的抽象声明符 vs '(*f)' 的括号声明符
            const CToken &n1 = peek(1);
            const bool paren_declarator =
                    n1.kind == CTokKind::Star || (n1.kind == CTokKind::LParen) ||
                    (n1.kind == CTokKind::Identifier && !typedefs_.count(n1.text));
            if (paren_declarator) {
                advance();   // (
                auto par = std::make_unique<CDeclarator>();
                par->kind = CDeclarator::Kind::Paren;
                par->range = span_of(peek());
                par->inner = parse_declarator(allow_abstract);
                consume(CTokKind::RParen, "')'");
                base = std::move(par);
            }
        }

        if (!base && check(CTokKind::Identifier)) {
            auto id = std::make_unique<CDeclarator>();
            id->kind = CDeclarator::Kind::Identifier;
            id->range = span_of(peek());
            id->name = advance().text;
            base = std::move(id);
        }

        if (!base) {
            if (!allow_abstract) return nullptr;
            auto id = std::make_unique<CDeclarator>();
            id->kind = CDeclarator::Kind::Identifier;
            id->range = span_of(peek());
            base = std::move(id);
        }

        // 后缀：数组与函数参数，可重复出现
        while (true) {
            if (check(CTokKind::LBracket)) {
                advance();
                auto arr = std::make_unique<CDeclarator>();
                arr->kind = CDeclarator::Kind::Array;
                arr->range = span_of(peek());
                // static / const 在 [ ] 内
                while (check(CTokKind::KwStatic) || check(CTokKind::KwConst) ||
                       check(CTokKind::KwRestrict) || check(CTokKind::KwVolatile)) {
                    if (check(CTokKind::KwStatic)) arr->array_is_static = true;
                    advance();
                }
                // 数组长度：原文收集（可能是宏或表达式）
                const size_t start = pos_;
                int depth = 0;
                while (!at_end()) {
                    if (check(CTokKind::LBracket) || check(CTokKind::LParen)) ++depth;
                    else if (check(CTokKind::RBracket) && depth == 0) break;
                    else if (check(CTokKind::RParen) && depth > 0) --depth;
                    advance();
                }
                for (size_t i = start; i < pos_; ++i) arr->array_size_text += toks_[i].text;
                consume(CTokKind::RBracket, "']'");
                arr->inner = std::move(base);
                base = std::move(arr);
                continue;
            }
            if (check(CTokKind::LParen)) {
                advance();
                auto fn = std::make_unique<CDeclarator>();
                fn->kind = CDeclarator::Kind::Function;
                fn->range = span_of(peek());
                if (!parse_params(fn->params, fn->variadic)) {
                    // 形参解析失败，仍然保留函数节点
                }
                consume(CTokKind::RParen, "')'");
                fn->inner = std::move(base);
                base = std::move(fn);
                continue;
            }
            break;
        }
        return base;
    }

    bool CParser::parse_params(std::vector<CParam> &out, bool &variadic) {
        variadic = false;
        if (check(CTokKind::RParen)) return true;
        // (void) 视为无参
        if (check(CTokKind::KwVoid) && peek(1).kind == CTokKind::RParen) {
            advance();
            return true;
        }
        while (!at_end()) {
            if (match(CTokKind::Ellipsis)) {
                variadic = true;
                break;
            }
            CParam p;
            parse_type_spec(p.spec);
            p.decl = parse_declarator(true);
            // 函数指针形参 void (*cb)(int) 的 decl 已经完整
            out.push_back(std::move(p));
            if (!match(CTokKind::Comma)) break;
        }
        return true;
    }

    bool CParser::looks_like_decl_start() const {
        const CTokKind k = peek().kind;
        if (is_type_spec_kind(k)) return true;
        if (k == CTokKind::Identifier) {
            const CTokKind n = peek(1).kind;
            // 「标识符 标识符」或「标识符 *」视为声明：这样未收录的自定义
            // 类型名也能被接受，而不是误判为表达式语句。
            return n == CTokKind::Identifier || n == CTokKind::Star;
        }
        return false;
    }
} // namespace sinter::cfront
// cfront: c_format.cpp —— C 格式化实现
//
// 核心是一个「行缓冲 + 缩进状态机」：
//   - 逐 token 扫描，根据 token 种类决定与前一个 token 之间要不要空格
//   - 遇到用户换行就 flush 一行，按当前缩进层级写前缀
//   - '{' 之后层级 +1，'}' 之前层级 -1，case/default 特殊处理
// 预处理行整行原样输出（宏体不能随便改缩进）。
#include "cfront/c_format.hpp"

#include "cfront/c_lexer.hpp"

#include <string>
#include <vector>

namespace sinter::cfront {
    namespace {
        /// 关键字后紧跟 '(' 时留空格（if/while/for/switch/return 等控制流写法），
        /// 而标识符/右括号后紧跟 '(' 紧贴（函数调用与声明：main(void)、y()）。
        bool is_control_keyword(const CTokKind k) {
            switch (k) {
                case CTokKind::KwIf:
                case CTokKind::KwWhile:
                case CTokKind::KwFor:
                case CTokKind::KwSwitch:
                case CTokKind::KwReturn:
                case CTokKind::KwSizeof:
                case CTokKind::KwAlignof:
                case CTokKind::KwDo:
                case CTokKind::KwElse: return true;
                default: return false;
            }
        }

        /// 这些 token 前不加空格
        bool no_space_before(const CTokKind k) {
            switch (k) {
                case CTokKind::RParen:
                case CTokKind::RBracket:
                case CTokKind::Semicolon:
                case CTokKind::Comma:
                case CTokKind::Dot:
                case CTokKind::Arrow:
                case CTokKind::PlusPlus:
                case CTokKind::MinusMinus:
                // 成员访问与函数指针的括号：`(*fp)(a)` 里 `)` 后紧贴 `(`，
                // `a[i]` 里 `]` 后紧贴 `[`，都不该插空格
                case CTokKind::LBracket: return true;
                default: return false;
            }
        }

        /// 这些 token 后不加空格
        bool no_space_after(const CTokKind k) {
            switch (k) {
                case CTokKind::LParen:
                case CTokKind::LBracket:
                case CTokKind::Dot:
                case CTokKind::Arrow:
                case CTokKind::Bang:
                case CTokKind::Tilde: return true;
                default: return false;
            }
        }

        /// 声明/表达式里的指针星号：`int *p`、`char *s`、`struct T *q`。
        /// 判断依据是「星号前是类型关键字或 typedef 名（标识符），且再往前是类型起点」。
        /// 这里只处理最稳妥的一种：星号紧跟已知类型关键字（int/char/void/...）。
        bool is_type_keyword(const CTokKind k) {
            switch (k) {
                case CTokKind::KwInt:
                case CTokKind::KwChar:
                case CTokKind::KwVoid:
                case CTokKind::KwFloat:
                case CTokKind::KwDouble:
                case CTokKind::KwLong:
                case CTokKind::KwShort:
                case CTokKind::KwSigned:
                case CTokKind::KwUnsigned:
                case CTokKind::KwConst:
                case CTokKind::KwVolatile:
                case CTokKind::KwStatic:
                case CTokKind::KwExtern:
                case CTokKind::KwStruct:
                case CTokKind::KwUnion:
                case CTokKind::KwEnum:
                case CTokKind::KwRegister:
                case CTokKind::KwInline: return true;
                default: return false;
            }
        }

        /// 属于「表达式语境」的 token：出现在这些 token 之后的 `+ - * & ! ~` 才可能是一元。
        /// 反过来，若前一个 token 是 `return`、`(`、`,`、`=`、运算符等，
        /// 说明当前是表达式的开头，`- 1` 就是一元负号。
        /// 注意 `return- 1` 这类错误的根源是没把 `return` 当成「表达式起始」处理。
        bool starts_expression(const CTokKind k) {
            switch (k) {
                // 语句/表达式起始
                case CTokKind::KwReturn:
                case CTokKind::KwCase:
                case CTokKind::KwSizeof:
                case CTokKind::KwAlignof:
                case CTokKind::KwIf:
                case CTokKind::KwWhile:
                case CTokKind::KwSwitch:
                case CTokKind::KwDo:
                case CTokKind::KwElse:
                // 运算符与分隔符之后
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
                case CTokKind::ShrAssign:
                case CTokKind::Comma:
                case CTokKind::Semicolon:
                case CTokKind::LParen:
                case CTokKind::LBracket:
                case CTokKind::LBrace:
                case CTokKind::Colon:
                case CTokKind::Question:
                case CTokKind::Plus:
                case CTokKind::Minus:
                case CTokKind::Star:
                case CTokKind::Slash:
                case CTokKind::Percent:
                case CTokKind::Amp:
                case CTokKind::Pipe:
                case CTokKind::Caret:
                case CTokKind::Bang:
                case CTokKind::Tilde:
                case CTokKind::AmpAmp:
                case CTokKind::PipePipe:
                case CTokKind::EqualEqual:
                case CTokKind::NotEqual:
                case CTokKind::Less:
                case CTokKind::Greater:
                case CTokKind::LessEqual:
                case CTokKind::GreaterEqual:
                case CTokKind::Shl:
                case CTokKind::Shr: return true;
                default: return false;
            }
        }

        /// 判断「第 i 个 token 之前是否已有空行」。
        /// 直接看原文：上一个 token 结束到本 token 开始之间是否存在连续两个换行。
        /// 这比推导累计换行数更可靠——累计计数会被预处理行、续行等分支打乱。
        bool has_blank_before(const std::string &src, const CTokens &toks, const size_t i) {
            if (i == 0 || i >= toks.size()) return false;
            const size_t from = toks[i - 1].end_offset();
            const size_t to = toks[i].offset;
            if (from >= to || to > src.size()) return false;
            int newlines = 0;
            for (size_t p = from; p < to; ++p) {
                if (src[p] == '\n') {
                    if (++newlines >= 2) return true;
                } else if (src[p] != ' ' && src[p] != '\t' && src[p] != '\r') {
                    // 中间夹了别的内容（理论上不会发生），保守判否
                    return false;
                }
            }
            return false;
        }

        /// 判断「第 i 个 token 之后是否紧跟空行」，与 has_blank_before 同源。
        bool has_blank_after(const std::string &src, const CTokens &toks, const size_t i) {
            return has_blank_before(src, toks, i + 1);
        }

        bool ends_with_label(const std::string &s) {
            // 行尾是 "case ...:" 或 "default:"
            if (s.empty()) return false;
            for (auto it = s.rbegin(); it != s.rend(); ++it) {
                if (*it == ':') return true;
                if (*it != ' ' && *it != '\t') return false;
            }
            return false;
        }
    } // namespace

    namespace {
        struct Emitter {
            const CFormatOptions &opt;
            std::string out;
            std::string line;
            int indent = 0;
            int pending_blank = 0;
            bool at_line_start = true;
            // switch 内部状态：进入 switch 的 '{' 后置 true，用来给 case 减一级
            std::vector<bool> brace_is_switch;

            explicit Emitter(const CFormatOptions &o) : opt(o) {}

            std::string ind(int levels) const {
                if (levels < 0) levels = 0;
                if (opt.use_tabs) return std::string(static_cast<size_t>(levels), '\t');
                return std::string(static_cast<size_t>(levels * opt.indent_width), ' ');
            }

            void begin_line_if_needed() {
                if (at_line_start) {
                    line = ind(indent);
                    at_line_start = false;
                }
            }

            void flush_line() {
                // 去掉行尾空白
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
                if (!line.empty()) {
                    // 只有「真正的空行」才补空行。这里 pending_blank 原本被当成「之前有换行」，
                    // 导致每个普通换行都变成空行；现在由调用方只在出现空行时置 true。
                    if (pending_blank > 0 && opt.keep_line_breaks && !out.empty()) {
                        const int n = pending_blank > opt.max_blank_lines ? opt.max_blank_lines
                                                                         : pending_blank;
                        for (int i = 0; i < n; ++i) out += '\n';
                    }
                    out += line;
                    out += '\n';
                    pending_blank = 0;
                }
                line.clear();
                at_line_start = true;
            }

            void append(const std::string &text) {
                begin_line_if_needed();
                line += text;
            }

            void append_space() {
                if (line.empty()) return;
                if (line.back() == ' ' || line.back() == '\t') return;
                line += ' ';
            }
        };
    } // namespace

    std::string format_c(const std::string &source, const CFormatOptions &opt) {
        // 空行判断直接查原文，因此保留一份源串引用（不拷贝）
        const std::string &lx_src = source;
        CLexer lx(source);
        lx.keep_comments(true);
        lx.merge_preproc(true);   // 预处理整行原样保留
        CLexResult r = lx.tokenize();
        if (r.tokens.empty()) return source;

        Emitter em(opt);
        CTokKind prev = CTokKind::Unknown;
        bool prev_was_switch = false;   // 用于给 case 标签减一级缩进
        int paren_depth = 0;
        int ternary_depth = 0;   // 未闭合的 '?' 个数，用于把 a ? b : c 保持在一行
        // 当前处在初始化列表内的层数（`= { ... }` / `{ 1, 2 }`）。
        // 初始化列表必须压平输出，不能按语句块的规则换行与缩进。
        int init_depth = 0;
        // 当前行是否处于 case/default 标签（从 'case' 开始到其 ':' 结束）
        bool case_label_line = false;
        // prev 是否被判定为单目前缀运算符（跨轮状态：`a * b` 与 `*p` 靠它区分）
        bool prev_is_unary_ = false;
        // prev 是否是声明里的指针星号（`int *p`）：它后面紧跟名字，不留空格
        bool prev_is_decl_star = false;

        for (size_t i = 0; i < r.tokens.size(); ++i) {
            const CToken &t = r.tokens[i];
            if (t.kind == CTokKind::EndOfFile) break;

            // 预处理行：整行原样输出
            if (t.kind == CTokKind::PreprocLine) {
                em.flush_line();
                // 预处理行自身不携带「后随空行」信息，只能看下一个 token；
                // 但下一个 token 若是预处理行，blank 值取决于两者之间是否真有空行。
                em.pending_blank = has_blank_after(lx_src, r.tokens, i) ? 1 : 0;
                em.line = t.text;
                em.flush_line();
                prev = CTokKind::PreprocLine;
                // 预处理行会打断表达式的连续性：`#define` 之后的 '*' 不应被当成前一个语句的延续
                prev_is_unary_ = false;
                prev_is_decl_star = false;
                continue;
            }

            // 换行处理
            if (t.leading_newline && opt.keep_line_breaks) {
                // 三元表达式内部（? 之后、对应的 : 前后）不折行：`fputs(s ? s : "", o)`
                // 被折开既难读又无收益。注意 ternary_depth 在读到 ':' 时已递减，
                // 所以「紧跟 ':' 的那个 token」要靠 prev == Colon 识别（这里已包含）。
                const bool in_ternary = ternary_depth > 0 || prev == CTokKind::Question ||
                                        prev == CTokKind::Colon;
                if (in_ternary) {
                    // 既不断行也不补空行，继续接在当前行后面
                } else {
                    // '}' 换行前先降级（由 '}' 自身处理），这里只 flush。
                    // 关键点：普通换行**不**置 pending_blank。只有源码中确实存在空行
                    // （token 前累计了 >=2 个换行）才补一个空行，否则每个换行都会被渲染成空行。
                    const bool blank = has_blank_before(lx_src, r.tokens, i);
                    if (em.line.find_first_not_of(" \t") != std::string::npos) {
                        em.flush_line();
                        if (blank) em.pending_blank = 1;
                    } else {
                        if (blank) em.pending_blank = 1;
                        em.line.clear();
                        em.at_line_start = true;
                    }
                }
            }

            // 注释：行注释独占一行；块注释原样
            if (t.kind == CTokKind::Comment) {
                if (t.text.rfind("//", 0) == 0) {
                    if (!em.line.empty() && em.line.find_first_not_of(" \t") != std::string::npos)
                        em.flush_line();
                    em.line = em.ind(em.indent) + t.text;
                    em.at_line_start = false;
                    em.flush_line();
                } else {
                    if (!em.line.empty() && em.line.find_first_not_of(" \t") != std::string::npos) {
                        em.append_space();
                        em.append(t.text);
                    } else {
                        em.line = em.ind(em.indent) + t.text;
                        em.at_line_start = false;
                        em.flush_line();
                    }
                }
                prev = t.kind;
                continue;
            }

            // '{'
            if (t.kind == CTokKind::LBrace) {
                const bool is_initializer = (prev == CTokKind::Assign || prev == CTokKind::Comma ||
                                             prev == CTokKind::LParen || prev == CTokKind::LBracket);
                if (is_initializer) {
                    // 初始化列表：压平，括号内按逗号隔开，收尾由 '}' 处理
                    if (!em.line.empty() && em.line.back() != ' ' && prev != CTokKind::LParen &&
                        prev != CTokKind::LBracket) {
                        em.append_space();
                    }
                    em.append("{");
                    ++init_depth;
                    prev = t.kind;
                    continue;
                }
                if (!em.line.empty() && em.line.find_first_not_of(" \t") != std::string::npos) {
                    em.append_space();
                    em.append("{");
                    em.flush_line();
                } else {
                    em.line = em.ind(em.indent) + "{";
                    em.at_line_start = false;
                    em.flush_line();
                }
                em.brace_is_switch.push_back(prev_was_switch);
                prev_was_switch = false;
                em.indent++;
                prev = t.kind;
                continue;
            }

            // '}'
            if (t.kind == CTokKind::RBrace) {
                // 初始化列表收尾：与 '{' 对称，压平处理，不单独成行
                if (init_depth > 0) {
                    --init_depth;
                    // `{ 1, 2 }` —— 收尾大括号前留一个空格
                    if (!em.line.empty() && em.line.back() != ' ' && em.line.back() != '{') {
                        em.append_space();
                    }
                    em.append("}");
                    prev = t.kind;
                    continue;
                }
                if (!em.brace_is_switch.empty()) {
                    em.brace_is_switch.pop_back();
                    if (em.indent > 0) em.indent--;
                    em.flush_line();
                    em.line = em.ind(em.indent) + "}";
                    em.at_line_start = false;
                } else {
                    // 非 switch 的收尾 '}'
                    if (em.line.empty() || em.line.find_first_not_of(" \t") == std::string::npos) {
                        // 该 '}' 独占一行（最常见）：直接按当前缩进输出
                        if (em.indent > 0) em.indent--;
                        em.line = em.ind(em.indent) + "}";
                        em.at_line_start = false;
                    } else {
                        // 与前面语句同行（如 `} else {` 的收尾或初始化列表）
                        em.append("}");
                    }
                    em.flush_line();
                }
                prev = t.kind;
                continue;
            }

            // case / default 标签：整行输出到冒号为止
            if (t.kind == CTokKind::KwCase || t.kind == CTokKind::KwDefault) {
                em.flush_line();
                int lvl = em.indent - 1 + opt.case_indent_delta;
                if (lvl < 0) lvl = 0;
                em.line = em.ind(lvl);
                em.at_line_start = false;
                em.append(t.text);
                // 标签之后的内容（值 + ':'）交给普通分支处理；
                // 这里只标记「当前行是 case 标签」，供 ':' 紧贴与换行判定使用
                case_label_line = true;
                prev = t.kind;
                continue;
            }

            // 普通 token 的空格策略
            if (!em.line.empty() && em.line.find_first_not_of(" \t") != std::string::npos) {

                // ---- 一元前缀运算符的判定（统一在这里算，避免各处推测不一致）----
                // 说明：`- + * & ! ~` 既可能是双目也可能是单目，取决于它左边的 token。
                // 若左边是「表达式起始」（return/(/,/=/运算符/语句开头…），它就是单目。
                const bool prev_starts_expr = starts_expression(prev);
                // 声明里的指针星号单独优先判定（`int *p`），不能落入单目运算符分支
                const bool is_decl_star = (t.kind == CTokKind::Star && is_type_keyword(prev));
                // `* & ! ~` 之后必跟操作数，所以它们一旦出现在表达式起始处就是单目。
                // `+ -` 同理，但 `*` 在 `int *p` 里是声明星号，已在上面排除。
                const bool t_can_be_unary =
                    !is_decl_star &&
                    (t.kind == CTokKind::Star || t.kind == CTokKind::Amp ||
                     t.kind == CTokKind::Plus || t.kind == CTokKind::Minus ||
                     t.kind == CTokKind::Bang || t.kind == CTokKind::Tilde);
                // prev_is_unary_ 是上一轮算出的「prev 是单目运算符」，直接用它，
                // 而不是在本轮根据 prev 的 kind 猜测——后者无法区分 `a * b` 与 `*p`。
                bool need_space = true;

                if (no_space_before(t.kind) || no_space_after(prev)) {
                    need_space = false;
                } else if (t.kind == CTokKind::LParen) {
                    // '(' 前：控制关键字留空格（if (x)）；函数名/右括号紧贴（f(x)、(a)(b)）；
                    // 类型关键字后跟 `(*` 时留空格（`int (*fp)(int)`）
                    const bool next_is_star = (i + 1 < r.tokens.size() &&
                                               r.tokens[i + 1].kind == CTokKind::Star);
                    // 三元 `?` 之后、以及赋值/比较/算术运算符之后的 '(' 都要留空格：
                    // `a ? (int64_t) b : c`、`x = (T) y`
                    const bool after_operator = (prev == CTokKind::Question || prev == CTokKind::Colon ||
                                                 prev == CTokKind::Comma ||
                                                 prev == CTokKind::Assign || prev == CTokKind::EqualEqual ||
                                                 prev == CTokKind::NotEqual || prev == CTokKind::Less ||
                                                 prev == CTokKind::Greater || prev == CTokKind::LessEqual ||
                                                 prev == CTokKind::GreaterEqual || prev == CTokKind::Plus ||
                                                 prev == CTokKind::Minus || prev == CTokKind::Star ||
                                                 prev == CTokKind::Slash || prev == CTokKind::Percent ||
                                                 prev == CTokKind::AmpAmp || prev == CTokKind::PipePipe);
                    need_space = is_control_keyword(prev) || after_operator ||
                                 (is_type_keyword(prev) && next_is_star);
                } else if (prev == CTokKind::LParen) {
                    // '(' 之后紧贴（`(*fp)`、`f(a)`）
                    need_space = false;
                } else if (prev == CTokKind::Comma) {
                    // 逗号后统一留一个空格（`f(a, b)`、`printf("%d", (long long) v)`）
                    need_space = true;
                } else if (t.kind == CTokKind::Colon && case_label_line) {
                    // `case 1:` 的冒号紧贴标签值
                    need_space = false;
                } else if (prev_is_unary_ || prev_is_decl_star) {
                    // 单目运算符或声明星号之后的操作数紧贴：`*p`、`&x`、`!ok`、`-1`
                    need_space = false;
                } else if (t_can_be_unary && prev_starts_expr) {
                    // 单目运算符本身：与左侧留空格（`return -1` 的 '-' 前有空格）
                    need_space = true;
                } else if (is_decl_star) {
                    // 声明里的指针星号 `int *p`：星号前留空格
                    need_space = true;
                } else if (ends_with_label(em.line) && prev != CTokKind::Colon) {
                    // 标签后紧接着的语句不补空格（保护性判断；三元 ':' 例外）
                    need_space = false;
                }
                if (need_space) em.append_space();

                // 记录本轮结论，供下一轮使用
                prev_is_unary_ = t_can_be_unary && prev_starts_expr;
                prev_is_decl_star = is_decl_star;
            } else {
                // 行首：若行首就是 `* & ! ~ + -`，它必然是单目运算符（`*p = 3;`）
                prev_is_unary_ = (t.kind == CTokKind::Star || t.kind == CTokKind::Amp ||
                                  t.kind == CTokKind::Bang || t.kind == CTokKind::Tilde ||
                                  t.kind == CTokKind::Plus || t.kind == CTokKind::Minus);
                prev_is_decl_star = false;
            }
            em.append(t.text);

            if (t.kind == CTokKind::LParen) ++paren_depth;
            if (t.kind == CTokKind::RParen && paren_depth > 0) --paren_depth;
            if (t.kind == CTokKind::Question) ++ternary_depth;
            // 记录当前 ':' 是否属于三元：属于则 ternary_depth 递减，
            // 后面「标签换行」判定必须知道这件事，否则 ends_with_label 会把
            // `fputs(s ? s :` 的行尾 ':' 误认成 goto 标签而折行。
            const bool colon_is_ternary = (t.kind == CTokKind::Colon && ternary_depth > 0);
            if (colon_is_ternary) --ternary_depth;

            prev_was_switch = (t.kind == CTokKind::KwSwitch);
            prev = t.kind;

            // ';' 之后换行（除非在 for 的括号里）
            if (t.kind == CTokKind::Semicolon && paren_depth == 0) {
                em.flush_line();
                // 注意：不在这里置 pending_blank。空行是否存在由源码决定
                // （下一个 token 的 blank_lines_before > 1），否则每个 ';' 后都会空一行。
                em.pending_blank = has_blank_after(lx_src, r.tokens, i) ? 1 : 0;
            }
            // 标签 ':' 之后换行（case 已处理；这里处理 goto label）。
            // 三元运算符的 ':' 属于表达式，绝不能当标签折行（否则 a ? b : c 会被折成两行）
            if (t.kind == CTokKind::Colon && case_label_line) {
                // `case 1:` 之后另起一行写 case 体
                em.flush_line();
                em.pending_blank = 0;
                case_label_line = false;
            }
            if (t.kind == CTokKind::Colon && !colon_is_ternary && !case_label_line &&
                ends_with_label(em.line) &&
                em.line.find("case ") == std::string::npos &&
                em.line.find("default") == std::string::npos) {
                em.flush_line();
                em.pending_blank = has_blank_after(lx_src, r.tokens, i) ? 1 : 0;
            }
        }
        em.flush_line();
        return em.out;
    }

    std::string reindent_c(const std::string &source, const int base_indent, const CFormatOptions &opt) {
        CLexer lx(source);
        lx.keep_comments(true);
        lx.merge_preproc(true);
        CLexResult r = lx.tokenize();
        if (r.tokens.empty()) return source;

        Emitter em(opt);
        em.indent = base_indent;
        CTokKind prev = CTokKind::Unknown;

        for (size_t i = 0; i < r.tokens.size(); ++i) {
            const CToken &t = r.tokens[i];
            if (t.kind == CTokKind::EndOfFile) break;
            if (t.leading_newline) {
                em.flush_line();
            }
            if (t.kind == CTokKind::RBrace) {
                if (em.indent > base_indent) em.indent--;
                em.flush_line();
            }
            if (t.kind == CTokKind::LBrace) {
                if (!em.line.empty() && em.line.find_first_not_of(" \t") != std::string::npos)
                    em.append_space();
                em.append(t.text);
                em.indent++;
                prev = t.kind;
                continue;
            }
            // 只在需要时补一个空格，其余保持 token 原样（保守）
            if (!em.line.empty() && em.line.find_first_not_of(" \t") != std::string::npos &&
                !no_space_before(t.kind) && !no_space_after(prev) &&
                // '(' 前：控制关键字留空格，函数名/右括号紧贴（与 format_c 一致）
                !(t.kind == CTokKind::LParen && !is_control_keyword(prev))) {
                em.append_space();
            }
            em.append(t.text);
            prev = t.kind;
        }
        em.flush_line();
        return em.out;
    }
} // namespace sinter::cfront
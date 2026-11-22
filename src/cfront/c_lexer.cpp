// cfront: c_lexer.cpp —— C 词法分析实现
//
// 覆盖范围（够用且有边界）：
//   - 全部 C11 标点与运算符（最长匹配优先）
//   - 标识符 / 关键字（关键字表来自 c_token.cpp，单一来源）
//   - 十进制/八进制/十六进制/二进制整数，含 u/l/ll 后缀
//   - 浮点：小数点、指数、f/l 后缀、十六进制浮点
//   - 字符与字符串字面量（含转义、前缀 L/u/U/u8）
//   - 行注释与块注释（可选保留）
//   - 预处理指令：整行合并为 PreprocLine，并解析出指令名
// 明确不做：宏展开、三元组（trigraph）、字符串拼接。（文档会写明）
#include "cfront/c_lexer.hpp"

#include <cctype>

namespace sinter::cfront {
    namespace {
        bool is_id_start(const char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        }

        bool is_id_char(const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        }
    } // namespace

    const std::unordered_map<std::string, CTokKind> &CLexer::kw_map() {
        static const std::unordered_map<std::string, CTokKind> m = [] {
            std::unordered_map<std::string, CTokKind> r;
            size_t n = 0;
            const CKeyword *kw = c_keywords(n);
            for (size_t i = 0; i < n; ++i) r.emplace(kw[i].name, kw[i].kind);
            return r;
        }();
        return m;
    }

    CLexer::CLexer(std::string src) : src_(std::move(src)) {}

    char CLexer::peek(const size_t k) const {
        const size_t i = pos_ + k;
        return i < src_.size() ? src_[i] : '\0';
    }

    char CLexer::advance() {
        if (pos_ >= src_.size()) return '\0';
        const char c = src_[pos_++];
        if (c == '\n') {
            line_++;
            col_ = 1;
        } else {
            col_++;
        }
        return c;
    }

    void CLexer::push(const CTokKind k, std::string text, const size_t start_off,
                      const uint32_t start_line, const uint32_t start_col, const bool nl,
                      const uint8_t spaces) {
        CToken t;
        t.kind = k;
        t.text = std::move(text);
        t.offset = start_off;
        t.line = start_line;
        t.column = start_col;
        t.leading_newline = nl;
        t.leading_spaces = spaces;
        // pending_blank_lines_ 统计的是「上一个 token 之后出现过几个换行」，
        // 1 表示普通换行，2 及以上表示中间有空行。格式化器靠它决定保留不保留空行。
        // 读完必须清零，否则下一 token 会继承累积值，整份文件都被判成空行。
        const size_t blanks = pending_blank_lines_ > 255 ? 255 : pending_blank_lines_;
        t.blank_lines_before = static_cast<uint8_t>(blanks);
        pending_blank_lines_ = 0;
        out_.push_back(std::move(t));
    }

    // ---------------------------------------------------------------- 预处理
    namespace {
        /// 文本里是否留着一个没闭合的 `/*`（只看最后一次开/闭的先后）
        bool has_open_block_comment(const std::string &s) {
            size_t last_open = std::string::npos;
            size_t last_close = std::string::npos;
            for (size_t i = 0; i + 1 < s.size(); ++i) {
                if (s[i] == '/' && s[i + 1] == '*') {
                    last_open = i;
                    ++i;
                } else if (s[i] == '*' && s[i + 1] == '/') {
                    last_close = i;
                    ++i;
                }
            }
            if (last_open == std::string::npos) return false;
            return last_close == std::string::npos || last_open > last_close;
        }
    } // namespace

    void CLexer::lex_preproc() {
        const size_t start_off = pos_;
        const uint32_t start_line = line_;
        const uint32_t start_col = col_;
        const bool nl = pending_newline_;
        const uint8_t spaces = pending_spaces_;

        // 收集整条逻辑行，处理行尾 '\' 续行
        std::string text;
        uint32_t newlines = 0;   // 本指令消耗的换行数（含续行），供空行统计
        while (pos_ < src_.size()) {
            const char c = advance();
            if (c == '\n') {
                ++newlines;
                // 检查是否续行：去掉行尾空格后是 '\'
                size_t p = text.size();
                while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t')) --p;
                if (p > 0 && text[p - 1] == '\\') {
                    text += c;   // 保留续行，继续吃下一行
                    continue;
                }
                break;
            }
            text += c;
        }
        // 指令里的块注释可以跨行：注释不产生 token，物理换行不结束指令。
        if (has_open_block_comment(text)) {
            bool closed = false;
            while (pos_ < src_.size() && !closed) {
                const char c = advance();
                text += c;
                const size_t n = text.size();
                if (n >= 2 && text[n - 2] == '*' && text[n - 1] == '/') closed = true;
                if (c == '\n') ++newlines;
            }
        }
        // 去掉尾随换行
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

        std::string directive;
        // 形如 "#  include" / "#include"
        size_t i = 0;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i < text.size() && text[i] == '#') {
            ++i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            while (i < text.size() && is_id_char(text[i])) directive += text[i++];
        }

        CToken t;
        t.kind = CTokKind::PreprocLine;
        t.text = text;
        t.offset = start_off;
        t.line = start_line;
        t.column = start_col;
        t.leading_newline = nl;
        t.leading_spaces = spaces;
        t.directive = directive;
        // 与 push() 一致：写入本 token 前累计的换行数，然后把本指令消耗的换行数
        // 作为「下一条 token 之前的换行数」累积进去。否则 '#include' 后面的
        // 空行会在统计里彻底消失（预处理行是唯一不走 push 的 token）。
        const size_t blanks = pending_blank_lines_ > 255 ? 255 : pending_blank_lines_;
        t.blank_lines_before = static_cast<uint8_t>(blanks);
        pending_blank_lines_ = newlines;
        out_.push_back(std::move(t));
    }

    // ---------------------------------------------------------------- 数字
    void CLexer::lex_number() {
        const size_t start_off = pos_;
        const uint32_t start_line = line_;
        const uint32_t start_col = col_;
        const bool nl = pending_newline_;
        const uint8_t spaces = pending_spaces_;
        std::string text;
        bool is_float = false;

        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            text += advance();   // 0
            text += advance();   // x
            while (std::isxdigit(static_cast<unsigned char>(peek()))) text += advance();
            // 十六进制浮点 0x1p3
            if (peek() == '.' || peek() == 'p' || peek() == 'P') {
                is_float = true;
                if (peek() == '.') {
                    text += advance();
                    while (std::isxdigit(static_cast<unsigned char>(peek()))) text += advance();
                }
                if (peek() == 'p' || peek() == 'P') {
                    text += advance();
                    if (peek() == '+' || peek() == '-') text += advance();
                    while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
                }
            }
        } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            text += advance();
            text += advance();
            while (peek() == '0' || peek() == '1') text += advance();
        } else {
            while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                is_float = true;
                text += advance();
                while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                const char n1 = peek(1);
                const bool exp = std::isdigit(static_cast<unsigned char>(n1)) || n1 == '+' || n1 == '-';
                if (exp) {
                    is_float = true;
                    text += advance();
                    if (peek() == '+' || peek() == '-') text += advance();
                    while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
                }
            }
        }

        // 后缀：u U l L f F 及其组合
        if (is_float) {
            if (peek() == 'f' || peek() == 'F' || peek() == 'l' || peek() == 'L') text += advance();
        } else {
            while (true) {
                const char c = peek();
                if (c == 'u' || c == 'U' || c == 'l' || c == 'L') text += advance();
                else break;
            }
        }
        push(is_float ? CTokKind::FloatLiteral : CTokKind::IntLiteral, std::move(text),
             start_off, start_line, start_col, nl, spaces);
    }

    // ------------------------------------------------------- 字符 / 字符串
    void CLexer::lex_char_or_string(const char quote) {
        const size_t start_off = pos_;
        const uint32_t start_line = line_;
        const uint32_t start_col = col_;
        const bool nl = pending_newline_;
        const uint8_t spaces = pending_spaces_;
        std::string text;
        text += advance();   // 开引号
        while (pos_ < src_.size()) {
            const char c = advance();
            text += c;
            if (c == '\\') {
                // 转义：原样吃掉下一个字符（八进制/十六进制转义长度不一，
                // 但词法上只需保证不误判闭引号）
                if (pos_ < src_.size()) text += advance();
                continue;
            }
            if (c == quote) break;
            if (c == '\n') {
                errors_.push_back({"unterminated literal", start_line, start_col, start_off});
                break;
            }
        }
        push(quote == '\'' ? CTokKind::CharLiteral : CTokKind::StringLiteral, std::move(text),
             start_off, start_line, start_col, nl, spaces);
    }

    // ---------------------------------------------------------------- 标识符
    void CLexer::lex_identifier() {
        const size_t start_off = pos_;
        const uint32_t start_line = line_;
        const uint32_t start_col = col_;
        const bool nl = pending_newline_;
        const uint8_t spaces = pending_spaces_;
        std::string text;
        while (is_id_char(peek())) text += advance();
        // `__attribute__((...))` / `__declspec(...)` / `__asm__(...)`：整段吸收成一个 token。
        // 它的参数是任意 token 序列（还可能嵌套括号与字符串），若拆成普通 token，
        // 解析器就得在「说明符 / 声明符 / 形参」三处各写一遍跳过逻辑，且极易漏。
        if (text == "__attribute__" || text == "__attribute" || text == "__declspec" ||
            text == "__asm__" || text == "__asm") {
            size_t k = 0;
            while (peek(k) == ' ' || peek(k) == '\t') ++k;
            if (peek(k) == '(') {
                int depth = 0;
                while (pos_ < src_.size()) {
                    const char c = advance();
                    text += c;
                    if (c == '(') { ++depth; continue; }
                    if (c == ')') { if (--depth == 0) break; continue; }
                    if (c == '"' || c == '\'') {
                        const char q = c;
                        while (pos_ < src_.size()) {
                            const char d = advance();
                            text += d;
                            if (d == '\\' && pos_ < src_.size()) { text += advance(); continue; }
                            if (d == q) break;
                        }
                    }
                }
                push(CTokKind::Attribute, std::move(text), start_off, start_line, start_col, nl, spaces);
                return;
            }
        }
        const auto &m = kw_map();
        if (const auto it = m.find(text); it != m.end())
            push(it->second, std::move(text), start_off, start_line, start_col, nl, spaces);
        else
            push(CTokKind::Identifier, std::move(text), start_off, start_line, start_col, nl, spaces);
    }

    // ---------------------------------------------------------------- 标点
    bool CLexer::try_punct() {
        struct P {
            const char *s;
            CTokKind k;
            uint8_t len;
        };
        // 最长匹配优先，顺序即优先级
        static const P table[] = {
            {"<<=", CTokKind::ShlAssign, 3}, {">>=", CTokKind::ShrAssign, 3},
            {"...", CTokKind::Ellipsis, 3},
            {"->", CTokKind::Arrow, 2}, {"++", CTokKind::PlusPlus, 2},
            {"--", CTokKind::MinusMinus, 2}, {"<<", CTokKind::Shl, 2},
            {">>", CTokKind::Shr, 2}, {"<=", CTokKind::LessEqual, 2},
            {">=", CTokKind::GreaterEqual, 2}, {"==", CTokKind::EqualEqual, 2},
            {"!=", CTokKind::NotEqual, 2}, {"&&", CTokKind::AmpAmp, 2},
            {"||", CTokKind::PipePipe, 2}, {"+=", CTokKind::PlusAssign, 2},
            {"-=", CTokKind::MinusAssign, 2}, {"*=", CTokKind::StarAssign, 2},
            {"/=", CTokKind::SlashAssign, 2}, {"%=", CTokKind::PercentAssign, 2},
            {"&=", CTokKind::AmpAssign, 2}, {"|=", CTokKind::PipeAssign, 2},
            {"^=", CTokKind::CaretAssign, 2}, {"##", CTokKind::HashHash, 2},
        };
        for (const auto &p: table) {
            bool ok = true;
            for (uint8_t i = 0; i < p.len; ++i)
                if (peek(i) != p.s[i]) { ok = false; break; }
            if (!ok) continue;
            const size_t start_off = pos_;
            const uint32_t start_line = line_;
            const uint32_t start_col = col_;
            const bool nl = pending_newline_;
            const uint8_t spaces = pending_spaces_;
            std::string text;
            for (uint8_t i = 0; i < p.len; ++i) text += advance();
            push(p.k, std::move(text), start_off, start_line, start_col, nl, spaces);
            return true;
        }
        // 单字符
        static const struct {
            char c;
            CTokKind k;
        } single[] = {
            {'(', CTokKind::LParen}, {')', CTokKind::RParen},
            {'{', CTokKind::LBrace}, {'}', CTokKind::RBrace},
            {'[', CTokKind::LBracket}, {']', CTokKind::RBracket},
            {';', CTokKind::Semicolon}, {',', CTokKind::Comma},
            {'.', CTokKind::Dot}, {'+', CTokKind::Plus}, {'-', CTokKind::Minus},
            {'*', CTokKind::Star}, {'/', CTokKind::Slash}, {'%', CTokKind::Percent},
            {'&', CTokKind::Amp}, {'|', CTokKind::Pipe}, {'^', CTokKind::Caret},
            {'~', CTokKind::Tilde}, {'!', CTokKind::Bang}, {'=', CTokKind::Assign},
            {'<', CTokKind::Less}, {'>', CTokKind::Greater},
            {'?', CTokKind::Question}, {':', CTokKind::Colon},
            {'#', CTokKind::Hash},
        };
        const char c = peek();
        for (const auto &s: single) {
            if (s.c != c) continue;
            const size_t start_off = pos_;
            const uint32_t start_line = line_;
            const uint32_t start_col = col_;
            const bool nl = pending_newline_;
            const uint8_t spaces = pending_spaces_;
            std::string text;
            text += advance();
            push(s.k, std::move(text), start_off, start_line, start_col, nl, spaces);
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------- 主循环
    CLexResult CLexer::tokenize() {
        while (pos_ < src_.size()) {
            const char c = peek();

            // 空白
            if (c == ' ' || c == '\t' || c == '\r') {
                if (pending_newline_) {
                    // 已在换行之后，只累加缩进
                    if (pending_spaces_ < 255) pending_spaces_++;
                } else {
                    if (pending_spaces_ < 255) pending_spaces_++;
                }
                advance();
                continue;
            }
            if (c == '\n') {
                pending_newline_ = true;
                pending_spaces_ = 0;
                pending_blank_lines_++;
                advance();
                line_start_ = true;
                continue;
            }

            // 注释
            if (c == '/' && peek(1) == '/') {
                const size_t start_off = pos_;
                const uint32_t start_line = line_;
                const uint32_t start_col = col_;
                const bool nl = pending_newline_;
                const uint8_t spaces = pending_spaces_;
                std::string text;
                while (pos_ < src_.size() && peek() != '\n') text += advance();
                if (keep_comments_)
                    push(CTokKind::Comment, std::move(text), start_off, start_line, start_col,
                         nl, spaces);
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                const size_t start_off = pos_;
                const uint32_t start_line = line_;
                const uint32_t start_col = col_;
                const bool nl = pending_newline_;
                const uint8_t spaces = pending_spaces_;
                std::string text;
                text += advance();   // /
                text += advance();   // *
                bool closed = false;
                while (pos_ < src_.size()) {
                    if (peek() == '*' && peek(1) == '/') {
                        text += advance();
                        text += advance();
                        closed = true;
                        break;
                    }
                    text += advance();
                }
                if (!closed)
                    errors_.push_back({"unterminated block comment", start_line, start_col, start_off});
                if (keep_comments_)
                    push(CTokKind::Comment, std::move(text), start_off, start_line, start_col,
                         nl, spaces);
                continue;
            }

            // 预处理指令：行首 '#'（允许前面只有空白）
            if (c == '#' && line_start_ && merge_preproc_) {
                lex_preproc();
                // 预处理行吃掉了行尾换行，因此下一条 token 仍处于行首——
                // 这正是连续多条 #include/#define 能逐行识别的关键。
                line_start_ = true;
                pending_newline_ = false;
                pending_spaces_ = 0;
                continue;
            }

            line_start_ = false;

            if (std::isdigit(static_cast<unsigned char>(c))) {
                lex_number();
            } else if (is_id_start(c)) {
                // 字符串前缀 L / u / U / u8
                const char n1 = peek(1);
                if ((c == 'L' || c == 'u' || c == 'U') && (n1 == '"' || n1 == '\'')) {
                    const size_t start_off = pos_;
                    const uint32_t start_line = line_;
                    const uint32_t start_col = col_;
                    const bool nl = pending_newline_;
                    const uint8_t spaces = pending_spaces_;
                    std::string prefix;
                    prefix += advance();
                    lex_char_or_string(n1);
                    // 把前缀并入刚输出的 token
                    if (!out_.empty()) {
                        out_.back().text = prefix + out_.back().text;
                        out_.back().offset = start_off;
                        out_.back().line = start_line;
                        out_.back().column = start_col;
                        out_.back().leading_newline = nl;
                        out_.back().leading_spaces = spaces;
                    }
                    continue;
                }
                if (c == 'u' && n1 == '8' && (peek(2) == '"' || peek(2) == '\'')) {
                    const size_t start_off = pos_;
                    const uint32_t start_line = line_;
                    const uint32_t start_col = col_;
                    const bool nl = pending_newline_;
                    const uint8_t spaces = pending_spaces_;
                    std::string prefix;
                    prefix += advance();
                    prefix += advance();
                    lex_char_or_string(peek());
                    if (!out_.empty()) {
                        out_.back().text = prefix + out_.back().text;
                        out_.back().offset = start_off;
                        out_.back().line = start_line;
                        out_.back().column = start_col;
                        out_.back().leading_newline = nl;
                        out_.back().leading_spaces = spaces;
                    }
                    continue;
                }
                lex_identifier();
            } else if (c == '"' || c == '\'') {
                lex_char_or_string(c);
            } else if (!try_punct()) {
                const size_t start_off = pos_;
                const uint32_t start_line = line_;
                const uint32_t start_col = col_;
                const char bad = advance();
                errors_.push_back({std::string("unrecognized character '") + bad + "'", start_line,
                                   start_col, start_off});
                push(CTokKind::Unknown, std::string(1, bad), start_off, start_line, start_col,
                     pending_newline_, pending_spaces_);
            }

            pending_newline_ = false;
            pending_spaces_ = 0;
        }

        CToken eof;
        eof.kind = CTokKind::EndOfFile;
        eof.offset = src_.size();
        eof.line = line_;
        eof.column = col_;
        eof.leading_newline = pending_newline_;
        out_.push_back(std::move(eof));

        CLexResult r;
        r.tokens = std::move(out_);
        r.errors = std::move(errors_);
        return r;
    }
} // namespace sinter::cfront
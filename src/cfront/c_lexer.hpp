// cfront: c_lexer.hpp —— C 词法分析器
#pragma once
#include "cfront/c_token.hpp"

#include <string>
#include <unordered_map>

namespace sinter::cfront {
    /// 词法错误（不抛异常，收集后由调用方决定如何处理，LSP 需要容错）
    struct CLexError {
        std::string message;
        uint32_t line = 1;
        uint32_t column = 1;
        size_t offset = 0;
    };

    struct CLexResult {
        CTokens tokens;
        std::vector<CLexError> errors;
    };

    class CLexer {
    public:
        explicit CLexer(std::string src);

        /// 是否保留注释 token（格式化器需要，语法分析时关掉）
        void keep_comments(bool v) { keep_comments_ = v; }

        /// 是否把整条预处理指令合并为单个 PreprocLine token（默认 true）。
        /// 关掉时 '#' 与后续内容按普通 token 输出，便于格式化器逐词处理。
        void merge_preproc(bool v) { merge_preproc_ = v; }

        CLexResult tokenize();

    private:
        std::string src_;
        size_t pos_ = 0;
        uint32_t line_ = 1;
        uint32_t col_ = 1;
        bool keep_comments_ = false;
        bool merge_preproc_ = true;
        bool line_start_ = true;   // 当前是否处于行首（预处理指令只在行首生效）
        CTokens out_;
        std::vector<CLexError> errors_;

        char peek(size_t k = 0) const;
        char advance();
        void skip_block_comment_body();
        void skip_line_comment_body();

        void push(CTokKind k, std::string text, size_t start_off, uint32_t start_line,
                  uint32_t start_col, bool nl, uint8_t spaces);
        void lex_preproc();
        void lex_number();
        void lex_char_or_string(char quote);
        void lex_identifier();
        bool try_punct();

        // 上一 token 与本 token 之间的空白信息，供格式化器使用
        bool pending_newline_ = false;
        uint8_t pending_spaces_ = 0;
        size_t pending_blank_lines_ = 0;

        static const std::unordered_map<std::string, CTokKind> &kw_map();
    };
} // namespace sinter::cfront
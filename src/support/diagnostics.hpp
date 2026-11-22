// sinter 诊断：统一错误/警告输出与错误计数
#pragma once

#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace sinter {
    enum class Severity { Note, WeakWarning, Warning, Error };

    struct Diagnostic {
        Severity severity = Severity::Error;
        std::string message;
        std::string file;
        int line = 0;      // 1-based; 0 means "no location"
        int col = 0;       // 1-based; 0 means "no column"
        int length = 0;    // span length for the ^^^^^ underline; 0 = infer
        /// Stable machine-readable code, e.g. "E0101". Printed as `error[E0101]:`.
        std::string code;
        /// Optional actionable suggestion, printed as `= hint: ...`.
        std::string hint;
        /// 出错位置所在的「模块 › 类/结构体/接口 › 函数」上下文，
        /// 打印成 `= scope: module std.string › class String › fun trim`。
        /// 空表示没有上下文（例如驱动层错误）。
        std::string scope;
    };

    /// 把 Severity 转成可读名称（供序列化/测试断言使用）
    inline const char *to_string(Severity s) {
        switch (s) {
            case Severity::Error: return "error";
            case Severity::WeakWarning: return "weak warning";
            case Severity::Warning: return "warning";
            default: return "note";
        }
    }

    namespace detail {
        /// 按需读取并缓存源文件的行，供渲染源码片段。
        inline const std::vector<std::string> &cached_source_lines(const std::string &file) {
            static std::unordered_map<std::string, std::vector<std::string>> cache;
            auto it = cache.find(file);
            if (it != cache.end()) return it->second;
            std::vector<std::string> lines;
            if (std::ifstream in(file); in) {
                std::string s;
                while (std::getline(in, s)) lines.push_back(s);
            }
            return cache.emplace(file, std::move(lines)).first->second;
        }
    }

    /// 渲染一条诊断。格式：
    ///   main.sinter:3:17: error[E0301]: type mismatch: expected i64, got str
    ///       |
    ///     3 |     var x: i64 = "str"
    ///       |                  ^^^^^
    ///       = hint: change the declared type to str, or use an i64 literal
    inline void render_diagnostic(std::ostream &os, const Diagnostic &d) {
        if (!d.file.empty()) {
            os << d.file;
            if (d.line > 0) {
                os << ":" << d.line;
                if (d.col > 0) os << ":" << d.col;
            }
            os << ": ";
        }
        os << to_string(d.severity);
        if (!d.code.empty()) os << "[" << d.code << "]";
        os << ": " << d.message << "\n";

        if (!d.scope.empty()) os << "  = scope: " << d.scope << "\n";
        if (d.line <= 0 || d.file.empty()) {
            if (!d.hint.empty()) os << "  = hint: " << d.hint << "\n";
            return;
        }
        const auto &lines = detail::cached_source_lines(d.file);
        if (d.line > static_cast<int>(lines.size())) {
            if (!d.hint.empty()) os << "  = hint: " << d.hint << "\n";
            return;
        }

        const std::string &src = lines[static_cast<size_t>(d.line) - 1];
        const std::string ln = std::to_string(d.line);
        const std::string pad(ln.size(), ' ');

        os << pad << " |\n";
        os << ln << " | " << src << "\n";

        if (d.col <= 0) {
            if (!d.hint.empty()) os << pad << " = hint: " << d.hint << "\n";
            return;
        }

        // 制表符按 4 列显示对齐
        std::string mark;
        int shown = 0;
        for (int i = 0; i < d.col - 1 && i < static_cast<int>(src.size()); ++i)
            shown += (src[static_cast<size_t>(i)] == '\t') ? 4 : 1;
        mark.assign(static_cast<size_t>(shown), ' ');

        int len = d.length;
        if (len <= 0) {
            // 未给长度：取到行内第一个分隔符为止
            len = 1;
            for (size_t i = static_cast<size_t>(d.col - 1); i < src.size(); ++i) {
                const char ch = src[i];
                if (ch == ' ' || ch == '\t' || ch == ')' || ch == ',' || ch == ';') break;
                ++len;
            }
            if (len > 1) --len;
        }
        mark.append(static_cast<size_t>(len), '^');
        os << pad << " | " << mark << "\n";
        if (!d.hint.empty()) os << pad << " = hint: " << d.hint << "\n";
    }

    /// 收集诊断并按统一格式输出（编译期错误计数用于决定是否继续生成代码）。
    class Diagnostics {
    public:
        /// 带错误码 / 修复建议的完整形式。聚合初始化顺序必须和 Diagnostic 一致：
        /// severity, message, file, line, col, length, code, hint。
        void report_ex(Severity sev, std::string code, std::string msg, std::string hint,
                       std::string file = {}, int line = 0, int col = 0, int length = 0) {
            items_.push_back({sev, std::move(msg), std::move(file), line, col, length,
                              std::move(code), std::move(hint)});
            if (sev == Severity::Error) ++errors_;
            else if (sev == Severity::Warning || sev == Severity::WeakWarning) ++warnings_;
        }

        void error_ex(std::string code, std::string msg, std::string hint,
                      std::string file = {}, int line = 0, int col = 0, int length = 0) {
            report_ex(Severity::Error, std::move(code), std::move(msg), std::move(hint),
                      std::move(file), line, col, length);
        }

        void report(Severity sev, std::string msg, std::string file = {}, int line = 0,
                    int col = 0, int length = 0) {
            items_.push_back({sev, std::move(msg), std::move(file), line, col, length});
            if (sev == Severity::Error) ++errors_;
            else if (sev == Severity::Warning || sev == Severity::WeakWarning) ++warnings_;
        }

        void error(std::string msg, std::string file = {}, int line = 0, int col = 0, int length = 0) {
            report(Severity::Error, std::move(msg), std::move(file), line, col, length);
        }

        void warning(std::string msg, std::string file = {}, int line = 0, int col = 0, int length = 0) {
            report(Severity::Warning, std::move(msg), std::move(file), line, col, length);
        }

        [[nodiscard]] int error_count() const { return errors_; }
        [[nodiscard]] int warning_count() const { return warnings_; }
        [[nodiscard]] bool has_errors() const { return errors_ > 0; }
        [[nodiscard]] const std::vector<Diagnostic> &items() const { return items_; }

        /// 输出到 stderr，返回是否存在错误。
        bool flush(std::ostream &os = std::cerr) const {
            for (const auto &d: items_) render_diagnostic(os, d);
            return errors_ > 0;
        }

        void clear() {
            items_.clear();
            errors_ = warnings_ = 0;
        }

    private:
        std::vector<Diagnostic> items_;
        int errors_ = 0;
        int warnings_ = 0;
    };
}
// sinter-lsp: document.cpp —— position and  URI 
#include "document.hpp"

#include <algorithm>

namespace sinterlsp {
    TextBuffer::TextBuffer(std::string text) : text_(std::move(text)) {
        line_starts_.push_back(0);
        for (size_t i = 0; i < text_.size(); ++i) {
            if (text_[i] == '\n') line_starts_.push_back(i + 1);
        }
    }

    size_t TextBuffer::line_start(const int line) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_starts_.size()) return text_.size();
        return line_starts_[static_cast<size_t>(line)];
    }

    /// a  UTF-8 column （）
    static size_t utf8_len(const unsigned char c) {
        if ((c & 0x80) == 0) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;   // invalid 1 ，
    }

    ///  requires  UTF-16 （BMP  ）
    static bool needs_surrogate_pair(const unsigned cp) { return cp > 0xFFFF; }

    size_t TextBuffer::offset_at(const Position p) const {
        const size_t beg = line_start(p.line);
        if (p.character <= 0) return beg;
        const size_t end = (static_cast<size_t>(p.line + 1) < line_starts_.size())
                               ? line_starts_[static_cast<size_t>(p.line + 1)]
                               : text_.size();
        size_t i = beg;
        int units = 0;
        while (i < end && units < p.character) {
            const unsigned char c = static_cast<unsigned char>(text_[i]);
            const size_t n = utf8_len(c);
            // ， requires 2   UTF-16 。
            // must 6 ：
            //   4  (F0 9F 98 80) → 0x1F600，> 0xFFFF ⇒  2   UTF-16 。
            // （ |= byte & 0x3F），will  0x1F，emoji   1  。
            unsigned cp = 0;
            if (n == 1) cp = c;
            else if (n == 2) cp = (c & 0x1Fu) << 6;
            else if (n == 3) cp = (c & 0x0Fu) << 12;
            else cp = (c & 0x07u) << 18;
            for (size_t k = 1; k < n && i + k < end; ++k)
                cp |= (static_cast<unsigned char>(text_[i + k]) & 0x3Fu) << (6 * (n - 1 - k));
            units += needs_surrogate_pair(cp) ? 2 : 1;
            i += n;
        }
        return std::min(i, end);
    }

    Position TextBuffer::position_at(const size_t offset) const {
        const size_t off = std::min(offset, text_.size());
        // to  offset  line
        const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), off);
        const int line = static_cast<int>(it - line_starts_.begin()) - 1;
        const size_t beg = line_start(line);
        int units = 0;
        size_t i = beg;
        while (i < off) {
            const unsigned char c = static_cast<unsigned char>(text_[i]);
            const size_t n = utf8_len(c);
            unsigned cp = 0;
            if (n == 1) cp = c;
            else if (n == 2) cp = (c & 0x1Fu) << 6;
            else if (n == 3) cp = (c & 0x0Fu) << 12;
            else cp = (c & 0x07u) << 18;
            // note： 「columnlength」 ，cannot off ——
            // off   ，off will  ， cp 
            // （emoji ， 1  ）。
            //  6 ，then  4  will 。
            for (size_t k = 1; k < n && i + k < text_.size(); ++k)
                cp |= (static_cast<unsigned char>(text_[i + k]) & 0x3Fu) << (6 * (n - 1 - k));
            units += needs_surrogate_pair(cp) ? 2 : 1;
            i += n;
        }
        return Position{line < 0 ? 0 : line, units};
    }

    std::string TextBuffer::line_text(const int line) const {
        if (line < 0 || static_cast<size_t>(line) >= line_starts_.size()) return {};
        const size_t beg = line_starts_[static_cast<size_t>(line)];
        size_t end = (static_cast<size_t>(line + 1) < line_starts_.size())
                         ? line_starts_[static_cast<size_t>(line + 1)]
                         : text_.size();
        // line  \n  and   \r
        while (end > beg && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) --end;
        return text_.substr(beg, end - beg);
    }

    std::string TextBuffer::slice(const size_t beg, const size_t end) const {
        if (beg >= end || beg >= text_.size()) return {};
        return text_.substr(beg, std::min(end, text_.size()) - beg);
    }

    std::string Document::file_name() const {
        const std::string p = uri_to_path(uri);
        const size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? p : p.substr(slash + 1);
    }

    std::string uri_to_path(const std::string &uri) {
        constexpr const char *kPrefix = "file://";
        if (uri.rfind(kPrefix, 0) == 0) return uri.substr(7);
        return uri;
    }

    std::string path_to_uri(const std::string &path) {
        return "file://" + path;
    }
} // namespace sinterlsp
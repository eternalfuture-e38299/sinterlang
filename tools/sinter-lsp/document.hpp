// sinter-lsp: document.hpp —— open document + position
//
// LSP   Position   (line, character)，linefrom  0 start、character  UTF-16 。
// 「offset」， ， to code。
#pragma once
#include <string>
#include <vector>

namespace sinterlsp {
    struct Position {
        int line = 0;
        int character = 0;
    };

    struct Range {
        Position start;
        Position end;
    };

    /// UTF-8 text line and position
    class TextBuffer {
    public:
        explicit TextBuffer(std::string text);

        [[nodiscard]] const std::string &text() const { return text_; }

        /// line（0 ）  offset； return  text().size()
        [[nodiscard]] size_t line_start(int line) const;

        /// Position -> offset（character  UTF-16 ，clamp to line）
        [[nodiscard]] size_t offset_at(Position p) const;

        /// offset -> Position
        [[nodiscard]] Position position_at(size_t offset) const;

        /// line
        [[nodiscard]] int line_count() const { return static_cast<int>(line_starts_.size()); }

        /// line content（not line）
        [[nodiscard]] std::string line_text(int line) const;

        ///  [beg,end) text
        [[nodiscard]] std::string slice(size_t beg, size_t end) const;

    private:
        std::string text_;
        std::vector<size_t> line_starts_;
    };

    /// a open document
    struct Document {
        std::string uri;
        std::string language_id;
        int version = 1;
        std::string text;

        [[nodiscard]] TextBuffer buffer() const { return TextBuffer(text); }

        /// file（diagnostic）
        [[nodiscard]] std::string file_name() const;
    };

    /// from  file:// URI path（not  ，）
    std::string uri_to_path(const std::string &uri);
    /// from pathgenerate file:// URI
    std::string path_to_uri(const std::string &path);
} // namespace sinterlsp
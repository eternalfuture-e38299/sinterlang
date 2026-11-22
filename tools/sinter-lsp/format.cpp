// sinter-lsp: format.cpp —— 只负责把格式化结果包成 LSP TextEdit[]
#include "format.hpp"

namespace sinterlsp {
    Json format_to_lsp_edits(const std::string &original, const std::string &formatted,
                             const Document &doc) {
        if (original == formatted) return Json::array();

        // 整文件替换：订阅全量同步的编辑器（CLion 插件就是）用这个最省事，
        // 也避免在「行数变了」时算出错位的增量 range。
        const TextBuffer buf = doc.buffer();
        const int last_line = buf.line_count() - 1;
        int last_char = 0;
        if (last_line >= 0) last_char = static_cast<int>(buf.line_text(last_line).size());

        Json s = Json::object();
        s.set("line", 0);
        s.set("character", 0);
        Json e = Json::object();
        e.set("line", last_line < 0 ? 0 : last_line);
        e.set("character", last_char);
        Json range = Json::object();
        range.set("start", std::move(s));
        range.set("end", std::move(e));

        Json edit = Json::object();
        edit.set("range", std::move(range));
        edit.set("newText", formatted);

        Json arr = Json::array();
        arr.push(std::move(edit));
        return arr;
    }
} // namespace sinterlsp

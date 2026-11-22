// sinter-lsp: features.cpp —— 
//
// ：
//   completion / definition / hover / documentSymbol →  Index（lexical）
//   publishDiagnostics（diagnostics_message）→ lexicaldiagnostic + embed C   C diagnostic
// i.e. code （AST not ） result。
#include "server.hpp"
#include "format.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace sinterlsp {
    namespace {
        ///  offset LSP Range
        Range range_of(const TextBuffer &buf, const size_t beg, const size_t len) {
            Range r;
            r.start = buf.position_at(beg);
            r.end = buf.position_at(beg + len);
            return r;
        }

        Json range_json(const Range &r) {
            Json j = Json::object();
            Json s = Json::object();
            s.set("line", r.start.line);
            s.set("character", r.start.character);
            Json e = Json::object();
            e.set("line", r.end.line);
            e.set("character", r.end.character);
            j.set("start", std::move(s));
            j.set("end", std::move(e));
            return j;
        }

        Json location_json(const std::string &uri, const Range &r) {
            Json loc = Json::object();
            loc.set("uri", uri);
            loc.set("range", range_json(r));
            return loc;
        }

        /// Sinter keywordcompletioncolumn（：  i.e. completion）
        const char *kCompletionKeywords[] = {
            "module", "import", "pub", "fun", "class", "struct", "interface", "enum",
            "const", "val", "var", "alias", "comptime", "embed", "bind", "ext",
            "if", "else", "while", "for", "in", "do", "break", "continue", "return",
            "when", "is", "null", "true", "false", "self", "super",
            "override", "must", "inline", "static", "abstract", "final",
            "extends", "implements", "del", "copy", "move", "default", "from",
            // compileposition：， src/front/comptime.cpp
            "__FILE__", "__MODULE__", "__LINE__", "__COLUMN__",
            "__FUNCTION__", "__CLASS__", "__STRUCT__",
        };

        /// builtin type
        const char *kCompletionTypes[] = {
            "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
            "f32", "f64", "bool", "void", "char",
        };

        /// 模板补全表。`${n:xxx}` 是 LSP tabstop，`$0` 是最终光标位置。
        /// 与 kCompletionKeywords 里的关键字同名，但排在前面（sortText 加 "0_"）。
        struct Snippet {
            const char *label;
            const char *detail;
            const char *body;
        };
        const Snippet kSnippets[] = {
            // ---- 结构 ----
            {"module",   "模块头",           "module ${1:name}\n\n"},
            {"import",   "导入模块",         "import std.${1:string}"},
            {"fun",      "函数",             "fun ${1:name}(${2}): ${3:void} {\n    $0\n}"},
            {"pub fun",  "公开函数",         "pub fun ${1:name}(${2}): ${3:void} {\n    $0\n}"},
            {"fun =>",   "表达式函数",       "fun ${1:name}(${2}): ${3:i64} => ${4:expr}"},
            {"class",    "类",               "class ${1:Name} : ${2:Any} {\n    var ${3:field}: ${4:i64}\n\n    ${1:Name}(${5}) {\n        $0\n    }\n}"},
            {"struct",   "结构体",           "struct ${1:Name} {\n    var ${2:field}: ${3:i64}\n    $0\n}"},
            {"interface","接口",             "interface ${1:Name} {\n    fun ${2:method}(): ${3:void}\n    $0\n}"},
            {"enum",     "枚举",             "enum ${1:Name} {\n    ${2:A},\n    ${3:B}$0\n}"},
            {"ext",      "扩展函数",         "pub ext fun ${1:str}.${2:name}(): ${3:i64} => $0"},
            // ---- 控制流 ----
            {"if",       "if",               "if (${1:cond}) {\n    $0\n}"},
            {"ifelse",   "if / else",        "if (${1:cond}) {\n    ${2}\n} else {\n    $0\n}"},
            {"while",    "while",            "while (${1:cond}) {\n    $0\n}"},
            {"for",      "for-in 区间",      "for (${1:i} in ${2:0}..${3:n}) {\n    $0\n}"},
            {"forin",    "for-in 数组",      "for (${1:x} in ${2:items}) {\n    $0\n}"},
            {"when",     "when",             "when (${1:value}) {\n    ${2:case} => {\n        $0\n    }\n}"},
            // ---- 常用 ----
            {"main",     "入口函数",         "fun main(): void {\n    $0\n}"},
            {"println",  "打印",             "println(${1:value})"},
            {"embed C",  "内联 C",           "embed C {\n    $0\n}"},
            {"bind C",   "绑定 C 函数",      "bind C fun ${1:name}(${2}): ${3:void}"},
            {"val",      "不可变变量",       "val ${1:name}: ${2:i64} = $0"},
            {"var",      "可变变量",         "var ${1:name}: ${2:i64} = $0"},
            {"comptime", "编译期常量",       "comptime {\n    val ${1:NAME}: ${2:i64} = $0\n}"},
            // ---- 注释 ----
            {"todo",     "TODO 注释",        "// TODO: $0"},
            {"doc",      "文档注释",         "/// $0"},
        };

        /// position    `xxx.`  or  `xxx->`（completion）
        std::string member_receiver(const TextBuffer &buf, const size_t offset) {
            const std::string &t = buf.text();
            if (offset == 0 || offset > t.size()) return {};
            size_t i = offset;
            //  and 
            while (i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t')) --i;
            if (i == 0) return {};
            size_t dot_len = 0;
            if (i >= 2 && t[i - 1] == '>' && t[i - 2] == '-') dot_len = 2;
            else if (t[i - 1] == '.') dot_len = 1;
            if (dot_len == 0) return {};
            size_t end = i - dot_len;
            while (end > 0 && (t[end - 1] == ' ' || t[end - 1] == '\t')) --end;
            size_t beg = end;
            while (beg > 0 && ((t[beg - 1] >= 'a' && t[beg - 1] <= 'z') ||
                               (t[beg - 1] >= 'A' && t[beg - 1] <= 'Z') ||
                               (t[beg - 1] >= '0' && t[beg - 1] <= '9') || t[beg - 1] == '_'))
                --beg;
            return t.substr(beg, end - beg);
        }

        /// offset  class/struct（`self.` completionrequires）。
        /// lexicalsave each     container  and ；
        ///  「declarationposition  、and  container     」 class。
        std::string current_container_at(const Index &idx, const size_t offset) {
            std::string best;
            int best_depth = -1;
            for (const auto &s: idx.symbols) {
                if (s.container.empty()) continue;
                if (s.decl_offset > offset) continue;    //  already  
                if (s.scope_depth > best_depth) {
                    best_depth = s.scope_depth;
                    best = s.container;
                }
            }
            return best;
        }
    } // namespace

    Json Server::on_completion(const std::string &uri, const Position pos) {
        const Index &idx = index_of(uri);
        const TextBuffer &buf = idx.buf;
        const size_t off = buf.offset_at(pos);

        Json items = Json::array();
        std::set<std::string> seen;
        auto add_item = [&items, &seen](const std::string &label, const int kind,
                                        const std::string &detail) {
            if (label.empty() || !seen.insert(label).second) return;
            Json it = Json::object();
            it.set("label", label);
            it.set("kind", kind);
            if (!detail.empty()) it.set("detail", detail);
            items.push(std::move(it));
        };
        // snippet：插入带占位符的模板。`${1:name}` 是 LSP 的 tabstop 语法，
        // 编辑器（CLion）会把它变成可跳转的占位符。
        auto add_snippet = [&items, &seen](const std::string &label, const int kind,
                                           const std::string &detail,
                                           const std::string &body,
                                           const std::string &sort_prefix) {
            if (label.empty() || !seen.insert(label).second) return;
            Json it = Json::object();
            it.set("label", label);
            it.set("kind", kind);
            if (!detail.empty()) it.set("detail", detail);
            it.set("insertTextFormat", 2);          // 2 = Snippet
            it.set("insertText", body);
            Json doc = Json::object();
            doc.set("kind", "markdown");
            doc.set("value", "```sinter\n" + body + "\n```");
            it.set("documentation", std::move(doc));
            // 让模板排在同名关键字前面
            it.set("sortText", sort_prefix + label);
            items.push(std::move(it));
        };

        // 1) ：the class/the struct field and method
        //
        // lexicalno type，  「 declarationtype」：
        //   fun use(r: Res) { r. }   → r   type_name = "Res" → column Res  
        //   var x = Res()            → no  type_name，is completion（not ）
        // `self.` ：    container。
        const std::string recv = member_receiver(buf, off);
        if (!recv.empty()) {
            std::string recv_type = recv == "self" ? current_container_at(idx, off) : std::string();
            if (recv_type.empty()) {
                //  declaration：  declaration
                const Symbol *best = nullptr;
                for (const auto &s: idx.symbols) {
                    if (s.name != recv || s.type_name.empty()) continue;
                    if (!best || s.scope_depth >= best->scope_depth) best = &s;
                }
                if (best) recv_type = best->type_name;
            }
            if (!recv_type.empty()) {
                for (const auto &s: idx.symbols) {
                    if (s.container != recv_type) continue;
                    if (s.kind != SymKind::Field && s.kind != SymKind::Method &&
                        s.kind != SymKind::Constant)
                        continue;
                    add_item(s.name, sym_kind_lsp(s.kind), s.detail);
                }
                // completionreturn the type ：i.e. is  not ，
                // then   `r.`   `fun/class/if` will 。
                return items;
            }
            // typenot  （e.g.  `var x = Res()`）：is completion，not 
        }

        // 2) 模板（snippet）：先给，这样排在关键字前面
        for (const auto &t: kSnippets) {
            add_snippet(t.label, 15 /* Snippet */, t.detail, t.body, "0_");
        }

        // 3) ：file  +  + builtin type
        for (const auto &s: idx.symbols) {
            add_item(s.name, sym_kind_lsp(s.kind), s.detail);
        }
        for (const char *kw: kCompletionKeywords) add_item(kw, 14, "");
        for (const char *ty: kCompletionTypes) add_item(ty, 25, "builtin type");
        return items;
    }

    Json Server::on_definition(const std::string &uri, const Position pos) {
        const Index &idx = index_of(uri);
        const TextBuffer &buf = idx.buf;
        const size_t off = buf.offset_at(pos);

        std::string name;
        size_t beg = 0, len = 0;
        if (!idx.ident_at(off, name, beg, len)) return Json();   // null = no definition

        const auto syms = idx.find_symbols(name);
        if (syms.empty()) return Json();

        // return 「 」 declaration（scope_depth  ），
        const Symbol *best = syms.front();
        for (const Symbol *s: syms)
            if (s->scope_depth > best->scope_depth) best = s;

        return location_json(uri, range_of(buf, best->decl_offset, best->name_len));
    }

    Json Server::on_hover(const std::string &uri, const Position pos) {
        const Index &idx = index_of(uri);
        const TextBuffer &buf = idx.buf;
        const size_t off = buf.offset_at(pos);

        std::string name;
        size_t beg = 0, len = 0;
        if (!idx.ident_at(off, name, beg, len)) return Json();

        // declaration info
        std::string detail;
        if (const Symbol *self = idx.symbol_decl_at(off)) {
            detail = self->detail;
            if (!self->container.empty()) detail += "  (in " + self->container + ")";
        } else {
            const auto syms = idx.find_symbols(name);
            if (!syms.empty()) {
                detail = syms.front()->detail;
                if (!syms.front()->container.empty())
                    detail += "  (in " + syms.front()->container + ")";
            }
        }
        if (detail.empty()) {
            // not  filedeclaration  ：  or builtin type
            for (const char *kw: kCompletionKeywords) {
                if (name == kw) {
                    detail = "Sinter keyword `" + name + "`";
                    break;
                }
            }
            if (detail.empty()) {
                for (const char *ty: kCompletionTypes) {
                    if (name == ty) {
                        detail = "builtin type `" + name + "`";
                        break;
                    }
                }
            }
        }
        if (detail.empty()) return Json();

        Json content = Json::object();
        content.set("kind", "markdown");
        content.set("value", "```sinter\n" + detail + "\n```");

        Json result = Json::object();
        result.set("contents", std::move(content));
        result.set("range", range_json(range_of(buf, beg, len)));
        return result;
    }

    Json Server::on_document_symbol(const std::string &uri) {
        const Index &idx = index_of(uri);
        const TextBuffer &buf = idx.buf;
        if (idx.symbols.empty()) return Json::array();

        //  scope_depth ：depth 0 is ，  to    。
        //  array，  column，「 JSON 」 。
        const size_t n = idx.symbols.size();
        std::vector<int> parent(n, -1);
        std::vector<std::vector<size_t>> kids(n);
        std::vector<size_t> roots;
        std::vector<std::pair<int, size_t>> stack;   // (depth, symbol index)

        for (size_t i = 0; i < n; ++i) {
            const int d = idx.symbols[i].scope_depth;
            while (!stack.empty() && stack.back().first >= d) stack.pop_back();
            if (stack.empty()) {
                roots.push_back(i);
            } else {
                parent[i] = static_cast<int>(stack.back().second);
                kids[stack.back().second].push_back(i);
            }
            stack.emplace_back(d, i);
        }

        // column
        std::function<Json(size_t)> build = [&](const size_t i) -> Json {
            const Symbol &s = idx.symbols[i];
            Json item = Json::object();
            item.set("name", s.name);
            item.set("kind", sym_kind_lsp(s.kind));
            item.set("range", range_json(range_of(buf, s.decl_offset, s.name_len)));
            item.set("selectionRange", range_json(range_of(buf, s.decl_offset, s.name_len)));
            if (!s.detail.empty()) item.set("detail", s.detail);
            if (!kids[i].empty()) {
                Json children = Json::array();
                for (const size_t c: kids[i]) children.push(build(c));
                item.set("children", std::move(children));
            }
            return item;
        };

        Json result = Json::array();
        for (const size_t r: roots) result.push(build(r));
        return result;
    }

    std::string Server::diagnostics_message(const std::string &uri) {
        const Index &idx = index_of(uri);
        const TextBuffer &buf = idx.buf;

        Json diags = Json::array();
        for (const auto &d: idx.diagnostics) {
            Json j = Json::object();
            j.set("range", range_json(range_of(buf, d.offset, d.length)));
            j.set("severity", d.severity);
            j.set("source", "sinter");
            j.set("message", d.message);
            diags.push(std::move(j));
        }

        // not   hint：classerror   
        int depth = 0;
        size_t last_open = 0;
        for (size_t i = 0; i < buf.text().size(); ++i) {
            const char c = buf.text()[i];
            if (c == '{') {
                ++depth;
                last_open = i;
            } else if (c == '}') {
                if (depth > 0) --depth;
            }
        }
        if (depth > 0) {
            Json j = Json::object();
            j.set("range", range_json(range_of(buf, last_open, 1)));
            j.set("severity", 2);
            j.set("source", "sinter");
            j.set("message", " " + std::to_string(depth) + " unclosed '{'");
            diags.push(std::move(j));
        }

        Json params = Json::object();
        params.set("uri", uri);
        params.set("diagnostics", std::move(diags));
        Json note = Json::object();
        note.set("jsonrpc", "2.0");
        note.set("method", "textDocument/publishDiagnostics");
        note.set("params", std::move(params));
        return note.dump();
    }
    Json Server::on_format(const std::string &uri) {
        const Document *doc = document(uri);
        if (!doc) return Json::array();
        const std::string formatted = format_sinter(doc->text);
        return format_to_lsp_edits(doc->text, formatted, *doc);
    }

    Json Server::on_range_format(const std::string &uri, Range /*range*/) {
        // 当前不做局部格式化：Sinter 的缩进是「从文件开头累积」出来的，
        // 单独格式化一段时得知道该段原本的缩进层级，否则会把嵌套块压平。
        // 走整文件替换在语义上等价（用户看到的只是「格式化整个文件」），
        // 而且绝不会产出错位的增量 range。
        return on_format(uri);
    }
} // namespace sinterlsp

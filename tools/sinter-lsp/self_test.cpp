// sinter-lsp: self_test.cpp —— self-test（--self-test）
//
// override：JSON parse、position、、、error。
// not   stdin/stdout（requires），protocol and 。
#include "server.hpp"

#include "document.hpp"
#include "index.hpp"
#include "json.hpp"

#include <cstdio>
#include <string>

namespace {
    int g_failed = 0;
    int g_total = 0;

    void check(const char *what, const bool ok, const std::string &detail = "") {
        ++g_total;
        if (ok) {
            std::printf("  [ OK ] %s\n", what);
        } else {
            ++g_failed;
            std::printf("  [FAIL] %s\n", what);
            if (!detail.empty()) std::printf("         %s\n", detail.c_str());
        }
    }

    bool contains(const std::string &haystack, const std::string &needle) {
        return haystack.find(needle) != std::string::npos;
    }
} // namespace

int run_self_test() {
    using namespace sinterlsp;

    std::printf("sinter-lsp self-test\n");

    // ---------- JSON ----------
    {
        std::string err;
        Json j = Json::parse(R"({"a":1,"b":"x","c":[true,false],"d":{"e":null}})", err);
        check("JSON parse ", j.is_object() && err.empty(), err);
        check("JSON integer", j.get_int("a") == 1);
        check("JSON string", j.get_string("b") == "x");
        const Json *c = j.get("c");
        check("JSON boolarray", c && c->is_array() && c->size() == 2 && c->as_array()[0].as_bool());
        const Json *d = j.get("d");
        check("JSON  null", d && d->get("e") && d->get("e")->is_null());

        Json bad = Json::parse("{", err);
        check("JSON errorinputreturn  null", bad.is_null());

        // 
        Json obj = Json::object();
        obj.set("k", "v\n\"q\"");
        Json back = Json::parse(obj.dump(), err);
        check("JSON ", back.get_string("k") == "v\n\"q\"", back.dump());
    }

    // ---------- position ----------
    {
        TextBuffer buf("ab\ncd\n");
        check("line", buf.line_count() == 3, std::to_string(buf.line_count()));
        check("lineoffset", buf.line_start(1) == 3);
        check("Position -> offset", buf.offset_at(Position{1, 1}) == 4);
        const Position p = buf.position_at(4);
        check("offset -> Position", p.line == 1 && p.character == 1,
              std::to_string(p.line) + "," + std::to_string(p.character));

        // ： a ，UTF-16 a 
        TextBuffer mb("\xE4\xB8\xAD" "x");
        check("UTF-8 ", mb.offset_at(Position{0, 1}) == 3);
        check("UTF-8 ", mb.position_at(3).character == 1);

        // emoji requires 2   UTF-16 （U+1F600   BMP ）。
        //  \u not   \xF0\x9F\x98\x80：C/C++   \x   ，
        //  （ a string） will  a ，
        //  "hex escape sequence out of range"。\u  4 、not will 。
        TextBuffer emoji("\U0001F600" "a");
        // 「😀a」 5 ：😀  4  / 2   UTF-16 ，'a'  1 。
        // offset 4（😀 end） column 2，offset 5（'a' end） column 3。
        check("emoji  2  ", emoji.position_at(4).character == 2,
              std::to_string(emoji.position_at(4).character));
        check("emoji offset", emoji.offset_at(Position{0, 2}) == 4,
              std::to_string(emoji.offset_at(Position{0, 2})));
        check("emoji   column", emoji.position_at(5).character == 3,
              std::to_string(emoji.position_at(5).character));
    }

    // ----------  ----------
    const std::string kSrc =
        "module demo\n"
        "\n"
        "import std\n"
        "\n"
        "class Res {\n"
        "    val id: i64\n"
        "    fun get() -> i64 { return self.id }\n"
        "}\n"
        "\n"
        "fun add(a: i64, b: i64) -> i64 {\n"
        "    var total: i64 = a + b\n"
        "    return total\n"
        "}\n"
        "\n"
        "embed C {\n"
        "int helper(int x) { return x * 2; }\n"
        "}\n";
    {
        Index idx = build_index("file:///demo.sinter", kSrc);
        check("：diagnostic", idx.diagnostics.empty(),
              idx.diagnostics.empty() ? "" : idx.diagnostics.front().message);
        check("：to  module demo", !idx.find_symbols("demo").empty());
        check("：to  class Res", !idx.find_symbols("Res").empty());
        check("：to  method get", !idx.find_symbols("get").empty());
        check("：to  field id", !idx.find_symbols("id").empty());
        check("：to  function add", !idx.find_symbols("add").empty());
        check("：to  variable total", !idx.find_symbols("total").empty());
        check("：args ", !idx.find_symbols("a").empty());
        check("：embed C range", idx.embed_c.found && idx.embed_c.body_end > idx.embed_c.body_begin);
        //  package helper
        const std::string body = idx.buf.slice(idx.embed_c.body_begin, idx.embed_c.body_end);
        check("：embed C ", contains(body, "helper"), body.substr(0, 40));

        // method get   container   Res
        const auto syms = idx.find_symbols("get");
        check("：methodclass", !syms.empty() && syms.front()->container == "Res",
              syms.empty() ? "not to " : syms.front()->container);
    }

    // ---------- service：initialize ----------
    Server srv;
    {
        auto out = srv.handle_message(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
        check("initialize response", out.size() == 1);
        const std::string &msg = out[0];
        check("declaration completion ", contains(msg, "completionProvider"));
        check("declaration definition ", contains(msg, "\"definitionProvider\":true"));
        check("declaration hover ", contains(msg, "\"hoverProvider\""));
        check("declaration documentSymbol ", contains(msg, "\"documentSymbolProvider\":true"));
        check("declaration", contains(msg, "\"change\":1"));
    }

    // ---------- service：didOpen + diagnostic ----------
    {
        const std::string uri = "file:///demo.sinter";
        Json open = Json::object();
        Json td = Json::object();
        td.set("uri", uri).set("languageId", "sinter").set("version", 1).set("text", kSrc);
        open.set("textDocument", std::move(td));
        Json note = Json::object();
        note.set("jsonrpc", "2.0").set("method", "textDocument/didOpen").set("params", std::move(open));
        auto out = srv.handle_message(note.dump());
        check("didOpen diagnostic", out.size() == 1 && contains(out[0], "publishDiagnostics"));
        check("codeerrordiagnostic", contains(out[0], "\"diagnostics\":[]"));

        // a  '}'  not 
        const std::string broken = "fun f() {\n    var x = 1\n";
        Json change = Json::object();
        Json td2 = Json::object();
        td2.set("uri", uri).set("version", 2);
        change.set("textDocument", std::move(td2));
        Json changes = Json::array();
        Json ch = Json::object();
        ch.set("text", broken);
        changes.push(std::move(ch));
        change.set("contentChanges", std::move(changes));
        Json n2 = Json::object();
        n2.set("jsonrpc", "2.0").set("method", "textDocument/didChange");
        n2.set("params", std::move(change));
        auto out2 = srv.handle_message(n2.dump());
        check("not codenot ", out2.size() == 1 && contains(out2[0], "unclosed"), out2.empty() ? "" : out2[0].substr(0, 120));

        // content test
        srv.set_document(uri, kSrc);
    }

    // ---------- completion ----------
    {
        const std::string uri = "file:///demo.sinter";
        Json p = Json::object();
        Json td = Json::object();
        td.set("uri", uri);
        p.set("textDocument", std::move(td));
        Json pos = Json::object();
        pos.set("line", 11).set("character", 4);   // `return total` line，kSrc  12 line（0  11）
        p.set("position", std::move(pos));
        Json req = Json::object();
        req.set("jsonrpc", "2.0").set("id", 2).set("method", "textDocument/completion");
        req.set("params", std::move(p));
        auto out = srv.handle_message(req.dump());
        const std::string &msg = out[0];
        check("completionreturn array", contains(msg, "\"result\":["), msg.substr(0, 160));
        check("completionfile  total", contains(msg, "\"label\":\"total\""));
        check("completion fun", contains(msg, "\"label\":\"fun\""));
        check("completionbuiltin type i64", contains(msg, "\"label\":\"i64\""));
    }

    // ---------- completion ----------
    {
        const std::string uri = "file:///demo.sinter";
        const std::string src2 =
            "class Res {\n    val id: i64\n    fun get() -> i64 { return self.id }\n}\n\n"
            "fun use(r: Res) {\n    r.\n}\n";
        srv.set_document(uri, src2);
        Json p = Json::object();
        Json td = Json::object();
        td.set("uri", uri);
        p.set("textDocument", std::move(td));
        Json pos = Json::object();
        pos.set("line", 6).set("character", 6);   // `    r.`  （0  6 line， 6）
        p.set("position", std::move(pos));
        Json req = Json::object();
        req.set("jsonrpc", "2.0").set("id", 3).set("method", "textDocument/completion");
        req.set("params", std::move(p));
        auto out = srv.handle_message(req.dump());
        check("completion id", contains(out[0], "\"label\":\"id\""), out[0].substr(0, 160));
        check("completion get", contains(out[0], "\"label\":\"get\""));
        check("completionnot  fun", !contains(out[0], "\"label\":\"fun\""));
    }

    // ---------- definition ----------
    {
        const std::string uri = "file:///demo.sinter";
        srv.set_document(uri, kSrc);
        Json p = Json::object();
        Json td = Json::object();
        td.set("uri", uri);
        p.set("textDocument", std::move(td));
        Json pos = Json::object();
        // `return total`   kSrc   0  11 line，`total`  4 column
        pos.set("line", 11).set("character", 11);
        p.set("position", std::move(pos));
        Json req = Json::object();
        req.set("jsonrpc", "2.0").set("id", 4).set("method", "textDocument/definition");
        req.set("params", std::move(p));
        auto out = srv.handle_message(req.dump());
        check("definitionreturn  location", contains(out[0], "\"uri\":\"file:///demo.sinter\""),
              out[0].substr(0, 160));
        // total  declaration  0  10 line（`    var total: i64 = a + b`）
        check("definitiondeclarationline", contains(out[0], "\"line\":10"), out[0].substr(0, 200));
    }

    // ---------- hover ----------
    {
        const std::string uri = "file:///demo.sinter";
        Json p = Json::object();
        Json td = Json::object();
        td.set("uri", uri);
        p.set("textDocument", std::move(td));
        Json pos = Json::object();
        pos.set("line", 11).set("character", 11);   // ：`return total`   total
        p.set("position", std::move(pos));
        Json req = Json::object();
        req.set("jsonrpc", "2.0").set("id", 5).set("method", "textDocument/hover");
        req.set("params", std::move(p));
        auto out = srv.handle_message(req.dump());
        check("hoverreturn  markdown", contains(out[0], "\"kind\":\"markdown\""), out[0].substr(0, 160));
        check("hover ", contains(out[0], "total"));
    }

    // ---------- document ----------
    {
        const std::string uri = "file:///demo.sinter";
        Json p = Json::object();
        Json td = Json::object();
        td.set("uri", uri);
        p.set("textDocument", std::move(td));
        Json req = Json::object();
        req.set("jsonrpc", "2.0").set("id", 6).set("method", "textDocument/documentSymbol");
        req.set("params", std::move(p));
        auto out = srv.handle_message(req.dump());
        check("return array", contains(out[0], "\"result\":["));
        check(" Res", contains(out[0], "\"name\":\"Res\""));
        check(" add", contains(out[0], "\"name\":\"add\""));
        // Res  is  children 
        check(" children", contains(out[0], "\"children\""));
    }

    // ---------- error ----------
    {
        auto out = srv.handle_message("{}");
        check(" method ", out.size() == 1 && contains(out[0], "\"error\""), out[0].substr(0, 120));

        auto out2 = srv.handle_message(
            R"({"jsonrpc":"2.0","id":9,"method":"textDocument/unknown","params":{"textDocument":{"uri":"file:///demo.sinter"}}})");
        check("not method", contains(out2[0], "-32601"), out2[0].substr(0, 120));

        // missing textDocument.uri  methodargserror，not  to 「not 」
        auto out2b = srv.handle_message(
            R"({"jsonrpc":"2.0","id":10,"method":"textDocument/hover","params":{}})");
        check(" uri argserror", contains(out2b[0], "-32602"), out2b[0].substr(0, 120));

        auto out3 = srv.handle_message("not json");
        check("invalid JSON  -32700", contains(out3[0], "-32700"), out3[0].substr(0, 120));
    }

    // ---------- 格式化 ----------
    // 这几条是 format.cpp 的行为契约（见该文件头的 9 条规则），改规则先改这里。
    {
        srv.handle_message(
            R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///fmt.sinter","languageId":"sinter","version":1,"text":"module demo\nfun add(a:i64,b:i64):i64{\nreturn a+b\n}\nif(x>2){\nprintln(\"big\")\n}else{\nprintln(\"small\")\n}\n"}}})");

        auto out = srv.handle_message(
            R"({"jsonrpc":"2.0","id":40,"method":"textDocument/formatting","params":{"textDocument":{"uri":"file:///fmt.sinter"},"options":{"tabSize":4,"insertSpaces":true}}})");
        check("formatting  result", out.size() == 1 && contains(out[0], "\"newText\""),
              out.empty() ? "no response" : out[0].substr(0, 160));

        const std::string msg = out.empty() ? std::string() : out[0];
        // 整文件替换：range 从 0,0 开始
        check("formatting  range 0,0", contains(msg, "\"line\":0"));
        // 缩进 4 空格
        check("formatting 4 ", contains(msg, "    return a + b"));
        // 二元运算符两侧空格
        check("formattingoperator ", contains(msg, "a + b"));
        // `:` 后一个空格、`,` 后一个空格
        check("formatting `:`  ", contains(msg, "a: i64, b: i64): i64 {"));
        // `{` 留在行尾
        check("formatting `{` ", contains(msg, "fun add(a: i64, b: i64): i64 {"));
        // `}` 单独成行
        check("formatting `}` ", contains(msg, "\\n}"));
        // `} else {` 同行
        check("formatting `} else {` ", contains(msg, "} else {"));
        // 字符串内部一个字节不动
        check("formatting ", contains(msg, "\\\"big\\\""));
        // 不可逆的破坏：绝不出现词被粘在一起
        check("formatting ", !contains(msg, "moduledemo"));
    }

    // ---------- 格式化：Sinter 专有语法 ----------
    // `=>` 不能被拆成 `= >`，一元负号不能变成二元，默认构造与文档注释原样保留。
    {
        const std::string src =
            "module m\\n"
            "/// doc comment\\n"
            "/** block doc */\\n"
            "class Any{\\n"
            "Any() = default\\n"
            "fun f():i64=>1\\n"
            "fun g():i64{ret -1}\\n"
            "fun h():i64=>Int(-1)\\n"
            "}\\n";
        srv.handle_message(
            R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///fmt2.sinter","languageId":"sinter","version":1,"text":")" +
            src + R"("}}})");
        auto out = srv.handle_message(
            R"({"jsonrpc":"2.0","id":42,"method":"textDocument/formatting","params":{"textDocument":{"uri":"file:///fmt2.sinter"},"options":{"tabSize":4,"insertSpaces":true}}})");
        const std::string msg = out.empty() ? std::string() : out[0];
        check("formatting => ", contains(msg, "fun f(): i64 => 1"), msg.substr(0, 200));
        check("formatting =>  = >", !contains(msg, "= >"));
        check("formatting  = default", contains(msg, "Any() = default"));
        check("formatting  -1", contains(msg, "ret -1"));
        check("formatting  (-1)", contains(msg, "Int(-1)"));
        check("formatting /// 原样", contains(msg, "/// doc comment"));
        check("formatting /** */ 原样", contains(msg, "/** block doc */"));
    }

    // ---------- rangeFormatting ----------
    {
        auto out = srv.handle_message(
            R"({"jsonrpc":"2.0","id":41,"method":"textDocument/rangeFormatting","params":{"textDocument":{"uri":"file:///fmt.sinter"},"range":{"start":{"line":0,"character":0},"end":{"line":3,"character":0}},"options":{"tabSize":4,"insertSpaces":true}}})");
        check("rangeFormatting  result", out.size() == 1 && contains(out[0], "\"newText\""),
              out.empty() ? "no response" : out[0].substr(0, 160));
    }

    // ---------- 模板（snippet） ----------
    {
        auto out = srv.handle_message(
            R"({"jsonrpc":"2.0","id":42,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///fmt.sinter"},"position":{"line":0,"character":0}}})");
        const std::string msg = out.empty() ? std::string() : out[0];

        // 模板必须是 snippet 格式，否则编辑器会把 `${1:name}` 当字面量插进去
        check("snippet insertTextFormat=2", contains(msg, "\"insertTextFormat\":2"), msg.substr(0, 160));
        check("snippet  tabstop", contains(msg, "${1:"));
        check("snippet  $0", contains(msg, "$0"));

        // 抽样几条关键模板
        check("snippet fun ", contains(msg, "\"label\":\"fun\""));
        check("snippet class", contains(msg, "\"label\":\"class\""));
        check("snippet embed C", contains(msg, "\"label\":\"embed C\""));
        check("snippet main", contains(msg, "\"label\":\"main\""));
        check("snippet when", contains(msg, "\"label\":\"when\""));
    }

    // ---------- shutdown / exit ----------
    {
        auto out = srv.handle_message(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");
        check("shutdown return  null result", contains(out[0], "\"result\":null"), out[0].substr(0, 120));
    }

    if (g_failed == 0) std::printf("all passed（%d ）\n", g_total);
    else std::printf("failed %d / %d \n", g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
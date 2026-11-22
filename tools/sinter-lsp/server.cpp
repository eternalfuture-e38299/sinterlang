// sinter-lsp: server.cpp —— protocol、document、
#include "server.hpp"
#include "cfront/c_parser.hpp"

#include <sinter/version.hpp>

#include <fstream>
#include <sstream>

#include "transport.hpp"

#include <iostream>
#include <utility>

namespace sinterlsp {
    BackendClient &backend() {
        static BackendClient c;
        return c;
    }

    Server::Server() = default;

    const Document *Server::document(const std::string &uri) const {
        const auto it = docs_.find(uri);
        return it == docs_.end() ? nullptr : &it->second;
    }

    void Server::set_document(const std::string &uri, std::string text, const std::string &lang,
                              const int version) {
        Document d;
        d.uri = uri;
        d.text = std::move(text);
        d.language_id = lang;
        d.version = version;
        docs_[uri] = std::move(d);
        index_cache_.erase(uri);
    }

    const Index &Server::index_of(const std::string &uri) {
        const auto cached = index_cache_.find(uri);
        if (cached != index_cache_.end()) return cached->second;
        const Document *d = document(uri);
        if (!d) {
            // return a （call）。
            // not  `Index{TextBuffer()}`，will  -Wmissing-field-initializers；
            //  default +  buf，semantic 。
            static const Index empty = [] {
                Index e;
                e.buf = TextBuffer(std::string());
                return e;
            }();
            return empty;
        }
        auto ins = index_cache_.emplace(uri, build_index(uri, d->text));
        return ins.first->second;
    }

    Json Server::make_response(const Json &id, Json result) {
        Json r = Json::object();
        r.set("jsonrpc", "2.0");
        r.set("id", id);
        r.set("result", std::move(result));
        return r;
    }

    Json Server::make_error(const Json &id, const int code, const std::string &message) {
        Json err = Json::object();
        err.set("code", code);
        err.set("message", message);
        Json r = Json::object();
        r.set("jsonrpc", "2.0");
        r.set("id", id);
        r.set("error", std::move(err));
        return r;
    }

    void Server::send(const std::string &body) const { write_message(std::cout, body); }

    Json Server::on_initialize(const Json & /*params*/) {
        // declaration： ，callnot  method
        Json completion = Json::object();
        // `>` （） and  `.` 
        Json triggers = Json::array();
        triggers.push(".").push(">").push(":");
        completion.set("triggerCharacters", triggers);

        // 标准结构是 completionProvider.completionItem.{snippetSupport,documentationFormat}。
        // 之前这里多套了一层 completionItem，导致真实编辑器读不到 snippetSupport，
        // 模板会被当成普通文本插入（`${1:name}` 原样落进代码里）。
        Json item = Json::object();
        item.set("labelDetailsSupport", false);
        // snippet：模板补全（`fun` → `fun name(args): ret { ... }`）靠它
        item.set("snippetSupport", true);
        item.set("documentationFormat", [] {
            Json d = Json::array();
            d.push("markdown").push("plaintext");
            return d;
        }());
        completion.set("completionItem", item);

        Json sync = Json::object();
        sync.set("openClose", true);
        // 1 = Full（each ）：  ，semantic 
        sync.set("change", 1);
        sync.set("save", true);

        Json hover = Json::object();
        hover.set("contentFormat", [] {
            Json a = Json::array();
            a.push("markdown").push("plaintext");
            return a;
        }());

        Json capabilities = Json::object();
        capabilities.set("textDocumentSync", sync);
        capabilities.set("completionProvider", completion);
        capabilities.set("definitionProvider", true);
        capabilities.set("hoverProvider", hover);
        capabilities.set("documentSymbolProvider", true);
        // 格式化：整文件替换（见 format.cpp）
        capabilities.set("documentFormattingProvider", true);
        capabilities.set("documentRangeFormattingProvider", true);
        // definition：client sinter/cHeaderSymbols  C   symbol。
        capabilities.set("sinter/cHeaderSymbols", true);

        Json server_info = Json::object();
        server_info.set("name", "sinter-lsp");
        server_info.set("version", SINTER_VERSION_STRING);

        Json result = Json::object();
        result.set("capabilities", std::move(capabilities));
        result.set("serverInfo", std::move(server_info));
        return result;
    }

    void Server::on_initialized() { initialized_ = true; }

    void Server::on_did_open(const Json &params, std::vector<std::string> &out) {
        const Json *td = params.get("textDocument");
        if (!td) return;
        const std::string uri = td->get_string("uri");
        if (uri.empty()) return;
        set_document(uri, td->get_string("text"), td->get_string("languageId", "sinter"),
                     td->get_int("version", 1));
        out.push_back(diagnostics_message(uri));
    }

    void Server::on_did_change(const Json &params, std::vector<std::string> &out) {
        const Json *td = params.get("textDocument");
        if (!td) return;
        const std::string uri = td->get_string("uri");
        if (uri.empty()) return;
        const Json *changes = params.get("contentChanges");
        if (!changes || !changes->is_array() || changes->as_array().empty()) return;
        // declaration  Full ，  a  change   text is 
        const auto &arr = changes->as_array();
        const std::string text = arr.back().get_string("text");
        const int version = td->get_int("version", 0);
        const Document *old = document(uri);
        set_document(uri, text, old ? old->language_id : "sinter", version);
        out.push_back(diagnostics_message(uri));
    }

    void Server::on_did_close(const Json &params, std::vector<std::string> &out) {
        const Json *td = params.get("textDocument");
        if (!td) return;
        const std::string uri = td->get_string("uri");
        docs_.erase(uri);
        index_cache_.erase(uri);
        // the file diagnostic（LSP ：closefile diagnostic）
        Json p = Json::object();
        p.set("uri", uri);
        p.set("diagnostics", Json::array());
        Json n = Json::object();
        n.set("jsonrpc", "2.0");
        n.set("method", "textDocument/publishDiagnostics");
        n.set("params", std::move(p));
        out.push_back(n.dump());
    }

    void Server::on_did_save(const Json &params, std::vector<std::string> &out) {
        const Json *td = params.get("textDocument");
        if (!td) return;
        const std::string uri = td->get_string("uri");
        index_cache_.erase(uri);
        out.push_back(diagnostics_message(uri));
    }

    std::vector<std::string> Server::handle_message(const std::string &body) {
        std::vector<std::string> out;

        std::string err;
        Json msg = Json::parse(body, err);
        if (msg.is_null() || !msg.is_object()) {
            out.push_back(make_error(Json(), -32700, err.empty() ? "parseerror" : err).dump());
            return out;
        }

        const std::string method = msg.get_string("method");
        const Json *id = msg.get("id");
        const Json *params = msg.get("params");
        static const Json kEmptyParams = Json::object();

        // no  method  no  id   not    JSON-RPC ：
        // must method，requestmust method。return  -32600 not  ，
        // then clientwill etcnot to response（ self-test「 」class）。
        if (method.empty()) {
            out.push_back(make_error(id ? *id : Json(), -32600, "missing method").dump());
            return out;
        }

        // ---- （no  id）----
        if (!id) {
            if (method == "initialized") {
                on_initialized();
            } else if (method == "textDocument/didOpen") {
                if (params) on_did_open(*params, out);
            } else if (method == "textDocument/didChange") {
                if (params) on_did_change(*params, out);
            } else if (method == "textDocument/didClose") {
                if (params) on_did_close(*params, out);
            } else if (method == "textDocument/didSave") {
                if (params) on_did_save(*params, out);
            } else if (method == "exit") {
                //  run() exit
            }
            return out;
        }

        // ---- request ----
        if (method == "initialize") {
            out.push_back(make_response(*id, on_initialize(params ? *params : kEmptyParams)).dump());
            return out;
        }
        // sinter/cHeaderSymbols：parse  C file（params.path），
        // return  functionand struct/union/enum 。Sinter notduplicate C
        // parse， src/cfront（and sinterc   bind C header path）。
        if (method == "sinter/cHeaderSymbols") {
            const std::string path = params ? params->get_string("path") : std::string();
            if (path.empty()) {
                out.push_back(make_error(*id, -32602, "missing path").dump());
                return out;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                out.push_back(make_error(*id, -32603, "cannot read header: " + path).dump());
                return out;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            const auto r = sinter::cfront::CParser::parse(ss.str(), path);
            std::string functions, tags;
            for (const auto &item: r.unit.items) {
                if (item.kind != sinter::cfront::CUnitItem::Kind::Declaration) continue;
                // `struct Demo { ... };` nodeclaration，fromtypedescription 。
                const std::string c = sinter::cfront::canonical_type(item.decl.spec);
                const bool is_tag = c.rfind("struct ", 0) == 0 || c.rfind("union ", 0) == 0 ||
                                    c.rfind("enum ", 0) == 0;
                if (is_tag) {
                    if (!tags.empty()) tags += "\n";
                    tags += c;
                    if (!item.decl.is_function()) continue;
                }
                const std::string name = item.decl.name();
                if (name.empty()) continue;
                if (item.decl.is_function()) {
                    if (!functions.empty()) functions += "\n";
                    functions += name;
                }
            }
            Json res = Json::object();
            res.set("functions", functions);
            res.set("types", tags);
            res.set("errors", static_cast<int>(r.errors.size()));
            out.push_back(make_response(*id, res).dump());
            return out;
        }

        if (method == "shutdown") {
            shutdown_requested_ = true;
            out.push_back(make_response(*id, Json()).dump());
            return out;
        }

        // requiresdocument request： uri  and position
        const Json *td = params ? params->get("textDocument") : nullptr;
        const std::string uri = td ? td->get_string("uri") : std::string();
        if (uri.empty()) {
            out.push_back(make_error(*id, -32602, "missing textDocument.uri").dump());
            return out;
        }

        auto pos_of = [params]() {
            Position p;
            if (params) {
                if (const Json *jp = params->get("position")) {
                    p.line = jp->get_int("line", 0);
                    p.character = jp->get_int("character", 0);
                }
            }
            return p;
        };

        if (method == "textDocument/completion") {
            out.push_back(make_response(*id, on_completion(uri, pos_of())).dump());
        } else if (method == "textDocument/definition") {
            out.push_back(make_response(*id, on_definition(uri, pos_of())).dump());
        } else if (method == "textDocument/hover") {
            out.push_back(make_response(*id, on_hover(uri, pos_of())).dump());
        } else if (method == "textDocument/documentSymbol") {
            out.push_back(make_response(*id, on_document_symbol(uri)).dump());
        } else if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") {
            out.push_back(make_response(*id, on_format(uri)).dump());
        } else {
            out.push_back(make_error(*id, -32601, "not  method: " + method).dump());
        }
        return out;
    }

    int Server::run() {
        while (!shutdown_requested_) {
            std::string body;
            if (!read_message(std::cin, body)) break;
            for (const auto &resp: handle_message(body)) send(resp);
        }
        // shutdown  etc exit， or  stdin closeexit
        while (shutdown_requested_) {
            std::string body;
            if (!read_message(std::cin, body)) break;
            for (const auto &resp: handle_message(body)) send(resp);
            if (body.find("\"exit\"") != std::string::npos) break;
        }
        return 0;
    }
} // namespace sinterlsp
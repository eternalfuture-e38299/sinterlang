#include <fstream>
#include <sstream>
// sinterd: ops.cpp —— op
//
// format/reindent  cfront  formatting；cparse/diagnose  C parse。
// symbols  return  C  （Sinter   LSP processparse，
// sinterd not duplicate Sinter semantic，）。
#include "server.hpp"

#include "cfront/c_format.hpp"
#include "cfront/c_parser.hpp"
#include "cfront2/c2_parser.hpp"
#include "format/sinter_format.hpp"

#include <sstream>

namespace sinterd {
    namespace {
        ///   cfront  diagnosticcolumn JSON array
        std::string diagnostics_json(const std::vector<sinter::cfront::CParseError> &errs) {
            std::ostringstream o;
            o << "[";
            for (size_t i = 0; i < errs.size(); ++i) {
                if (i) o << ",";
                o << "{\"line\":" << errs[i].line << ",\"column\":" << errs[i].column
                  << ",\"message\":\"" << json_escape(errs[i].message) << "\"}";
            }
            o << "]";
            return o.str();
        }

        std::string string_array_json(const std::vector<std::string> &v) {
            std::ostringstream o;
            o << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) o << ",";
                o << "\"" << json_escape(v[i]) << "\"";
            }
            o << "]";
            return o.str();
        }

        /// parserequest  already  typedef（arrayfield "typedefs"）
        std::set<std::string> extra_typedefs(const Request &req) {
            std::set<std::string> s;
            if (const JsonField *f = req.find("typedefs")) {
                for (const auto &t: f->arr) s.insert(t);
            }
            return s;
        }

        std::string run_parse(const Request &req, const std::string &source) {
            const std::string path = req.str("path", "<memory>");
            auto r = sinter::cfront::CParser::parse_with_typedefs(source, extra_typedefs(req), path);

            std::ostringstream fns;
            fns << "[";
            const auto names = r.unit.function_names();
            for (size_t i = 0; i < names.size(); ++i) {
                if (i) fns << ",";
                fns << "\"" << json_escape(names[i]) << "\"";
            }
            fns << "]";

            std::ostringstream syms;
            syms << "[";
            bool first = true;
            for (const auto &item: r.unit.items) {
                if (item.kind != sinter::cfront::CUnitItem::Kind::FunctionDef) continue;
                if (!item.func) continue;
                if (!first) syms << ",";
                first = false;
                syms << "{\"kind\":\"function\",\"name\":\""
                     << json_escape(item.func->decl.name()) << "\",\"type\":\""
                     << json_escape(item.func->decl.spec.canonical()) << "\",\"line\":"
                     << item.decl.range.begin_line << "}";
            }
            syms << "]";

            std::ostringstream o;
            o << "\"functions\":" << fns.str()
              << ",\"tags\":" << string_array_json(r.unit.tag_names())
              << ",\"typedefs\":" << string_array_json(r.unit.typedef_names())
              << ",\"includes\":" << string_array_json(r.unit.includes())
              << ",\"items\":" << r.unit.items.size()
              << ",\"symbols\":" << syms.str()
              << ",\"diagnostics\":" << diagnostics_json(r.errors);
            return o.str();
        }
        std::string run_parse2(const Request &req) {
            const auto u = sinter::cfront2::parse(req.source());

            std::ostringstream fns;
            fns << "[";
            for (size_t i = 0; i < u.functions.size(); ++i) {
                const auto &f = u.functions[i];
                if (i) fns << ",";
                fns << "{\"name\":\"" << json_escape(f.name) << "\",\"return\":\""
                    << json_escape(f.return_type) << "\",\"params\":" << f.params.size()
                    << ",\"variadic\":" << (f.variadic ? "true" : "false")
                    << ",\"definition\":" << (f.definition ? "true" : "false")
                    << ",\"line\":" << f.line << "}";
            }
            fns << "]";

            std::ostringstream recs;
            recs << "[";
            for (size_t i = 0; i < u.records.size(); ++i) {
                const auto &r = u.records[i];
                if (i) recs << ",";
                recs << "{\"kind\":\"" << json_escape(r.kind) << "\",\"name\":\""
                     << json_escape(r.name) << "\",\"fields\":" << r.fields.size()
                     << ",\"variants\":" << r.variants.size() << "}";
            }
            recs << "]";

            std::ostringstream tds;
            tds << "[";
            for (size_t i = 0; i < u.typedefs.size(); ++i) {
                if (i) tds << ",";
                tds << "{\"name\":\"" << json_escape(u.typedefs[i].name) << "\",\"type\":\""
                    << json_escape(u.typedefs[i].type) << "\"}";
            }
            tds << "]";

            std::ostringstream o;
            o << "\"frontend\":\"cfront2\""
              << ",\"functions\":" << fns.str()
              << ",\"records\":" << recs.str()
              << ",\"typedefs\":" << tds.str()
              << ",\"includes\":" << string_array_json(u.includes)
              << ",\"diagnostics\":[";
            for (size_t i = 0; i < u.diagnostics.size(); ++i) {
                if (i) o << ",";
                o << "{\"line\":" << u.diagnostics[i].line << ",\"column\":"
                  << u.diagnostics[i].col << ",\"message\":\""
                  << json_escape(u.diagnostics[i].message) << "\"}";
            }
            o << "]";
            return o.str();
        }
    } // namespace

    std::string op_ping(const Request &req) {
        return resp_ok(json_id_of(req), "\"pong\":true,\"version\":\"" + std::string(kVersion) + "\"");
    }

    std::string op_format(const Request &req) {
        // .sinter 源码用 Sinter 格式化器；C 版本（cfront::format_c）只适合
        // embed C 正文，误用到 Sinter 会把 `=>` 拆成 `= >`、把类型注解断行。
        const std::string src = req.source();
        sinter::format::FormatOptions opt;
        opt.tab_size = static_cast<int32_t>(req.num("indent", 4));
        if (opt.tab_size < 1) opt.tab_size = 4;
        const std::string out = sinter::format::format_sinter(src, opt);
        return resp_string(json_id_of(req), "text", out);
    }

    std::string op_reindent(const Request &req) {
        // 只给 embed C { ... } 正文做缩进，C 版本最保守（不动 token 间距）。
        const std::string src = req.source();
        const int base = static_cast<int>(req.num("base_indent", 0));
        sinter::cfront::CFormatOptions opt;
        opt.indent_width = static_cast<int>(req.num("indent", 4));
        if (opt.indent_width < 1) opt.indent_width = 4;
        opt.use_tabs = req.flag("use_tabs", false);
        const std::string out = sinter::cfront::reindent_c(src, base, opt);
        return resp_string(json_id_of(req), "text", out);
    }

    std::string op_cparse(const Request &req) {
        const std::string src = req.source();
        return resp_ok(json_id_of(req), run_parse(req, src));
    }

    /// 第二个 C 前端（独立 lexer/parser）：同一份源码走 cfront2。
    std::string op_cparse2(const Request &req) {
        return resp_ok(json_id_of(req), run_parse2(req));
    }

    std::string op_symbols2(const Request &req) {
        return resp_ok(json_id_of(req), run_parse2(req));
    }

    std::string op_symbols(const Request &req) {
        //  and  cparse  ，semantic「 」；
        //  is  clientcode ，will  。
        const std::string src = req.source();
        return resp_ok(json_id_of(req), run_parse(req, src));
    }

    /// Parse a real C header **by path** (not inline source) and return the same
    /// symbol JSON as `cparse`. LSP/editors use it to ask "which prototypes and
    /// structs live in this header" without shipping the source over the wire.
    std::string op_cheader(const Request &req) {
        const std::string path = req.str("path", "");
        if (path.empty()) return resp_error(json_id_of(req), "missing 'path'");
        std::ifstream in(path, std::ios::binary);
        if (!in) return resp_error(json_id_of(req), "cannot read header: " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return resp_ok(json_id_of(req), run_parse(req, ss.str()));
    }

    std::string op_diagnose(const Request &req) {
        const std::string src = req.source();
        auto r = sinter::cfront::CParser::parse_with_typedefs(src, extra_typedefs(req),
                                                             req.str("path", "<memory>"));
        return resp_field(json_id_of(req), "diagnostics", diagnostics_json(r.errors));
    }
} // namespace sinterd
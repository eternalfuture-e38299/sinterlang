// sinterd: ops_tokens.cpp —— tokens op（C lexical ）
//
// ：  CLexer  resultcolumn JSON，a class category，
//  LSP  semanticnot 「this  kind  」。
#include "server.hpp"

#include "cfront/c_lexer.hpp"

#include <sstream>

namespace sinterd {
    namespace {
        /// token  class：LSP semantic（semanticTokens）this to  token type，
        /// not  client「this  kind  」 。
        const char *tok_category(const sinter::cfront::CTokKind k) {
            using K = sinter::cfront::CTokKind;
            switch (k) {
                case K::Identifier:
                    return "identifier";
                case K::IntLiteral:
                case K::FloatLiteral:
                case K::CharLiteral:
                    return "number";
                case K::StringLiteral:
                    return "string";
                case K::Comment:
                    return "comment";
                case K::PreprocLine:
                case K::Hash:
                case K::HashHash:
                    return "preproc";
                case K::EndOfFile:
                case K::Unknown:
                    return "other";
                default:
                    break;
            }
            // cannot c_tok_name  （return  "if"、";" classsource，
            // not enum ），——  「definition」 。
            size_t n = 0;
            const sinter::cfront::CKeyword *kws = sinter::cfront::c_keywords(n);
            for (size_t i = 0; i < n; ++i) {
                if (kws[i].kind == k) return "keyword";
            }
            return "operator";
        }
    } // namespace

    std::string op_tokens(const Request &req) {
        sinter::cfront::CLexer lx(req.source());
        // clientcancomment/not and line；LSP 
        lx.keep_comments(req.flag("comments", true));
        lx.merge_preproc(req.flag("merge_preproc", true));
        const auto r = lx.tokenize();

        std::ostringstream o;
        o << "[";
        bool first = true;
        for (const auto &t: r.tokens) {
            if (t.kind == sinter::cfront::CTokKind::EndOfFile) continue;
            if (!first) o << ",";
            first = false;
            o << "{\"kind\":\"" << tok_category(t.kind)
              << "\",\"type\":\"" << sinter::cfront::c_tok_name(t.kind)
              << "\",\"text\":\"" << json_escape(t.text)
              << "\",\"offset\":" << t.offset
              << ",\"length\":" << t.text.size()
              << ",\"line\":" << t.line
              << ",\"column\":" << t.column;
            if (!t.directive.empty())
                o << ",\"directive\":\"" << json_escape(t.directive) << "\"";
            o << "}";
        }
        o << "]";

        std::ostringstream errs;
        errs << "[";
        for (size_t i = 0; i < r.errors.size(); ++i) {
            if (i) errs << ",";
            errs << "{\"line\":" << r.errors[i].line << ",\"column\":" << r.errors[i].column
                 << ",\"message\":\"" << json_escape(r.errors[i].message) << "\"}";
        }
        errs << "]";

        return resp_ok(json_id_of(req), "\"tokens\":" + o.str() + ",\"errors\":" + errs.str());
    }
} // namespace sinterd
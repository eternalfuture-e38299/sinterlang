// sinterd: protocol.hpp —— request/responseprotocol
//
// ：Unix domain socket，is line JSON（line）。
//  JSON-over-newline not  length ， is  socat/nc ，
// and   responsenot （    format ， range）。
//
// request：{"id":"<>","op":"<op>", ...args}
// response：{"id":"<request>","ok":true, ...result}   or   {"id":...,"ok":false,"error":"..."}
//
// op（sinterd ：formatting +  LSP  ）：
//   ping                     -> {"pong":true,"version":"..."}
//   format                   -> formatting C  or  Sinter source
//   reindent                 -> indent（，not ）
//   cparse                   -> parse C，return  （ LSP language）
//   symbols                  -> parse Sinter source，return module/function/class 
//   diagnose                 -> parsesourceand return diagnostic
//   shutdown                 -> closeprocess
#pragma once
#include <string>
#include <vector>

#include <sinter/version.hpp>

namespace sinterd {
    /// 版本号来自 CMake 的 project(VERSION)，四个工具共用同一份。
    constexpr const char *kVersion = SINTER_VERSION_STRING;
    constexpr const char *kDefaultSocket = "/tmp/sinterd.sock";

    ///  JSON parseartifact：supportprotocol to  
    /// （ 、string、bool、number、array），and no dependency。
    struct JsonValue;

    struct JsonField {
        std::string key;
        std::string str_value;
        bool is_string = true;
        bool bool_value = false;
        double num_value = 0;
        std::vector<std::string> arr;   // stringarraywill  
    };

    /// a request：id + op + string/bool/numberargs
    struct Request {
        std::string id;
        /// id  not   JSON string（response   ）
        bool id_is_string = true;
        std::string op;
        std::vector<JsonField> fields;

        const JsonField *find(const std::string &k) const {
            for (const auto &f: fields)
                if (f.key == k) return &f;
            return nullptr;
        }
        std::string str(const std::string &k, const std::string &dflt = "") const {
            const JsonField *f = find(k);
            return (f && f->is_string) ? f->str_value : dflt;
        }
        /// sourcetext：  "text"  and  "source" field。
        /// protocoldocument   text， socat/nc    source；
        ///  ，callis fieldnot  to （will failed）。
        std::string source() const {
            std::string s = str("text");
            if (s.empty()) s = str("source");
            return s;
        }
        bool flag(const std::string &k, const bool dflt = false) const {
            const JsonField *f = find(k);
            if (!f) return dflt;
            return f->is_string ? f->bool_value : false;
        }
        double num(const std::string &k, const double dflt = 0) const {
            const JsonField *f = find(k);
            return f && !f->is_string ? f->num_value : dflt;
        }
    };

    ///  linetextparse Request；failed return  false and  error
    bool parse_request(const std::string &line, Request &out, std::string &error);

    ///  JSON string（ ）
    std::string json_escape(const std::string &s);

    ///  numberformatting JSON numbertext（integernot ）
    std::string json_number_text(double v);

    ///  request id  JSON ：string ，number（not  ）。
    ///  id stringwill 「 number」info，clientcannotresponse。
    std::string json_id_of(const Request &req);

    /// response。id args 「already   JSON 」，please  json_id_of(req) 。
    std::string resp_ok(const std::string &id_json, const std::string &extra_fields = "");
    std::string resp_error(const std::string &id_json, const std::string &error);
    std::string resp_string(const std::string &id_json, const std::string &key,
                            const std::string &value);
    std::string resp_field(const std::string &id_json, const std::string &key,
                           const std::string &json_value);
} // namespace sinterd
// sinterd: protocol.cpp ——  JSON parse and response
//
// parseprotocolto  ： 、string/bool/number、stringarray。
// not ： JSON requires and ，protocol no 。
#include "protocol.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace sinterd {
    namespace {
        struct Parser {
            const std::string &s;
            size_t i = 0;
            std::string err;

            explicit Parser(const std::string &src) : s(src) {}

            void skip_ws() {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
                    ++i;
            }

            bool eof() const { return i >= s.size(); }
            char cur() const { return eof() ? '\0' : s[i]; }

            bool expect(const char c) {
                skip_ws();
                if (cur() != c) {
                    err = std::string("expected '") + c + "'";
                    return false;
                }
                ++i;
                return true;
            }

            bool parse_string(std::string &out) {
                skip_ws();
                if (cur() != '"') {
                    err = "expected string";
                    return false;
                }
                ++i;
                out.clear();
                while (!eof() && cur() != '"') {
                    char c = s[i++];
                    if (c == '\\' && !eof()) {
                        const char e = s[i++];
                        switch (e) {
                            case 'n': out += '\n'; break;
                            case 't': out += '\t'; break;
                            case 'r': out += '\r'; break;
                            case '"': out += '"'; break;
                            case '\\': out += '\\'; break;
                            case '/': out += '/'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case 'u': {
                                // \uXXXX： BMP，output UTF-8
                                if (i + 4 > s.size()) {
                                    err = "\\u incomplete escape";
                                    return false;
                                }
                                unsigned cp = 0;
                                for (int k = 0; k < 4; ++k) {
                                    const char h = s[i++];
                                    cp <<= 4;
                                    if (h >= '0' && h <= '9') cp |= (unsigned) (h - '0');
                                    else if (h >= 'a' && h <= 'f') cp |= (unsigned) (h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F') cp |= (unsigned) (h - 'A' + 10);
                                    else {
                                        err = "\\u invalid escape";
                                        return false;
                                    }
                                }
                                if (cp < 0x80) {
                                    out += static_cast<char>(cp);
                                } else if (cp < 0x800) {
                                    out += static_cast<char>(0xC0 | (cp >> 6));
                                    out += static_cast<char>(0x80 | (cp & 0x3F));
                                } else {
                                    out += static_cast<char>(0xE0 | (cp >> 12));
                                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                    out += static_cast<char>(0x80 | (cp & 0x3F));
                                }
                                break;
                            }
                            default: out += e; break;
                        }
                    } else {
                        out += c;
                    }
                }
                if (eof()) {
                    err = "unterminated string";
                    return false;
                }
                ++i;   //  
                return true;
            }

            bool parse_value(JsonField &f) {
                skip_ws();
                if (cur() == '"') {
                    f.is_string = true;
                    return parse_string(f.str_value);
                }
                if (cur() == 't' || cur() == 'f') {
                    const bool is_true = (cur() == 't');
                    const std::string lit = is_true ? "true" : "false";
                    if (s.compare(i, lit.size(), lit) != 0) {
                        err = "invalid literal";
                        return false;
                    }
                    i += lit.size();
                    f.is_string = true;
                    f.bool_value = is_true;
                    f.str_value = is_true ? "true" : "false";
                    return true;
                }
                if (cur() == '[') {
                    ++i;
                    skip_ws();
                    if (cur() == ']') {
                        ++i;
                        f.is_string = true;
                        return true;
                    }
                    while (true) {
                        std::string v;
                        if (!parse_string(v)) return false;
                        f.arr.push_back(v);
                        skip_ws();
                        if (cur() == ',') {
                            ++i;
                            continue;
                        }
                        if (cur() == ']') {
                            ++i;
                            break;
                        }
                        err = "expected ',' or ']' in array";
                        return false;
                    }
                    f.is_string = true;
                    return true;
                }
                // number
                skip_ws();
                const size_t start = i;
                if (cur() == '-' || cur() == '+') ++i;
                while (!eof() && (std::isdigit(static_cast<unsigned char>(cur())) || cur() == '.' ||
                                  cur() == 'e' || cur() == 'E' || cur() == '-' || cur() == '+'))
                    ++i;
                if (i == start) {
                    err = "expected value";
                    return false;
                }
                f.is_string = false;
                f.num_value = std::strtod(s.substr(start, i - start).c_str(), nullptr);
                return true;
            }
        };
    } // namespace

    bool parse_request(const std::string &line, Request &out, std::string &error) {
        Parser p(line);
        if (!p.expect('{')) {
            error = "request must be a JSON object: " + p.err;
            return false;
        }
        p.skip_ws();
        if (p.cur() == '}') return true;
        while (true) {
            JsonField f;
            if (!p.parse_string(f.key)) {
                error = "field name parse failed: " + p.err;
                return false;
            }
            if (!p.expect(':')) {
                error = "expected ':': " + p.err;
                return false;
            }
            if (!p.parse_value(f)) {
                error = "field '" + f.key + "' parse failed: " + p.err;
                return false;
            }
            out.fields.push_back(std::move(f));
            p.skip_ws();
            if (p.cur() == ',') {
                ++p.i;
                continue;
            }
            if (p.cur() == '}') break;
            error = "expected ',' or '}'";
            return false;
        }
        // id / op 。
        // note id  number（`{"id":1}`）：JSON   number parse is_string=false，
        // str_value 。 str_value，response   id  ，client 
        // cannot response and request ——sonumber id formattingtext，
        // and and response number（not  ）。
        for (const auto &f: out.fields) {
            if (f.key == "id") {
                out.id = f.is_string ? f.str_value : json_number_text(f.num_value);
                out.id_is_string = f.is_string;
            }
            if (f.key == "op") out.op = f.str_value;
        }
        if (out.op.empty()) {
            error = "missing 'op' field";
            return false;
        }
        return true;
    }

    std::string json_escape(const std::string &s) {
        std::string r;
        r.reserve(s.size() + 8);
        for (const char c: s) {
            switch (c) {
                case '"': r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                case '\b': r += "\\b"; break;
                case '\f': r += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        //  \u00XX
                        const char *hex = "0123456789abcdef";
                        r += "\\u00";
                        r += hex[(c >> 4) & 0xF];
                        r += hex[c & 0xF];
                    } else {
                        r += c;
                    }
                    break;
            }
        }
        return r;
    }

    std::string json_number_text(const double v) {
        // integerand not integer， "1.000000"
        if (v == static_cast<double>(static_cast<long long>(v))) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            return buf;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        return buf;
    }

    std::string json_id_of(const Request &req) {
        if (req.id_is_string) return "\"" + json_escape(req.id) + "\"";
        return req.id;   // number id：output，not  
    }

    std::string resp_ok(const std::string &id_json, const std::string &extra_fields) {
        std::string r = "{\"id\":" + id_json + ",\"ok\":true";
        if (!extra_fields.empty()) r += "," + extra_fields;
        r += "}";
        return r;
    }

    std::string resp_error(const std::string &id_json, const std::string &error) {
        return "{\"id\":" + id_json + ",\"ok\":false,\"error\":\"" + json_escape(error) + "\"}";
    }

    std::string resp_string(const std::string &id_json, const std::string &key,
                            const std::string &value) {
        return resp_ok(id_json, "\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"");
    }

    std::string resp_field(const std::string &id, const std::string &key, const std::string &json_value) {
        return resp_ok(id, "\"" + json_escape(key) + "\":" + json_value);
    }
} // namespace sinterd
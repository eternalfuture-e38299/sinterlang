// sinter-lsp: json.cpp —— JSON parse and column
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sinterlsp {
    std::string json_escape(const std::string &s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c: s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        return out;
    }

    std::string json_number(const double d) {
        if (std::isfinite(d) && d == static_cast<double>(static_cast<long long>(d)) &&
            std::fabs(d) < 1e15) {
            return std::to_string(static_cast<long long>(d));
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        return buf;
    }

    namespace {
        void dump_into(const Json &j, std::string &out) {
            switch (j.kind()) {
                case Json::Kind::Null: out += "null"; return;
                case Json::Kind::Bool: out += j.as_bool() ? "true" : "false"; return;
                case Json::Kind::Number: out += json_number(j.as_number()); return;
                case Json::Kind::String:
                    out += '"';
                    out += json_escape(j.as_string());
                    out += '"';
                    return;
                case Json::Kind::Array: {
                    out += '[';
                    bool first = true;
                    for (const auto &e: j.as_array()) {
                        if (!first) out += ',';
                        first = false;
                        dump_into(e, out);
                    }
                    out += ']';
                    return;
                }
                case Json::Kind::Object: {
                    out += '{';
                    bool first = true;
                    for (const auto &kv: j.as_object()) {
                        if (!first) out += ',';
                        first = false;
                        out += '"';
                        out += json_escape(kv.first);
                        out += "\":";
                        dump_into(kv.second, out);
                    }
                    out += '}';
                    return;
                }
            }
        }

        struct Parser {
            const std::string &s;
            size_t i = 0;
            std::string err;

            explicit Parser(const std::string &text) : s(text) {}

            void skip_ws() {
                while (i < s.size() &&
                       (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                    ++i;
            }

            bool parse_value(Json &out);

            bool parse_string(std::string &out) {
                if (i >= s.size() || s[i] != '"') {
                    err = "expected string";
                    return false;
                }
                ++i;
                out.clear();
                while (i < s.size()) {
                    const char c = s[i++];
                    if (c == '"') return true;
                    if (c != '\\') {
                        out.push_back(c);
                        continue;
                    }
                    if (i >= s.size()) break;
                    const char e = s[i++];
                    switch (e) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            // \uXXXX -> UTF-8（not  ，LSP  ）
                            if (i + 4 > s.size()) {
                                err = "\\u incomplete escape";
                                return false;
                            }
                            unsigned cp = 0;
                            for (int k = 0; k < 4; ++k) {
                                const char h = s[i + k];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                                else {
                                    err = "\\u invalid";
                                    return false;
                                }
                            }
                            i += 4;
                            if (cp < 0x80) {
                                out.push_back(static_cast<char>(cp));
                            } else if (cp < 0x800) {
                                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            break;
                        }
                        default: err = "invalid"; return false;
                    }
                }
                err = "unterminated string";
                return false;
            }

            bool parse_number(Json &out) {
                const size_t start = i;
                if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
                while (i < s.size() &&
                       ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                        s[i] == '+' || s[i] == '-'))
                    ++i;
                if (start == i) {
                    err = "expectednumber";
                    return false;
                }
                out = Json(std::strtod(s.substr(start, i - start).c_str(), nullptr));
                return true;
            }
        };

        bool Parser::parse_value(Json &out) {
            skip_ws();
            if (i >= s.size()) {
                err = " inputend";
                return false;
            }
            const char c = s[i];
            if (c == '{') {
                ++i;
                out = Json::object();
                skip_ws();
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    return true;
                }
                while (true) {
                    skip_ws();
                    std::string key;
                    if (!parse_string(key)) return false;
                    skip_ws();
                    if (i >= s.size() || s[i] != ':') {
                        err = "expected ':'";
                        return false;
                    }
                    ++i;
                    Json v;
                    if (!parse_value(v)) return false;
                    out.set(std::move(key), std::move(v));
                    skip_ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}') {
                        ++i;
                        return true;
                    }
                    err = "expected ','  or  '}'";
                    return false;
                }
            }
            if (c == '[') {
                ++i;
                out = Json::array();
                skip_ws();
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    return true;
                }
                while (true) {
                    Json v;
                    if (!parse_value(v)) return false;
                    out.push(std::move(v));
                    skip_ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']') {
                        ++i;
                        return true;
                    }
                    err = "expected ','  or  ']'";
                    return false;
                }
            }
            if (c == '"') {
                std::string str;
                if (!parse_string(str)) return false;
                out = Json(std::move(str));
                return true;
            }
            if (s.compare(i, 4, "true") == 0) {
                i += 4;
                out = Json(true);
                return true;
            }
            if (s.compare(i, 5, "false") == 0) {
                i += 5;
                out = Json(false);
                return true;
            }
            if (s.compare(i, 4, "null") == 0) {
                i += 4;
                out = Json();
                return true;
            }
            return parse_number(out);
        }
    } // namespace

    std::string Json::dump() const {
        std::string out;
        dump_into(*this, out);
        return out;
    }

    Json Json::parse(const std::string &text, std::string &error) {
        Parser p(text);
        Json out;
        if (!p.parse_value(out)) {
            error = p.err.empty() ? "JSON parsefailed" : p.err;
            return Json();
        }
        p.skip_ws();
        if (p.i != text.size()) {
            error = "JSON extracontent";
            return Json();
        }
        return out;
    }
} // namespace sinterlsp
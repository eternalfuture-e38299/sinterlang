// sinter-lsp: transport.cpp —— 
#include "transport.hpp"

#include <cstdlib>
#include <cstring>

namespace sinterlsp {
    namespace {
        /// line（ \n ）， \r\n；EOF and no content return  false
        bool read_line(std::istream &in, std::string &line) {
            line.clear();
            char c = 0;
            while (in.get(c)) {
                if (c == '\n') return true;
                if (c != '\r') line.push_back(c);
            }
            return !line.empty();
        }
    } // namespace

    bool read_message(std::istream &in, std::string &body) {
        size_t content_length = 0;
        bool got_length = false;

        std::string line;
        // field：to lineis 
        while (read_line(in, line)) {
            if (line.empty()) {
                if (!got_length) {
                    // lineto  Content-Length：
                    continue;
                }
                break;
            }
            const std::string key = "Content-Length:";
            if (line.rfind(key, 0) == 0) {
                content_length = std::strtoul(line.c_str() + key.size(), nullptr, 10);
                got_length = true;
            }
            // Content-Type etcits 
        }
        if (!got_length) return false;

        body.assign(content_length, '\0');
        if (content_length > 0) in.read(&body[0], static_cast<std::streamsize>(content_length));
        return in.gcount() == static_cast<std::streamsize>(content_length);
    }

    void write_message(std::ostream &out, const std::string &body) {
        out << "Content-Length: " << body.size() << "\r\n\r\n";
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
    }
} // namespace sinterlsp
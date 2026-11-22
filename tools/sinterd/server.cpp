// sinterd: server.cpp —— socket 、request
//
// thread + ： request （parsesource），
//  LSP clientwill  request。 ：
//   1) no ，no 
//   2)  
// will requiresand ，each  a threadi.e. ，not protocol。
#include "server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace sinterd {
    namespace {
        /// from  socket line（ \n ），return   out not  \n。
        ///
        /// must： recv request，
        ///  to a  \n   return，     
        /// ——clientwill etcnot to response，and   「」， 。
        struct LineReader {
            std::string pending;   ///< already  

            bool next(const int fd, std::string &out, const size_t max_bytes) {
                out.clear();
                while (true) {
                    const size_t nl = pending.find('\n');
                    if (nl != std::string::npos) {
                        out.assign(pending, 0, nl);
                        pending.erase(0, nl + 1);
                        //  CRLF client
                        if (!out.empty() && out.back() == '\r') out.pop_back();
                        if (out.size() > max_bytes) {
                            out.clear();
                            continue;   // line，
                        }
                        return true;
                    }
                    if (pending.size() > max_bytes) {
                        pending.clear();
                        return false;
                    }
                    char buf[4096];
                    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        return false;
                    }
                    if (n == 0) {
                        //  close：  no linecontent  
                        if (pending.empty()) return false;
                        out.swap(pending);
                        return true;
                    }
                    pending.append(buf, static_cast<size_t>(n));
                }
            }
        };

        bool write_all(const int fd, const std::string &s) {
            size_t off = 0;
            while (off < s.size()) {
                const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                off += static_cast<size_t>(n);
            }
            return true;
        }
    } // namespace

    std::string handle_request_line(const std::string &line) {
        if (line.empty()) return resp_error("null", "empty request");

        Request req;
        std::string err;
        if (!parse_request(line, req, err)) return resp_error("null", err);

        const std::string &op = req.op;
        if (op == "ping") return op_ping(req);
        if (op == "format") return op_format(req);
        if (op == "reindent") return op_reindent(req);
        if (op == "cparse") return op_cparse(req);
        if (op == "cparse2") return op_cparse2(req);
        if (op == "symbols") return op_symbols(req);
        if (op == "symbols2") return op_symbols2(req);
        if (op == "diagnose") return op_diagnose(req);
        if (op == "cheader") return op_cheader(req);
        if (op == "tokens") return op_tokens(req);
        if (op == "shutdown") return resp_ok(json_id_of(req), "\"bye\":true");
        return resp_error(json_id_of(req), "unknown op: " + op);
    }

    int run_server(const ServerConfig &cfg) {
        //  socket
        struct stat st {};
        if (::stat(cfg.socket_path.c_str(), &st) == 0) {
            if (S_ISSOCK(st.st_mode)) ::unlink(cfg.socket_path.c_str());
            else {
                std::cerr << "sinterd: " << cfg.socket_path << " already existsand not a socket\n";
                return 1;
            }
        }

        const int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (srv < 0) {
            std::cerr << "sinterd: socket() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (cfg.socket_path.size() >= sizeof(addr.sun_path)) {
            std::cerr << "sinterd: socket pathtoo long\n";
            ::close(srv);
            return 1;
        }
        std::strncpy(addr.sun_path, cfg.socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            std::cerr << "sinterd: bind failed: " << std::strerror(errno) << "\n";
            ::close(srv);
            return 1;
        }
        //  its process （LSP clientnot line）
        ::chmod(cfg.socket_path.c_str(), 0600);
        if (::listen(srv, 16) < 0) {
            std::cerr << "sinterd: listen failed: " << std::strerror(errno) << "\n";
            ::close(srv);
            return 1;
        }

        if (cfg.verbose)
            std::cerr << "sinterd " << kVersion << " ready: " << cfg.socket_path << "\n";

        std::atomic<bool> stopping{false};
        while (!stopping.load()) {
            const int fd = ::accept(srv, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                break;
            }
            // a canrequest（LSP clientwill ）。
            // LineReader ， recv request   。
            std::string line;
            LineReader reader;
            while (reader.next(fd, line, cfg.max_line_bytes)) {
                if (line.empty()) continue;
                if (cfg.verbose) std::cerr << "[req] " << line.substr(0, 200) << "\n";
                //  op fieldexit，not   line  "shutdown" ：
                //  will  source  shutdown  exit。
                Request probe;
                std::string perr;
                const bool is_shutdown = parse_request(line, probe, perr) && probe.op == "shutdown";
                const std::string resp = handle_request_line(line);
                if (!write_all(fd, resp + "\n")) break;
                if (is_shutdown) {
                    stopping = true;
                    break;
                }
            }
            ::close(fd);
        }

        ::close(srv);
        ::unlink(cfg.socket_path.c_str());
        return 0;
    }
} // namespace sinterd
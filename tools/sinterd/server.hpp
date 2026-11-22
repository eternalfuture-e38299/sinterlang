// sinterd: server.hpp —— process
#pragma once
#include "protocol.hpp"

#include <atomic>
#include <string>

namespace sinterd {
    struct ServerConfig {
        std::string socket_path = kDefaultSocket;
        /// idle exit（）；0 = not  。default 0（stay resident）。
        int idle_timeout_sec = 0;
        bool verbose = false;
        /// request  ，linememory
        size_t max_line_bytes = 8u * 1024u * 1024u;
    };

    /// lineprocess。return processexit。
    int run_server(const ServerConfig &cfg);

    /// linerequest（test，not dependency socket）
    std::string handle_request_line(const std::string &line);

    /// op （  ops.cpp）
    std::string op_ping(const Request &req);
    std::string op_format(const Request &req);
    std::string op_reindent(const Request &req);
    std::string op_cparse(const Request &req);
    std::string op_cparse2(const Request &req);
    std::string op_symbols(const Request &req);
    std::string op_symbols2(const Request &req);
    std::string op_diagnose(const Request &req);
    std::string op_cheader(const Request &req);
    /// tokens： C lexicalresult（kind/text/offset/line/column）， LSP semantic and 
    std::string op_tokens(const Request &req);
} // namespace sinterd
// sinter-lsp: server.hpp —— LSP service
#pragma once
#include <map>
#include <string>
#include <vector>

#include "document.hpp"
#include "index.hpp"
#include "json.hpp"

namespace sinterlsp {
    ///  （  features  generate， server ）
    struct Outgoing {
        std::string body;
    };

    class Server {
    public:
        Server();

        /// loop：from  stdin to  shutdown/exit。return processexit。
        int run();

        /// test：，return  column
        std::vector<std::string> handle_message(const std::string &body);

        /// test： / readdocument
        void set_document(const std::string &uri, std::string text,
                          const std::string &lang = "sinter", int version = 1);
        [[nodiscard]] const Document *document(const std::string &uri) const;
        /// （and cache）
        [[nodiscard]] const Index &index_of(const std::string &uri);

    private:
        // ----  ----
        Json on_initialize(const Json &params);
        void on_initialized();
        // ---- document ----
        void on_did_open(const Json &params, std::vector<std::string> &out);
        void on_did_change(const Json &params, std::vector<std::string> &out);
        void on_did_close(const Json &params, std::vector<std::string> &out);
        void on_did_save(const Json &params, std::vector<std::string> &out);

        // ---- （ features.cpp）----
        Json on_completion(const std::string &uri, const Position pos);
        Json on_definition(const std::string &uri, const Position pos);
        Json on_hover(const std::string &uri, const Position pos);
        Json on_document_symbol(const std::string &uri);
        /// 整文件格式化（format.cpp）：返回 TextEdit[]，无变化时是空数组
        Json on_format(const std::string &uri);
        /// 范围格式化：当前实现等价于整文件（range 仅用于标记「被请求过」）
        Json on_range_format(const std::string &uri, Range range);
        /// generatediagnostic（publishDiagnostics）
        std::string diagnostics_message(const std::string &uri);

        // ---- protocol ----
        void send(const std::string &body) const;
        static Json make_response(const Json &id, Json result);
        static Json make_error(const Json &id, const int code, const std::string &message);

        std::map<std::string, Document> docs_;
        std::map<std::string, Index> index_cache_;
        bool shutdown_requested_ = false;
        bool initialized_ = false;
        bool verbose_ = false;

        friend struct FeatureAccess;
    };

    /// sinterd  client（）：embed C  。
    /// not  socket  callreturn ，is 「not  C 」。
    struct BackendClient {
        std::string socket_path;
        bool available() const { return !socket_path.empty(); }
    };

    BackendClient &backend();
} // namespace sinterlsp
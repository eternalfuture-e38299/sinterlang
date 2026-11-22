// sinter-lsp: transport.hpp —— LSP base protocol（Content-Length ）
//
// LSP over stdio  ：
//   Content-Length: <>\r\n
//   \r\n
//   <JSON >
//  Content-Type ，。
#pragma once
#include <istream>
#include <ostream>
#include <string>

namespace sinterlsp {
    /// from input；EOF  return  false
    bool read_message(std::istream &in, std::string &body);

    /// output（ Content-Length and  flush）
    void write_message(std::ostream &out, const std::string &body);
} // namespace sinterlsp
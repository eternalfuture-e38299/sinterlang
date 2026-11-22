#pragma once

#include "c2_ast.hpp"

#include <string>
#include <vector>

namespace sinter::cfront2 {
    [[nodiscard]] std::vector<Token> lex(const std::string &source);
    [[nodiscard]] bool is_keyword(const std::string &s);
}

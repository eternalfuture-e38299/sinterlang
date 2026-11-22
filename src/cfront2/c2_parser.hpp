#pragma once

#include "c2_ast.hpp"

#include <string>

namespace sinter::cfront2 {
    [[nodiscard]] Unit parse(const std::string &source);
}

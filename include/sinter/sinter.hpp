// Sinter 编译器公共头：一次性引入全部对外组件
#pragma once

#include "front/ast.hpp"
#include "front/lexer.hpp"
#include "front/parser.hpp"
#include "backend/codegen.hpp"
#include "sem/semantic.hpp"
#include "support/diagnostics.hpp"
#include "support/token.hpp"

/// Sinter 编译器版本
#define SINTER_VERSION_MAJOR 0
#define SINTER_VERSION_MINOR 2
#define SINTER_VERSION_PATCH 0
#define SINTER_VERSION_STRING "0.2.0"
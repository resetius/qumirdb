#pragma once

#include "ast.h"

#include <string>

namespace NQdb {
namespace NSql {

// Renders a parsed SQL AST as an indented S-expression tree. SQL nodes are
// printed here; embedded expressions are delegated to the Qumir core printer.
// The output is intended for regression goldens, not for re-parsing.
std::string PrintAst(const TSqlNodePtr& node);

} // namespace NSql
} // namespace NQdb

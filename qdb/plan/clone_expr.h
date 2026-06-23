#pragma once

#include <qumir/parser/ast.h>

namespace NQdb {

// Deep-clones an expression tree (every node freshly allocated). Passes such as
// QualifyColumns mutate expression nodes in place, so a subtree referenced more
// than once (e.g. a CTE inlined several times) must be cloned to keep the copies
// independent. Throws NQumir::TError on an unsupported node type.
NQumir::NAst::TExprPtr CloneExpr(const NQumir::NAst::TExprPtr& expr);

} // namespace NQdb

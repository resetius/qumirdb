#pragma once

#include <qumir/parser/ast.h>

#include <string>
#include <unordered_set>

namespace NQqb {

// Returns the set of free (unbound) variable names in expr.
// Names introduced by let-bindings, fun params, var declarations, and for-loop
// variables are considered bound and excluded from the result.
// Unbound names in a kernel expression correspond to input column references.
std::unordered_set<std::string> FindUnboundVars(const NQumir::NAst::TExprPtr& expr);

} // namespace NQqb

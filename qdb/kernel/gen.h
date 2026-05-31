#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <unordered_set>

namespace NQqb {
namespace NKernel {

// Replaces TIdentExpr nodes that match field names with TIndexExpr{ident, i}.
void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const NQumir::NAst::TExprPtr& indexIdent);

// Builds a complete vectorized filter kernel AST from an unannotated predicate.
// Returns: unannotated (block (fun <kernel> void (...) () (block loop...)))
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType);

} // namespace NKernel
} // namespace NQqb

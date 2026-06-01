#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <unordered_map>
#include <unordered_set>

namespace NQqb {
namespace NKernel {

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const NQumir::NAst::TExprPtr& indexIdent);

// Builds a vectorized filter kernel that takes (ref TRowSet) directly.
// fieldIndices maps each field name to its index in TRowSet.Columns.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType);

} // namespace NKernel
} // namespace NQqb

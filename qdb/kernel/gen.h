#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <optional>
#include <string>
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

// Builds an op-dispatched aggregation update kernel:
//   agg_dispatch(ref HashTable ht, ref TRowSet batch, i64 arg, i64 op) -> i64
// op == 0: count_init(ht, arg)              (init, capacity = arg)
// op == 1: for each selected row in batch, count_update(ht, key, value)
//          (value = the argField column, or constant 0 if argField is unset)
// otherwise: count_destroy(ht)
// keyField and argField (if set) must name i64 columns (Stage 1: integer
// keys/args only); fieldIndices maps their names to TRowSet.Columns indices.
// The result calls count_init/count_update/count_destroy, so it must be
// merged with count.oz's FunDecls (see MergeKernelLibrary) before compiling.
NQumir::NAst::TExprPtr GenAggregateKernelAst(
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    const std::string& keyField,
    const std::optional<std::string>& argField,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType);

} // namespace NKernel
} // namespace NQqb

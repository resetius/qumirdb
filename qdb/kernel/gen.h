#pragma once

// Generates core-lang kernel source for filter and project operators.

#include <qumir/parser/type.h>

#include <string>
#include <unordered_set>

namespace NQqb {
namespace NKernel {

// Returns a <ptr T> type string for use in kernel parameters.
std::string PtrTypeToCoreStr(const NQumir::NAst::TTypePtr& type);

// Substitutes bare field-name tokens in an expression with (index i fieldname).
// Used to convert scalar predicates to vectorized form.
std::string SubstFieldsWithIndex(
    const std::string& expr,
    const std::unordered_set<std::string>& fieldNames);

// Generates a complete core-lang filter kernel source with a single context ABI:
//   (fun <kernel> void ((var ctx <ref <struct (n i64) (selection <ptr bool>) ...>>)) ()
//     (block
//       (var n i64) (= n (field n ctx))
//       (var selection <ptr bool>) (= selection (field selection ctx))
//       ...
//       (while (< i n) (block (= selection [i] PRED) ...))))
// where PRED is predicate with field refs substituted by (index i fieldname).
std::string GenFilterKernelSource(
    const NQumir::NAst::TStructType& inputType,
    const std::string& predicate);

} // namespace NKernel
} // namespace NQqb

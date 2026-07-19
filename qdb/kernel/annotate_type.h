#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

namespace NQdb::NKernel {

// Infers the result type of a Project/aggregate expression over `inputType` by
// running the real qumir annotator: the expression is wrapped in a function whose
// parameters are the input columns, composed with qumirdb.oz, then name-resolved
// and type-annotated. External-function and operator (incl. Nullable[T]) return
// types therefore come from their declarations, not a hand-maintained table.
//
// Source-module aliases are mapped back to planner types: Nullable[T] stays
// nullable, and StringView becomes a string type.
// Throws NQumir::TError for expressions the annotator cannot type.
NQumir::NAst::TTypePtr AnnotateExprType(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

// Coerces the branches of a CASE/`if` with a bare NULL (or nullable/non-nullable
// mismatch) to a common Nullable[T]: T is inferred from the non-null branch via
// AnnotateExprType, branches are wrapped with qumirdb.oz's nullable_from_value /
// nullable_from_null casts. Post-order and per-`if`, so nesting needs no handling.
// No-op without a NULL/`if`. `inputType` describes the referenced columns.
NQumir::NAst::TExprPtr NormalizeNullBranches(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

} // namespace NQdb::NKernel

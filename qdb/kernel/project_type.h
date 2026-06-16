#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

namespace NQqb::NKernel {

// Infers the result type of a Project expression over `inputType` (a standalone
// type-inference pass, independent of kernel lowering). Stage 1 supports column
// references, numeric/string literals, casts, unary, and binary arithmetic /
// comparison / logical operators; it drops nullability (returns the unwrapped
// non-null type). Throws NQumir::TError for unsupported expressions.
//
// Arithmetic promotion is conservative ("any f64 operand -> f64"): combined with
// the explicit `cast(expr, inferredType)` the project kernel emits, the buffer
// type never under-widens the expression's natural type.
NQumir::NAst::TTypePtr InferProjectExprType(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

} // namespace NQqb::NKernel

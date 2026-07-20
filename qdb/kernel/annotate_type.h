#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

namespace NQdb::NKernel {

// Lightweight, rule-based SQL type inference for a Project/filter/aggregate expression
// over `inputType`. No AST rewrite and no per-node qumirdb.oz compose: operators use
// promotion rules, extern-call returns come from the resolver, and NULL propagates (a
// nullable operand makes the result Nullable[T]) — i.e. it types the expression as if
// ExpandNullable had already run. Used by the logical typing pass, which must keep the
// AST clean (`a==b`, `a+b`) for equi-join extraction. Throws on a null expression.
NQumir::NAst::TTypePtr AnnotateExprType(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

// Heavy rewrite, run once per kernel just before compilation (never during logical
// planning). Rewrites null-strict ops/calls/casts, AND/OR (SQL 3VL) and CASE/`if`
// branches so nullability is explicit in the AST (guards on `.Valid`, results wrapped
// Nullable[T]); the non-nullable path is untouched. Operates on a clone — the plan's
// shared expression is left intact. Returns the rewritten expression and its planner type.
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandNullable(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

// Final expression rewrite for kernel compilation. Applies the existing nullable
// normalization and then qdb-only decimal erasure (Decimal -> qumirdb.oz BinInt).
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandKernelExpr(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

} // namespace NQdb::NKernel

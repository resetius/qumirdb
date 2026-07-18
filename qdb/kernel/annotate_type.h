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
// De-risk boundary: the result is unwrapped from Nullable (and StringView is mapped
// to a string type) so the planner/kernel behaviour is unchanged for now.
// Throws NQumir::TError for expressions the annotator cannot type.
NQumir::NAst::TTypePtr AnnotateExprType(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);

} // namespace NQdb::NKernel

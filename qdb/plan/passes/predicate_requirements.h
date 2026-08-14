#pragma once

#include <qumir/parser/ast.h>

#include <vector>

namespace NQdb {

// Builds a row-superset needed by any alternative consumer. Conjuncts are
// grouped by the columns they depend on; a group is retained only when every
// consumer constrains it. With one alternative this preserves every conjunct
// over the supplied output schema and is used for row-group hints.
NQumir::NAst::TExprPtr BuildPredicateSuperset(
    const NQumir::NAst::TStructType& output,
    const std::vector<NQumir::NAst::TExprPtr>& alternatives);

NQumir::NAst::TExprPtr ConjoinPredicates(
    const std::vector<NQumir::NAst::TExprPtr>& predicates);

} // namespace NQdb

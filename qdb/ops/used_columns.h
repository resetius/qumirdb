#pragma once

#include <qdb/ops/operator.h>

#include <string>
#include <unordered_set>

namespace NQqb {

// Returns the set of source column names transitively required by the subtree.
std::unordered_set<std::string> CollectUsedColumns(const TOperatorPtr& op);

} // namespace NQqb

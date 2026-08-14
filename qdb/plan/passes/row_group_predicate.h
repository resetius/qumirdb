#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Copies a predicate from every direct Filter(Source) pair onto its source as
// a Parquet row-group pruning hint. This does not rewrite or remove filters.
void AttachRowGroupPredicates(const TOperatorPtr& root);

} // namespace NQdb

#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Rewrites `limit(sort(input), N, 0)` to `top-sort(input, keys, N)`.
// The pass returns the possibly replaced root and recursively optimizes child
// operators first.
TOperatorPtr ApplyTopSort(const TOperatorPtr& root);

} // namespace NQdb

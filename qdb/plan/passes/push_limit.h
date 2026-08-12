#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Rewrites `limit(project(x)) -> project(limit(x))`, valid because a projection
// preserves row count and order. Run before ApplyTopSort so a limit separated
// from its sort by a strip projection (an ORDER BY key absent from the select
// list) still fuses into a top-sort.
TOperatorPtr PushDownLimits(const TOperatorPtr& root);

} // namespace NQdb

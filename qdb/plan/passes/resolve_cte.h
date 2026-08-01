#pragma once

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Inlines every TCteRef with an independent clone of its definition plan.
// Temporary — the reuse phase replaces this with a materialize/inline decision.
TOperatorPtr ResolveCteRefs(TOperatorPtr plan);

} // namespace NQdb

#pragma once

#include <qumir/parser/core/printer.h>

#include <qdb/plan/ops/operator.h>

#include <ostream>

namespace NQdb {
namespace NSexp {

NQumir::NAst::NCore::TPrintExprFactory MakeRelPrinters();

// Prints a pre-reuse plan as a self-contained s-expression. When the plan
// references CTEs it emits a (query (cte <id> ...) ... (main ...)) envelope so
// the definitions round-trip; otherwise it prints the bare operator tree.
// Post-reuse materialized CTE plans are not serializable by this format yet.
void PrintRelPlan(std::ostream& out, const TOperatorPtr& plan);

} // namespace NSexp
} // namespace NQdb

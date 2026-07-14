#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/join_order.h>

namespace NQdb {

struct TPlanPassDiagnostics {
    TJoinReorderDiagnostics JoinReorder;
};

struct TPlanPassOptions {
    bool EnableCbo = true;
    TPlanPassDiagnostics* Diagnostics = nullptr;
};

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options = {});

} // namespace NQdb

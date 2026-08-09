#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/typing.h>

#include <memory>

namespace NQdb {

class TExternalCatalogSnapshot;

struct TPlanPassDiagnostics {
    TJoinReorderDiagnostics JoinReorder;
};

struct TPlanPassOptions {
    bool EnableCbo = true;
    TPlanPassDiagnostics* Diagnostics = nullptr;
    NKernel::TAnnotationContext Annotation;
};

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options = {});

} // namespace NQdb

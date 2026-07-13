#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

struct TPlanPassOptions {
    bool EnableCbo = true;
};

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options = {});

} // namespace NQdb

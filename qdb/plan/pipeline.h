#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

void ApplyPlanPasses(TOperatorPtr& plan);

} // namespace NQdb

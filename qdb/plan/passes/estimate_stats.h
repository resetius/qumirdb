#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

TStatsPtr EstimateStats(TOperatorPtr op);

} // namespace NQdb
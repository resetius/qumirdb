#pragma once

#include <qdb/exec/executor.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/scheduler/settings.h>

#include <iosfwd>
#include <memory>

namespace NQdb {
namespace NScheduler {

std::unique_ptr<IRuntimeNode> BuildSchedulerPlanPipeline(
    const TOperatorPtr& root,
    TSettings settings,
    std::ostream* diagnostics);

} // namespace NScheduler
} // namespace NQdb

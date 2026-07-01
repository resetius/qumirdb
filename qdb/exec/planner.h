#pragma once

#include <qdb/exec/executor.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/scheduler/settings.h>

#include <memory>
#include <ostream>

namespace NQdb {

// Builds a physical execution tree from a logical operator tree.
// Compiles kernels (filter/project) to LLVM function pointers.
//
// Current status:
//   - TSourceOperator    → TRuntimeSource    ✓
//   - TFilterOperator    → TRuntimeUnaryStreamingKernel ✓
//   - TProjectOperator   → TRuntimeUnaryStreamingKernel ✓
//   - TAggregateOperator → TRuntimeUnaryBlockingKernel ✓
class TPhysicalPlanner {
public:
    explicit TPhysicalPlanner(
        std::ostream* diagnostics = nullptr,
        NScheduler::TSettings schedulerSettings = {})
        : Diagnostics_(diagnostics)
        , SchedulerSettings_(schedulerSettings)
    {}

    void PrintRuntimePlan(const TOperatorPtr& root) const;
    std::unique_ptr<IRuntimeNode> Build(const TOperatorPtr& root);

private:
    void PrintRuntimePlan(const TOperatorPtr& root, int depth) const;

    std::ostream* Diagnostics_ = nullptr;
    NScheduler::TSettings SchedulerSettings_;
};

} // namespace NQdb

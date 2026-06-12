#pragma once

#include <qdb/exec/executor.h>
#include <qdb/ops/operator.h>

#include <memory>

namespace NQqb {

// Builds a physical execution tree from a logical operator tree.
// Compiles kernels (filter/project) to LLVM function pointers.
//
// Current status:
//   - TSourceOperator    → TRuntimeSource    ✓
//   - TFilterOperator    → TRuntimeFilter    ✓
//   - TProjectOperator   → column selection if exprs are identifiers, else TODO
//   - TAggregateOperator → TRuntimeAggregate ✓
class TPhysicalPlanner {
public:
    std::unique_ptr<IRuntimeNode> Build(const TOperatorPtr& root);
};

} // namespace NQqb

#pragma once

#include <qdb/exec/executor.h>
#include <qdb/ops/operator.h>

#include <memory>

namespace NQqb {

// Builds a physical execution tree from a logical operator tree.
// Compiles kernels (filter/project) to LLVM function pointers.
//
// Current status:
//   - TSourceOperator  → TSourceExecutor  ✓
//   - TFilterOperator  → TODO (kernel compilation pending)
//   - TProjectOperator → column selection if exprs are identifiers, else TODO
class TPhysicalPlanner {
public:
    std::unique_ptr<IRuntimeNode> Build(const TOperatorPtr& root);
};

} // namespace NQqb

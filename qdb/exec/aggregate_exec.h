#pragma once

#include <qdb/exec/executor.h>
#include <qdb/kernel/compiler.h>

#include <memory>

namespace NQqb {

// Pipeline breaker (L4): consumes all input batches via Kernels.Dispatch
// (init/update/destroy of the HashTable), then emits exactly one output
// TRowSet (group key column ++ one column per agg, in `aggs` order, as
// passed to TKernelCompiler::CompileAggregate) via Kernels.Finalize.
class TRuntimeAggregate : public IRuntimeNode {
public:
    TRuntimeAggregate(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        TAggregateKernels kernels);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    TAggregateKernels Kernels_;
    bool Done_ = false;
};

} // namespace NQqb

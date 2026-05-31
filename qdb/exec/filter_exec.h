#pragma once

#include <qdb/exec/executor.h>
#include <qdb/kernel/compiler.h>

#include <functional>
#include <memory>
#include <vector>

namespace NQqb {

class TRuntimeFilter : public IRuntimeNode {
public:
    TRuntimeFilter(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        TKernelCompiler::TFilterDispatch dispatch);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    TKernelCompiler::TFilterDispatch Dispatch_;
    std::vector<char> SelectionBuf_;
};

} // namespace NQqb

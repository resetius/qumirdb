#pragma once

#include <qdb/exec/executor.h>

#include <memory>

namespace NQdb {

class TRuntimeBinaryKernel : public IRuntimeNode {
public:
    TRuntimeBinaryKernel(
        std::unique_ptr<IRuntimeNode> left,
        std::unique_ptr<IRuntimeNode> right,
        NQumir::NAst::TTypePtr outputType);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }

protected:
    std::unique_ptr<IRuntimeNode> Left_;
    std::unique_ptr<IRuntimeNode> Right_;
    NQumir::NAst::TTypePtr OutputType_;
};

} // namespace NQdb

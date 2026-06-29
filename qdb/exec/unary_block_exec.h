#pragma once

#include <qdb/exec/executor.h>

#include <functional>
#include <memory>

namespace NQdb {

class TRuntimeUnaryBlockingKernel : public IRuntimeNode {
public:
    using TProcess = std::function<bool(IRuntimeNode&, TRowSet&)>;

    TRuntimeUnaryBlockingKernel(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        TProcess process);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    TProcess Process_;
    bool Done_ = false;
};

} // namespace NQdb

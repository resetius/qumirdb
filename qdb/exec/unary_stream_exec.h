#pragma once

#include <qdb/exec/executor.h>

#include <functional>
#include <memory>
#include <vector>

namespace NQdb {

struct TUnaryStreamingKernelState {
    std::vector<uint8_t> Selection;
};

class TRuntimeUnaryStreamingKernel : public IRuntimeNode {
public:
    using TProcess = std::function<void(TRowSet&, TUnaryStreamingKernelState&)>;

    TRuntimeUnaryStreamingKernel(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        TProcess process);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    TProcess Process_;
    TUnaryStreamingKernelState State_;
};

} // namespace NQdb

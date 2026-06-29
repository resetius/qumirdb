#include <qdb/exec/unary_stream_exec.h>

namespace NQdb {

TRuntimeUnaryStreamingKernel::TRuntimeUnaryStreamingKernel(
    std::unique_ptr<IRuntimeNode> input,
    NQumir::NAst::TTypePtr outputType,
    TProcess process)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Process_(std::move(process))
{}

bool TRuntimeUnaryStreamingKernel::Next(TRowSet& rowSet) {
    if (!Input_->Next(rowSet)) {
        return false;
    }
    Process_(rowSet, State_);
    return true;
}

} // namespace NQdb

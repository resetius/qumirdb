#include <qdb/exec/unary_block_exec.h>

#include <utility>

namespace NQdb {

TRuntimeUnaryBlockingKernel::TRuntimeUnaryBlockingKernel(
    std::unique_ptr<IRuntimeNode> input,
    NQumir::NAst::TTypePtr outputType,
    TProcess process)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Process_(std::move(process))
{}

bool TRuntimeUnaryBlockingKernel::Next(TRowSet& rowSet) {
    return Process_(*Input_, rowSet);
}

} // namespace NQdb

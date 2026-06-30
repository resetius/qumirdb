#include <qdb/exec/binary_exec.h>

#include <utility>

namespace NQdb {

TRuntimeBinaryKernel::TRuntimeBinaryKernel(
    std::unique_ptr<IRuntimeNode> left,
    std::unique_ptr<IRuntimeNode> right,
    NQumir::NAst::TTypePtr outputType)
    : Left_(std::move(left))
    , Right_(std::move(right))
    , OutputType_(std::move(outputType))
{}

} // namespace NQdb

#include <qdb/exec/filter_exec.h>

namespace NQdb {

TRuntimeFilter::TRuntimeFilter(
    std::unique_ptr<IRuntimeNode> input,
    NQumir::NAst::TTypePtr outputType,
    TKernelCompiler::TFilterDispatch dispatch)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Dispatch_(std::move(dispatch))
{}

bool TRuntimeFilter::Next(TRowSet& rowSet) {
    if (!Input_->Next(rowSet)) {
        return false;
    }
    SelectionBuf_.resize(rowSet.RowCount);
    rowSet.Selection = SelectionBuf_.data();
    Dispatch_(rowSet);
    return true;
}

} // namespace NQdb

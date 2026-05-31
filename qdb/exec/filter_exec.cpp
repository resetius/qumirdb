#include <qdb/exec/filter_exec.h>

namespace NQqb {

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
    SelectionBuf_.assign(rowSet.RowCount, false);
    auto* selection = reinterpret_cast<bool*>(SelectionBuf_.data());
    Dispatch_(rowSet, selection);
    rowSet.Selection = selection;
    return true;
}

} // namespace NQqb

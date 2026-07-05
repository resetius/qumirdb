#include <qdb/exec/filter_exec.h>

#include <utility>

namespace NQdb {

TUnaryStreamProcess MakeFilterProcess(
    TKernelCompiler::TFilterDispatch dispatch)
{
    return [dispatch = std::move(dispatch)](
        TRowSet& rowSet,
        TUnaryStreamingKernelState& state)
    {
        state.Selection.resize(rowSet.RowCount);
        rowSet.Selection = state.Selection.data();
        dispatch(rowSet);
    };
}

} // namespace NQdb

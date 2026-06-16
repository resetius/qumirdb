#include <qdb/exec/project_exec.h>

#include <utility>

namespace NQqb {

namespace {

struct TProjectedRowSetData {
    TRowSet Input = {};
    std::vector<std::vector<uint8_t>> ComputedBuffers; // one per computed column
    std::vector<TColumn> Columns;                      // output columns (mixed)
};

void DestroyProjectedRowSet(TRowSet* rowSet) {
    auto* data = static_cast<TProjectedRowSetData*>(rowSet->Private);
    Release(&data->Input);
    delete data;
}

} // namespace

TRuntimeProject::TRuntimeProject(
    std::unique_ptr<IRuntimeNode> input,
    NQumir::NAst::TTypePtr outputType,
    std::vector<TProjectColumn> columns,
    TKernelCompiler::TProjectDispatch computeDispatch,
    std::vector<size_t> computedWidths)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Columns_(std::move(columns))
    , ComputeDispatch_(std::move(computeDispatch))
    , ComputedWidths_(std::move(computedWidths))
{}

bool TRuntimeProject::Next(TRowSet& rowSet) {
    TRowSet input = {};
    if (!Input_->Next(input)) {
        return false;
    }

    auto* data = new TProjectedRowSetData;
    data->Input = input;

    // Materialize computed columns into owned buffers and run the kernel.
    const size_t numComputed = ComputedWidths_.size();
    if (numComputed > 0) {
        data->ComputedBuffers.resize(numComputed);
        std::vector<void*> outBuffers(numComputed);
        for (size_t k = 0; k < numComputed; ++k) {
            data->ComputedBuffers[k].resize(
                static_cast<size_t>(input.RowCount) * ComputedWidths_[k]);
            outBuffers[k] = data->ComputedBuffers[k].data();
        }
        ComputeDispatch_(&input, outBuffers.data());
    }

    // Output columns: ident -> zero-copy input column; computed -> owned buffer.
    data->Columns.reserve(Columns_.size());
    for (const auto& col : Columns_) {
        if (col.Computed) {
            data->Columns.push_back(TColumn{
                .Data = reinterpret_cast<char*>(data->ComputedBuffers[col.Index].data())});
        } else {
            data->Columns.push_back(input.Columns[col.Index]);
        }
    }

    rowSet = {
        .Columns = data->Columns.data(),
        .ColumnCount = static_cast<int64_t>(data->Columns.size()),
        .RowCount = input.RowCount,
        .Selection = input.Selection,
        .Destroy = DestroyProjectedRowSet,
        .Private = data,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQqb

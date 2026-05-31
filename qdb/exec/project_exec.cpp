#include <qdb/exec/project_exec.h>

#include <utility>

namespace NQqb {

namespace {

struct TProjectedRowSetData {
    TRowSet Input = {};
    std::vector<TColumn> Columns;
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
    std::vector<int32_t> columnIndices)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , ColumnIndices_(std::move(columnIndices))
{}

bool TRuntimeProject::Next(TRowSet& rowSet) {
    TRowSet input = {};
    if (!Input_->Next(input)) {
        return false;
    }

    auto* data = new TProjectedRowSetData;
    data->Input = input;
    data->Columns.reserve(ColumnIndices_.size());
    for (int32_t index : ColumnIndices_) {
        data->Columns.push_back(input.Columns[index]);
    }

    rowSet = {
        .Columns = data->Columns.data(),
        .ColumnCount = static_cast<int32_t>(data->Columns.size()),
        .RowCount = input.RowCount,
        .Selection = input.Selection,
        .Destroy = DestroyProjectedRowSet,
        .Private = data,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQqb

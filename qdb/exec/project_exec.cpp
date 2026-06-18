#include <qdb/exec/project_exec.h>
#include <qdb/modules/qumirdb_types.h>

#include <cstring>
#include <utility>

namespace NQqb {

namespace {

struct TProjectedRowSetData {
    TRowSet Input = {};
    std::vector<std::vector<uint8_t>> ComputedBuffers; // one per computed column
    std::vector<std::vector<char>> StringData;         // owned bytes for string columns
    std::vector<std::vector<int64_t>> StringOffsets;   // owned offsets for string columns
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
    std::vector<size_t> computedWidths,
    std::vector<bool> computedIsString)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Columns_(std::move(columns))
    , ComputeDispatch_(std::move(computeDispatch))
    , ComputedWidths_(std::move(computedWidths))
    , ComputedIsString_(std::move(computedIsString))
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

        // Post-process string computed columns: convert TStringView array
        // (ptr + size per row) into a proper offset-based string TColumn.
        data->StringData.resize(numComputed);
        data->StringOffsets.resize(numComputed);
        const size_t n = static_cast<size_t>(input.RowCount);
        for (size_t k = 0; k < numComputed; ++k) {
            if (k < ComputedIsString_.size() && ComputedIsString_[k]) {
                const auto* svs = reinterpret_cast<const TStringView*>(
                    data->ComputedBuffers[k].data());
                auto& offsets = data->StringOffsets[k];
                offsets.resize(n + 1);
                offsets[0] = 0;
                for (size_t r = 0; r < n; ++r) {
                    offsets[r + 1] = offsets[r] + svs[r].Size;
                }
                auto& strData = data->StringData[k];
                strData.resize(static_cast<size_t>(offsets[n]));
                for (size_t r = 0; r < n; ++r) {
                    if (svs[r].Size > 0 && svs[r].Data) {
                        std::memcpy(strData.data() + offsets[r], svs[r].Data,
                            static_cast<size_t>(svs[r].Size));
                    }
                }
            }
        }
    }

    // Output columns: ident -> zero-copy input column; computed -> owned buffer.
    data->Columns.reserve(Columns_.size());
    for (const auto& col : Columns_) {
        if (col.Computed) {
            const size_t k = static_cast<size_t>(col.Index);
            if (k < ComputedIsString_.size() && ComputedIsString_[k]) {
                data->Columns.push_back(TColumn{
                    .Data = data->StringData[k].data(),
                    .Offsets = data->StringOffsets[k].data(),
                    .OffsetWidth = 8,
                });
            } else {
                data->Columns.push_back(TColumn{
                    .Data = reinterpret_cast<char*>(data->ComputedBuffers[k].data())});
            }
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

#include <qdb/exec/project_exec.h>
#include <qdb/modules/qumirdb_types.h>

#include <cstring>
#include <utility>

namespace NQdb {

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

TRuntimeUnaryStreamingKernel::TProcess MakeProjectProcess(
    std::vector<TProjectColumn> columns,
    TKernelCompiler::TProjectDispatch computeDispatch,
    std::vector<size_t> computedWidths,
    std::vector<bool> computedIsString)
{
    return [
        columns = std::move(columns),
        computeDispatch = std::move(computeDispatch),
        computedWidths = std::move(computedWidths),
        computedIsString = std::move(computedIsString)](
        TRowSet& rowSet,
        TUnaryStreamingKernelState&)
    {
        auto* data = new TProjectedRowSetData;
        data->Input = rowSet;
        const int64_t rowCount = rowSet.RowCount;
        uint8_t* selection = rowSet.Selection;

        // Materialize computed columns into owned buffers and run the kernel.
        const size_t numComputed = computedWidths.size();
        if (numComputed > 0) {
            data->ComputedBuffers.resize(numComputed);
            std::vector<void*> outBuffers(numComputed);
            for (size_t k = 0; k < numComputed; ++k) {
                data->ComputedBuffers[k].resize(
                    static_cast<size_t>(rowCount) * computedWidths[k]);
                outBuffers[k] = data->ComputedBuffers[k].data();
            }
            computeDispatch(&rowSet, outBuffers.data());

            // Post-process string computed columns: convert TStringView array
            // (ptr + size per row) into a proper offset-based string TColumn.
            data->StringData.resize(numComputed);
            data->StringOffsets.resize(numComputed);
            const size_t n = static_cast<size_t>(rowCount);
            for (size_t k = 0; k < numComputed; ++k) {
                if (k < computedIsString.size() && computedIsString[k]) {
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
        data->Columns.reserve(columns.size());
        for (const auto& col : columns) {
            if (col.Computed) {
                const size_t k = static_cast<size_t>(col.Index);
                if (k < computedIsString.size() && computedIsString[k]) {
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
                data->Columns.push_back(rowSet.Columns[col.Index]);
            }
        }

        rowSet = {
            .Columns = data->Columns.data(),
            .ColumnCount = static_cast<int64_t>(data->Columns.size()),
            .RowCount = rowCount,
            .Selection = selection,
            .Destroy = DestroyProjectedRowSet,
            .Private = data,
            .RefCount = 1,
        };
    };
}

} // namespace NQdb

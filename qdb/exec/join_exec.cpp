#include <qdb/exec/join_exec.h>

#include <qdb/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>
#include <cstring>

namespace NQqb {

using namespace NQumir::NAst;

namespace {

// Owns the gathered buffers behind an output TRowSet (one batch).
struct TJoinedRowSetData {
    std::vector<TGatheredColumn> Gathered;
    std::vector<TColumn> Columns;
};

void DestroyJoinedRowSet(TRowSet* rowSet) {
    delete static_cast<TJoinedRowSetData*>(rowSet->Private);
}

// Validity of source row `row` in column `col` (true == non-null).
bool SourceValid(const TColumn& col, int32_t row) {
    if (!col.Mask) {
        return true;
    }
    const int32_t bit = col.MaskBitOffset + row;
    return ((col.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

// Reads offset `i` of a variable-length column (OffsetWidth 4 or 8).
int64_t OffsetAt(const TColumn& col, int32_t i) {
    if (col.OffsetWidth == 8) {
        return static_cast<const int64_t*>(col.Offsets)[i];
    }
    return static_cast<const int32_t*>(col.Offsets)[i];
}

void ClearBit(std::vector<uint8_t>& mask, size_t i) {
    mask[i / 8] &= ~(uint8_t(1) << (i % 8));
}

} // namespace

size_t JoinColumnFixedWidth(const TTypePtr& type) {
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8;
    }
    if (TMaybeType<TStringType>(inner)) {
        return 0; // variable-length
    }
    throw NQumir::TError(
        "join cannot materialize column of type " +
        (type ? type->ToString() : std::string("<null>")));
}

void TakeColumn(const TRowStore& store, const std::vector<TRowId>& rowIds,
    int32_t srcColIdx, const TTypePtr& type, TGatheredColumn& out)
{
    const size_t n = rowIds.size();
    const size_t width = JoinColumnFixedWidth(type);

    out.Data.clear();
    out.Offsets.clear();
    out.Mask.assign((n + 7) / 8, 0xff); // start all-valid; clear bits for nulls
    bool anyNull = false;

    auto markNull = [&](size_t j) {
        ClearBit(out.Mask, j);
        anyNull = true;
    };

    if (width == 0) {
        // String: pass 1 computes lengths/offsets, pass 2 copies payload bytes.
        out.Offsets.resize(n + 1);
        out.Offsets[0] = 0;
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            int64_t len = 0;
            if (id == kNullRowId) {
                markNull(j);
            } else {
                const TColumn& col = store.Column(id, srcColIdx);
                const int32_t row = RowIndex(id);
                if (!SourceValid(col, row)) {
                    markNull(j);
                } else {
                    len = OffsetAt(col, row + 1) - OffsetAt(col, row);
                }
            }
            out.Offsets[j + 1] = out.Offsets[j] + len;
        }
        out.Data.resize(static_cast<size_t>(out.Offsets[n]));
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            if (id == kNullRowId) {
                continue;
            }
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                continue;
            }
            const int64_t start = OffsetAt(col, row);
            const int64_t len = OffsetAt(col, row + 1) - start;
            if (len > 0) {
                std::memcpy(out.Data.data() + out.Offsets[j], col.Data + start, len);
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .Mask = anyNull ? out.Mask.data() : nullptr,
            .Offsets = out.Offsets.data(),
            .OffsetWidth = 8,
        };
    } else {
        // Fixed-width: copy `width` bytes per row; null/absent rows stay zeroed.
        out.Data.assign(n * width, 0);
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            if (id == kNullRowId) {
                markNull(j);
                continue;
            }
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                markNull(j);
                continue;
            }
            std::memcpy(out.Data.data() + j * width, col.Data + row * width, width);
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .Mask = anyNull ? out.Mask.data() : nullptr,
        };
    }

    if (!anyNull) {
        out.Mask.clear();
    }
}

bool TJoinOutputBuilder::NextBatch(TRowSet& out) {
    if (Cursor_ >= LeftIds_.size()) {
        return false;
    }
    const size_t n = std::min<size_t>(
        static_cast<size_t>(BatchRows_), LeftIds_.size() - Cursor_);

    const std::vector<TRowId> leftSlice(
        LeftIds_.begin() + Cursor_, LeftIds_.begin() + Cursor_ + n);
    const std::vector<TRowId> rightSlice(
        RightIds_.begin() + Cursor_, RightIds_.begin() + Cursor_ + n);

    auto* data = new TJoinedRowSetData;
    data->Gathered.resize(Columns_.size());
    data->Columns.resize(Columns_.size());
    for (size_t c = 0; c < Columns_.size(); ++c) {
        const auto& ref = Columns_[c];
        const TRowStore& store = (ref.Side == EJoinSide::Left) ? *Left_ : *Right_;
        const auto& ids = (ref.Side == EJoinSide::Left) ? leftSlice : rightSlice;
        TakeColumn(store, ids, ref.SrcColIdx, ref.Type, data->Gathered[c]);
        data->Columns[c] = data->Gathered[c].Column;
    }

    out = TRowSet{
        .Columns = data->Columns.data(),
        .ColumnCount = static_cast<int64_t>(Columns_.size()),
        .RowCount = static_cast<int64_t>(n),
        .Selection = nullptr,
        .Destroy = DestroyJoinedRowSet,
        .Private = data,
        .RefCount = 1,
    };
    Cursor_ += n;
    return true;
}

} // namespace NQqb

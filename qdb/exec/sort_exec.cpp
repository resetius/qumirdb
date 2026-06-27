#include <qdb/exec/sort_exec.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

struct TSortedRowSetData {
    std::vector<TGatheredColumn> Gathered;
    std::vector<TColumn> Columns;
};

void DestroySortedRowSet(TRowSet* rowSet) {
    delete static_cast<TSortedRowSetData*>(rowSet->Private);
}

bool IsBitSet(const uint8_t* data, int64_t bit) {
    return ((data[bit / 8] >> (bit % 8)) & 1) != 0;
}

void ClearBit(std::vector<uint8_t>& mask, size_t i) {
    mask[i / 8] &= ~(uint8_t(1) << (i % 8));
}

bool RowSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

bool SourceValid(const TColumn& col, int32_t row) {
    if (!col.Mask) {
        return true;
    }
    return IsBitSet(col.Mask, col.MaskBitOffset + row);
}

int64_t OffsetAt(const TColumn& col, int32_t i) {
    if (col.OffsetWidth == 8) {
        return static_cast<const int64_t*>(col.Offsets)[i];
    }
    return static_cast<const int32_t*>(col.Offsets)[i];
}

std::string_view StringAt(const TColumn& col, int32_t row) {
    const int64_t begin = OffsetAt(col, row);
    const int64_t end = OffsetAt(col, row + 1);
    return std::string_view(col.Data + begin, static_cast<size_t>(end - begin));
}

bool BoolAt(const TColumn& col, int32_t row) {
    return IsBitSet(reinterpret_cast<const uint8_t*>(col.Data), col.DataBitOffset + row);
}

ESortNulls EffectiveNulls(const TSortKey& key) {
    if (key.Nulls != ESortNulls::Default) {
        return key.Nulls;
    }
    // Matches PostgreSQL's default: ASC puts NULLS LAST, DESC puts NULLS FIRST.
    return key.Direction == ESortDirection::Desc ? ESortNulls::First : ESortNulls::Last;
}

template <class T>
int CompareScalar(T lhs, T rhs) {
    if (lhs < rhs) {
        return -1;
    }
    if (rhs < lhs) {
        return 1;
    }
    return 0;
}

template <class T>
T Load(const TColumn& col, int32_t row) {
    T value{};
    std::memcpy(&value, col.Data + static_cast<int64_t>(row) * sizeof(T), sizeof(T));
    return value;
}

int CompareValues(const TColumn& left, int32_t leftRow,
    const TColumn& right, int32_t rightRow,
    const TTypePtr& type)
{
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        switch (integer.Cast()->Kind) {
            case TIntegerType::I8:
                return CompareScalar(Load<int8_t>(left, leftRow), Load<int8_t>(right, rightRow));
            case TIntegerType::I16:
                return CompareScalar(Load<int16_t>(left, leftRow), Load<int16_t>(right, rightRow));
            case TIntegerType::I32:
                return CompareScalar(Load<int32_t>(left, leftRow), Load<int32_t>(right, rightRow));
            case TIntegerType::I64:
                return CompareScalar(Load<int64_t>(left, leftRow), Load<int64_t>(right, rightRow));
            case TIntegerType::U8:
                return CompareScalar(Load<uint8_t>(left, leftRow), Load<uint8_t>(right, rightRow));
            case TIntegerType::U16:
                return CompareScalar(Load<uint16_t>(left, leftRow), Load<uint16_t>(right, rightRow));
            case TIntegerType::U32:
                return CompareScalar(Load<uint32_t>(left, leftRow), Load<uint32_t>(right, rightRow));
            case TIntegerType::U64:
                return CompareScalar(Load<uint64_t>(left, leftRow), Load<uint64_t>(right, rightRow));
        }
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return CompareScalar(Load<double>(left, leftRow), Load<double>(right, rightRow));
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return CompareScalar(BoolAt(left, leftRow), BoolAt(right, rightRow));
    }
    if (TMaybeType<TStringType>(valueType)) {
        return StringAt(left, leftRow).compare(StringAt(right, rightRow));
    }
    throw std::runtime_error(
        "sort: unsupported key type " +
        (type ? type->ToString() : std::string("<null>")));
}

bool LessByKey(const TRowStore& store, TRowId leftId, TRowId rightId,
    const TSortKey& key, const TSortColumnRef& keyColumn)
{
    const auto& leftBatch = store.Batch(BatchIndex(leftId));
    const auto& rightBatch = store.Batch(BatchIndex(rightId));
    const auto& leftColumn = leftBatch.Columns[keyColumn.Index];
    const auto& rightColumn = rightBatch.Columns[keyColumn.Index];
    const int32_t leftRow = RowIndex(leftId);
    const int32_t rightRow = RowIndex(rightId);

    const bool leftValid = SourceValid(leftColumn, leftRow);
    const bool rightValid = SourceValid(rightColumn, rightRow);
    if (!leftValid || !rightValid) {
        if (leftValid == rightValid) {
            return false;
        }
        return EffectiveNulls(key) == ESortNulls::First ? !leftValid : leftValid;
    }

    int cmp = CompareValues(leftColumn, leftRow, rightColumn, rightRow, keyColumn.Type);
    if (key.Direction == ESortDirection::Desc) {
        cmp = -cmp;
    }
    return cmp < 0;
}

bool EqualByKey(const TRowStore& store, TRowId leftId, TRowId rightId,
    const TSortColumnRef& keyColumn)
{
    const auto& leftBatch = store.Batch(BatchIndex(leftId));
    const auto& rightBatch = store.Batch(BatchIndex(rightId));
    const auto& leftColumn = leftBatch.Columns[keyColumn.Index];
    const auto& rightColumn = rightBatch.Columns[keyColumn.Index];
    const int32_t leftRow = RowIndex(leftId);
    const int32_t rightRow = RowIndex(rightId);

    const bool leftValid = SourceValid(leftColumn, leftRow);
    const bool rightValid = SourceValid(rightColumn, rightRow);
    if (leftValid != rightValid) {
        return false;
    }
    if (!leftValid) {
        return true;
    }
    return CompareValues(leftColumn, leftRow, rightColumn, rightRow, keyColumn.Type) == 0;
}

bool SortRowsLess(const TRowStore& store, const std::vector<TSortKey>& keys,
    const std::vector<TSortColumnRef>& keyColumns, TRowId leftId, TRowId rightId)
{
    for (size_t i = 0; i < keys.size(); ++i) {
        if (LessByKey(store, leftId, rightId, keys[i], keyColumns[i])) {
            return true;
        }
        if (!EqualByKey(store, leftId, rightId, keyColumns[i])) {
            return false;
        }
    }
    return false;
}

bool HasAnyNullMask(const TRowStore& store, const std::vector<TRowId>& rows,
    int32_t columnIdx)
{
    for (TRowId rowId : rows) {
        if (store.Column(rowId, columnIdx).Mask) {
            return true;
        }
    }
    return false;
}

template <class T>
std::vector<T> MaterializeValues(const TRowStore& store,
    const std::vector<TRowId>& rows, int32_t columnIdx)
{
    std::vector<T> values;
    values.reserve(rows.size());
    for (TRowId rowId : rows) {
        const auto& column = store.Column(rowId, columnIdx);
        values.push_back(Load<T>(column, RowIndex(rowId)));
    }
    return values;
}

bool ApplyRadixKey(const TRowStore& store, const std::vector<TRowId>& rows,
    const TSortColumnRef& keyColumn, const TSortRadixKey& radixKey,
    bool desc, std::vector<uint32_t>& indices, std::vector<uint32_t>& work,
    std::vector<uint32_t>& counts)
{
    auto valueType = UnwrapNamedType(UnwrapNullableType(keyColumn.Type));
    void* valuesPtr = nullptr;
    std::vector<int8_t> i8;
    std::vector<int16_t> i16;
    std::vector<int32_t> i32;
    std::vector<int64_t> i64;
    std::vector<uint8_t> u8;
    std::vector<uint16_t> u16;
    std::vector<uint32_t> u32;
    std::vector<uint64_t> u64;
    std::vector<double> f64;

    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        switch (integer.Cast()->Kind) {
            case TIntegerType::I8:
                i8 = MaterializeValues<int8_t>(store, rows, keyColumn.Index);
                valuesPtr = i8.data();
                break;
            case TIntegerType::I16:
                i16 = MaterializeValues<int16_t>(store, rows, keyColumn.Index);
                valuesPtr = i16.data();
                break;
            case TIntegerType::I32:
                i32 = MaterializeValues<int32_t>(store, rows, keyColumn.Index);
                valuesPtr = i32.data();
                break;
            case TIntegerType::I64:
                i64 = MaterializeValues<int64_t>(store, rows, keyColumn.Index);
                valuesPtr = i64.data();
                break;
            case TIntegerType::U8:
                u8 = MaterializeValues<uint8_t>(store, rows, keyColumn.Index);
                valuesPtr = u8.data();
                break;
            case TIntegerType::U16:
                u16 = MaterializeValues<uint16_t>(store, rows, keyColumn.Index);
                valuesPtr = u16.data();
                break;
            case TIntegerType::U32:
                u32 = MaterializeValues<uint32_t>(store, rows, keyColumn.Index);
                valuesPtr = u32.data();
                break;
            case TIntegerType::U64:
                u64 = MaterializeValues<uint64_t>(store, rows, keyColumn.Index);
                valuesPtr = u64.data();
                break;
        }
    } else if (TMaybeType<TFloatType>(valueType)) {
        f64 = MaterializeValues<double>(store, rows, keyColumn.Index);
        valuesPtr = f64.data();
    }

    if (!valuesPtr) {
        return false;
    }
    radixKey.Dispatch(valuesPtr, indices.data(), work.data(), counts.data(),
        static_cast<int64_t>(indices.size()), desc);
    return true;
}

size_t SortColumnFixedWidth(const TTypePtr& type) {
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return 0;
    }
    if (TMaybeType<TStringType>(valueType)) {
        return 0;
    }
    throw std::runtime_error(
        "sort: unsupported output column type " +
        (type ? type->ToString() : std::string("<null>")));
}

void GatherColumn(const TRowStore& store, const std::vector<TRowId>& rowIds,
    int32_t srcColIdx, const TTypePtr& type, TGatheredColumn& out)
{
    const size_t n = rowIds.size();
    const auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    const bool isBool = static_cast<bool>(TMaybeType<TBoolType>(valueType));
    const bool isString = static_cast<bool>(TMaybeType<TStringType>(valueType));
    const size_t width = SortColumnFixedWidth(type);

    out.Data.clear();
    out.Offsets.clear();
    out.Mask.assign((n + 7) / 8, 0xff);
    bool anyNull = false;

    auto markNull = [&](size_t j) {
        ClearBit(out.Mask, j);
        anyNull = true;
    };

    if (isString) {
        out.Offsets.resize(n + 1);
        out.Offsets[0] = 0;
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            int64_t len = 0;
            if (!SourceValid(col, row)) {
                markNull(j);
            } else {
                len = OffsetAt(col, row + 1) - OffsetAt(col, row);
            }
            out.Offsets[j + 1] = out.Offsets[j] + len;
        }
        out.Data.resize(static_cast<size_t>(out.Offsets[n]));
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                continue;
            }
            const int64_t begin = OffsetAt(col, row);
            const int64_t len = OffsetAt(col, row + 1) - begin;
            if (len > 0) {
                std::memcpy(out.Data.data() + out.Offsets[j], col.Data + begin, len);
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .Mask = anyNull ? out.Mask.data() : nullptr,
            .Offsets = out.Offsets.data(),
            .OffsetWidth = 8,
        };
    } else if (isBool) {
        out.Data.assign((n + 7) / 8, 0);
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                markNull(j);
                continue;
            }
            if (BoolAt(col, row)) {
                out.Data[j / 8] |= char(uint8_t(1) << (j % 8));
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .DataBitOffset = 0,
            .Mask = anyNull ? out.Mask.data() : nullptr,
        };
    } else {
        out.Data.assign(n * width, 0);
        for (size_t j = 0; j < n; ++j) {
            const TRowId id = rowIds[j];
            const TColumn& col = store.Column(id, srcColIdx);
            const int32_t row = RowIndex(id);
            if (!SourceValid(col, row)) {
                markNull(j);
                continue;
            }
            std::memcpy(out.Data.data() + j * width, col.Data + static_cast<int64_t>(row) * width, width);
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

} // namespace

TRuntimeSort::TRuntimeSort(std::unique_ptr<IRuntimeNode> input,
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    std::vector<TSortRadixKey> radixKeys,
    int64_t batchRows)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , RadixKeys_(std::move(radixKeys))
    , BatchRows_(batchRows)
{}

bool TRuntimeSort::TryRadixSort() {
    if (Rows_.empty()) {
        return true;
    }
    if (Rows_.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    if (RadixKeys_.size() != Keys_.size() || KeyColumns_.size() != Keys_.size()) {
        return false;
    }
    for (size_t i = 0; i < Keys_.size(); ++i) {
        if (!RadixKeys_[i].Enabled || !RadixKeys_[i].Dispatch) {
            return false;
        }
        if (EffectiveNulls(Keys_[i]) != Keys_[i].Nulls && Keys_[i].Nulls != ESortNulls::Default) {
            return false;
        }
        if (HasAnyNullMask(Store_, Rows_, KeyColumns_[i].Index)) {
            return false;
        }
    }

    std::vector<uint32_t> indices(Rows_.size());
    std::vector<uint32_t> work(Rows_.size());
    std::vector<uint32_t> counts(256);
    for (uint32_t i = 0; i < static_cast<uint32_t>(indices.size()); ++i) {
        indices[i] = i;
    }

    for (size_t k = Keys_.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        if (!ApplyRadixKey(Store_, Rows_, KeyColumns_[keyIdx], RadixKeys_[keyIdx],
                Keys_[keyIdx].Direction == ESortDirection::Desc, indices, work, counts)) {
            return false;
        }
    }

    std::vector<TRowId> sorted;
    sorted.reserve(Rows_.size());
    for (uint32_t index : indices) {
        sorted.push_back(Rows_[index]);
    }
    Rows_ = std::move(sorted);
    return true;
}

void TRuntimeSort::Materialize() {
    if (Materialized_) {
        return;
    }
    TRowSet batch{};
    while (Input_->Next(batch)) {
        const int32_t batchIdx = Store_.PushBatch(batch);
        for (int32_t row = 0; row < batch.RowCount; ++row) {
            if (RowSelected(batch, row)) {
                Rows_.push_back(MakeRowId(batchIdx, row));
            }
        }
    }

    if (!TryRadixSort()) {
        std::stable_sort(Rows_.begin(), Rows_.end(),
            [&](TRowId leftId, TRowId rightId) {
                return SortRowsLess(Store_, Keys_, KeyColumns_, leftId, rightId);
            });
    }

    Materialized_ = true;
}

bool TRuntimeSort::Next(TRowSet& rowSet) {
    Materialize();
    if (Cursor_ >= Rows_.size()) {
        return false;
    }

    const size_t n = std::min<size_t>(
        static_cast<size_t>(BatchRows_), Rows_.size() - Cursor_);
    const std::vector<TRowId> slice(Rows_.begin() + Cursor_, Rows_.begin() + Cursor_ + n);

    auto* outputType = static_cast<TStructType*>(OutputType_.get());
    if (!outputType) {
        throw std::runtime_error("sort output must have TStructType");
    }
    auto* data = new TSortedRowSetData;
    data->Gathered.resize(outputType->Fields.size());
    data->Columns.resize(outputType->Fields.size());
    for (size_t c = 0; c < outputType->Fields.size(); ++c) {
        GatherColumn(Store_, slice, static_cast<int32_t>(c), outputType->Fields[c].second, data->Gathered[c]);
        data->Columns[c] = data->Gathered[c].Column;
    }

    rowSet = TRowSet{
        .Columns = data->Columns.data(),
        .ColumnCount = static_cast<int64_t>(data->Columns.size()),
        .RowCount = static_cast<int64_t>(n),
        .Selection = nullptr,
        .Destroy = DestroySortedRowSet,
        .Private = data,
        .RefCount = 1,
    };
    Cursor_ += n;
    return true;
}

} // namespace NQdb

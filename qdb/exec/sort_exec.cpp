#include <qdb/exec/sort_exec.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
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

size_t RadixValueWidth(const TTypePtr& type) {
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 8;
    }
    return 0;
}

std::vector<char> MaterializeRadixValues(const TRowStore& store,
    const std::vector<TRowId>& rows, const TSortColumnRef& keyColumn)
{
    const size_t width = RadixValueWidth(keyColumn.Type);
    if (width == 0) {
        return {};
    }
    std::vector<char> values(rows.size() * width);
    for (size_t i = 0; i < rows.size(); ++i) {
        const TRowId rowId = rows[i];
        const auto& column = store.Column(rowId, keyColumn.Index);
        std::memcpy(values.data() + i * width,
            column.Data + static_cast<int64_t>(RowIndex(rowId)) * width,
            width);
    }
    return values;
}

std::vector<uint8_t> MaterializeRadixValidity(const TRowStore& store,
    const std::vector<TRowId>& rows, const TSortColumnRef& keyColumn)
{
    std::vector<uint8_t> valid(rows.size(), uint8_t{1});
    for (size_t i = 0; i < rows.size(); ++i) {
        const TRowId rowId = rows[i];
        const auto& column = store.Column(rowId, keyColumn.Index);
        valid[i] = SourceValid(column, RowIndex(rowId)) ? uint8_t{1} : uint8_t{0};
    }
    return valid;
}

} // namespace

TRuntimeSort::TRuntimeSort(std::unique_ptr<IRuntimeNode> input,
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t batchRows)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , RadixKernel_(std::move(radixKernel))
    , BatchRows_(batchRows)
{}

bool TRuntimeSort::TryRadixSort() {
    if (Rows_.empty()) {
        return true;
    }
    if (Rows_.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    if (!RadixKernel_.Enabled || !RadixKernel_.Dispatch) {
        return false;
    }
    if (KeyColumns_.size() != Keys_.size()) {
        return false;
    }
    bool hasAnyNulls = false;
    for (size_t i = 0; i < Keys_.size(); ++i) {
        hasAnyNulls = hasAnyNulls || HasAnyNullMask(Store_, Rows_, KeyColumns_[i].Index);
    }
    if (hasAnyNulls && !RadixKernel_.NullableDispatch) {
        return false;
    }

    std::vector<uint32_t> indices(Rows_.size());
    std::vector<uint32_t> work(Rows_.size());
    std::vector<uint32_t> counts(hasAnyNulls ? 257 : 256);
    std::vector<std::vector<char>> valueStorage(Keys_.size());
    std::vector<void*> valuePtrs(Keys_.size());
    std::vector<std::vector<uint8_t>> validStorage(Keys_.size());
    std::vector<uint8_t*> validPtrs(Keys_.size());
    auto descs = std::make_unique<bool[]>(Keys_.size());
    auto nullsFirsts = std::make_unique<bool[]>(Keys_.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(indices.size()); ++i) {
        indices[i] = i;
    }
    for (size_t k = 0; k < Keys_.size(); ++k) {
        valueStorage[k] = MaterializeRadixValues(Store_, Rows_, KeyColumns_[k]);
        if (valueStorage[k].empty() && !Rows_.empty()) {
            return false;
        }
        valuePtrs[k] = valueStorage[k].data();
        descs[k] = Keys_[k].Direction == ESortDirection::Desc;
        nullsFirsts[k] = EffectiveNulls(Keys_[k]) == ESortNulls::First;
        if (hasAnyNulls) {
            validStorage[k] = MaterializeRadixValidity(Store_, Rows_, KeyColumns_[k]);
            validPtrs[k] = validStorage[k].data();
        }
    }

    if (hasAnyNulls) {
        RadixKernel_.NullableDispatch(valuePtrs.data(), validPtrs.data(),
            indices.data(), work.data(), counts.data(),
            static_cast<int64_t>(indices.size()), descs.get(), nullsFirsts.get());
    } else {
        RadixKernel_.Dispatch(valuePtrs.data(), indices.data(), work.data(), counts.data(),
            static_cast<int64_t>(indices.size()), descs.get());
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

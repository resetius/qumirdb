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

struct TTopSortPick {
    uint8_t Src = 0; // 0 = old state, 1 = incoming temp batch
    uint32_t Idx = 0;
};

struct TTopSortState {
    std::vector<TGatheredColumn> Gathered;
    std::vector<TColumn> Columns;
    int64_t RowCount = 0;

    const TRowSet RowSet() const {
        return TRowSet{
            .Columns = const_cast<TColumn*>(Columns.data()),
            .ColumnCount = static_cast<int64_t>(Columns.size()),
            .RowCount = RowCount,
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        };
    }
};

struct TTopSortScratch {
    std::unique_ptr<TTopSortState> State = std::make_unique<TTopSortState>();
    std::vector<uint32_t> TempRows;
    std::vector<uint32_t> TempLocalIndices;
    std::vector<uint32_t> TempRowsSorted;
    std::vector<uint32_t> Work;
    std::vector<uint32_t> Counts;
    std::vector<std::vector<char>> ValueStorage;
    std::vector<void*> ValuePtrs;
    std::vector<std::vector<uint8_t>> ValidStorage;
    std::vector<uint8_t*> ValidPtrs;
    std::unique_ptr<bool[]> Descs;
    std::unique_ptr<bool[]> NullsFirsts;
    size_t KeyCapacity = 0;
    std::vector<TTopSortPick> Picks;

    void EnsureKeyCapacity(size_t keyCount) {
        if (KeyCapacity == keyCount) {
            return;
        }
        ValueStorage.resize(keyCount);
        ValuePtrs.resize(keyCount);
        ValidStorage.resize(keyCount);
        ValidPtrs.resize(keyCount);
        Descs = std::make_unique<bool[]>(keyCount);
        NullsFirsts = std::make_unique<bool[]>(keyCount);
        KeyCapacity = keyCount;
    }
};

namespace {

struct TSortedRowSetData {
    std::vector<TGatheredColumn> Gathered;
    std::vector<TColumn> Columns;
};

struct TLimitRowSetData {
    TRowSet Input{};
    std::vector<uint8_t> Selection;
};

void DestroySortedRowSet(TRowSet* rowSet) {
    delete static_cast<TSortedRowSetData*>(rowSet->Private);
}

void DestroyLimitRowSet(TRowSet* rowSet) {
    auto* data = static_cast<TLimitRowSetData*>(rowSet->Private);
    Release(&data->Input);
    delete data;
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

bool SortRowsLessColumns(const TColumn* leftColumns, int32_t leftRow,
    const TColumn* rightColumns, int32_t rightRow,
    const std::vector<TSortKey>& keys,
    const std::vector<TSortColumnRef>& keyColumns)
{
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& keyColumn = keyColumns[i];
        const TColumn& leftColumn = leftColumns[keyColumn.Index];
        const TColumn& rightColumn = rightColumns[keyColumn.Index];
        const bool leftValid = SourceValid(leftColumn, leftRow);
        const bool rightValid = SourceValid(rightColumn, rightRow);
        if (!leftValid || !rightValid) {
            if (leftValid != rightValid) {
                return EffectiveNulls(keys[i]) == ESortNulls::First ? !leftValid : leftValid;
            }
            continue;
        }

        int cmp = CompareValues(leftColumn, leftRow, rightColumn, rightRow, keyColumn.Type);
        if (cmp != 0) {
            if (keys[i].Direction == ESortDirection::Desc) {
                cmp = -cmp;
            }
            return cmp < 0;
        }
    }
    return false;
}

size_t SortColumnFixedWidth(const TTypePtr& type);

void GatherTopSortColumn(const TColumn& stateColumn, const TColumn& tempColumn,
    const std::vector<TTopSortPick>& picks, size_t pickCount,
    const TTypePtr& type, TGatheredColumn& out)
{
    const auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    const bool isBool = static_cast<bool>(TMaybeType<TBoolType>(valueType));
    const bool isString = static_cast<bool>(TMaybeType<TStringType>(valueType));
    const size_t width = SortColumnFixedWidth(type);

    out.Data.clear();
    out.Offsets.clear();
    out.Mask.assign((pickCount + 7) / 8, 0xff);
    bool anyNull = false;

    auto source = [&](const TTopSortPick& pick) -> std::pair<const TColumn&, int32_t> {
        return pick.Src == 0
            ? std::pair<const TColumn&, int32_t>{stateColumn, static_cast<int32_t>(pick.Idx)}
            : std::pair<const TColumn&, int32_t>{tempColumn, static_cast<int32_t>(pick.Idx)};
    };
    auto markNull = [&](size_t i) {
        ClearBit(out.Mask, i);
        anyNull = true;
    };

    if (isString) {
        out.Offsets.resize(pickCount + 1);
        out.Offsets[0] = 0;
        for (size_t i = 0; i < pickCount; ++i) {
            auto [col, row] = source(picks[i]);
            int64_t len = 0;
            if (!SourceValid(col, row)) {
                markNull(i);
            } else {
                len = OffsetAt(col, row + 1) - OffsetAt(col, row);
            }
            out.Offsets[i + 1] = out.Offsets[i] + len;
        }
        out.Data.resize(static_cast<size_t>(out.Offsets[pickCount]));
        for (size_t i = 0; i < pickCount; ++i) {
            auto [col, row] = source(picks[i]);
            if (!SourceValid(col, row)) {
                continue;
            }
            const int64_t begin = OffsetAt(col, row);
            const int64_t len = OffsetAt(col, row + 1) - begin;
            if (len > 0) {
                std::memcpy(out.Data.data() + out.Offsets[i], col.Data + begin, len);
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .Mask = anyNull ? out.Mask.data() : nullptr,
            .Offsets = out.Offsets.data(),
            .OffsetWidth = 8,
        };
    } else if (isBool) {
        out.Data.assign((pickCount + 7) / 8, 0);
        for (size_t i = 0; i < pickCount; ++i) {
            auto [col, row] = source(picks[i]);
            if (!SourceValid(col, row)) {
                markNull(i);
                continue;
            }
            if (BoolAt(col, row)) {
                out.Data[i / 8] |= char(uint8_t(1) << (i % 8));
            }
        }
        out.Column = TColumn{
            .Data = out.Data.data(),
            .DataBitOffset = 0,
            .Mask = anyNull ? out.Mask.data() : nullptr,
        };
    } else {
        out.Data.assign(pickCount * width, 0);
        for (size_t i = 0; i < pickCount; ++i) {
            auto [col, row] = source(picks[i]);
            if (!SourceValid(col, row)) {
                markNull(i);
                continue;
            }
            std::memcpy(out.Data.data() + i * width,
                col.Data + static_cast<int64_t>(row) * width, width);
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

bool HasAnyNullMask(const TRowSet& batch, const std::vector<uint32_t>& rows,
    int32_t columnIdx)
{
    const TColumn& column = batch.Columns[columnIdx];
    return column.Mask && !rows.empty();
}

std::vector<char> MaterializeRadixValues(const TRowSet& batch,
    const std::vector<uint32_t>& rows, const TSortColumnRef& keyColumn)
{
    const size_t width = RadixValueWidth(keyColumn.Type);
    if (width == 0) {
        return {};
    }
    std::vector<char> values(rows.size() * width);
    const TColumn& column = batch.Columns[keyColumn.Index];
    for (size_t i = 0; i < rows.size(); ++i) {
        std::memcpy(values.data() + i * width,
            column.Data + static_cast<int64_t>(rows[i]) * width,
            width);
    }
    return values;
}

std::vector<uint8_t> MaterializeRadixValidity(const TRowSet& batch,
    const std::vector<uint32_t>& rows, const TSortColumnRef& keyColumn)
{
    std::vector<uint8_t> valid(rows.size(), uint8_t{1});
    const TColumn& column = batch.Columns[keyColumn.Index];
    for (size_t i = 0; i < rows.size(); ++i) {
        valid[i] = SourceValid(column, static_cast<int32_t>(rows[i]))
            ? uint8_t{1}
            : uint8_t{0};
    }
    return valid;
}

} // namespace

TSortProcessor::TSortProcessor(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t batchRows)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , RadixKernel_(std::move(radixKernel))
    , BatchRows_(batchRows)
{}

bool TSortProcessor::TryRadixSort()
{
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

void TSortProcessor::Add(TRowSet& rowSet)
{
    if (Materialized_) {
        throw std::runtime_error("sort processor is already finished");
    }

    const int32_t batchIdx = Store_.PushBatch(rowSet);
    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (RowSelected(rowSet, row)) {
            Rows_.push_back(MakeRowId(batchIdx, row));
        }
    }
    rowSet = {};
}

void TSortProcessor::Finish()
{
    if (Materialized_) {
        return;
    }
    if (!TryRadixSort()) {
        std::stable_sort(Rows_.begin(), Rows_.end(),
            [&](TRowId leftId, TRowId rightId) {
                return SortRowsLess(Store_, Keys_, KeyColumns_, leftId, rightId);
            });
    }

    Materialized_ = true;
}

bool TSortProcessor::Next(TRowSet& rowSet)
{
    Finish();
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

TRuntimeUnaryBlockingKernel::TProcess MakeSortProcess(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t batchRows)
{
    auto state = std::make_shared<TSortProcessor>(
        std::move(outputType),
        std::move(keys),
        std::move(keyColumns),
        std::move(radixKernel),
        batchRows);
    return [state = std::move(state)](IRuntimeNode& input, TRowSet& rowSet) {
        TRowSet batch{};
        while (input.Next(batch)) {
            state->Add(batch);
        }
        return state->Next(rowSet);
    };
}

struct TTopSortProcessState {
    TTopSortProcessState(
        TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        int64_t limit,
        int64_t batchRows);

    bool Next(IRuntimeNode& input, TRowSet& rowSet);

private:
    void Materialize(IRuntimeNode& input);
    bool TryRadixSortBatch(const TRowSet& batch, std::vector<uint32_t>& rows);

    TTypePtr OutputType_;
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    TSortRadixKernel RadixKernel_;
    int64_t Limit_ = 0;
    int64_t BatchRows_ = kJoinOutputBatchRows;

    bool Materialized_ = false;
    std::unique_ptr<TTopSortScratch> Scratch_;
    size_t Cursor_ = 0;
};

TTopSortProcessState::TTopSortProcessState(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t limit,
    int64_t batchRows)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , RadixKernel_(std::move(radixKernel))
    , Limit_(limit)
    , BatchRows_(batchRows)
    , Scratch_(std::make_unique<TTopSortScratch>())
{}

bool TTopSortProcessState::TryRadixSortBatch(
    const TRowSet& batch,
    std::vector<uint32_t>& rows)
{
    if (rows.empty()) {
        return true;
    }
    if (rows.size() > std::numeric_limits<uint32_t>::max()) {
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
        hasAnyNulls = hasAnyNulls || HasAnyNullMask(batch, rows, KeyColumns_[i].Index);
    }
    if (hasAnyNulls && !RadixKernel_.NullableDispatch) {
        return false;
    }

    auto& scratch = *Scratch_;
    scratch.EnsureKeyCapacity(Keys_.size());
    scratch.TempLocalIndices.resize(rows.size());
    scratch.Work.resize(rows.size());
    scratch.Counts.assign(hasAnyNulls ? 257 : 256, 0);
    scratch.TempRowsSorted.resize(rows.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(scratch.TempLocalIndices.size()); ++i) {
        scratch.TempLocalIndices[i] = i;
    }

    for (size_t k = 0; k < Keys_.size(); ++k) {
        scratch.ValueStorage[k] = MaterializeRadixValues(batch, rows, KeyColumns_[k]);
        if (scratch.ValueStorage[k].empty()) {
            return false;
        }
        scratch.ValuePtrs[k] = scratch.ValueStorage[k].data();
        scratch.Descs[k] = Keys_[k].Direction == ESortDirection::Desc;
        scratch.NullsFirsts[k] = EffectiveNulls(Keys_[k]) == ESortNulls::First;
        if (hasAnyNulls) {
            scratch.ValidStorage[k] = MaterializeRadixValidity(batch, rows, KeyColumns_[k]);
            scratch.ValidPtrs[k] = scratch.ValidStorage[k].data();
        }
    }

    if (hasAnyNulls) {
        RadixKernel_.NullableDispatch(scratch.ValuePtrs.data(), scratch.ValidPtrs.data(),
            scratch.TempLocalIndices.data(), scratch.Work.data(), scratch.Counts.data(),
            static_cast<int64_t>(scratch.TempLocalIndices.size()),
            scratch.Descs.get(), scratch.NullsFirsts.get());
    } else {
        RadixKernel_.Dispatch(scratch.ValuePtrs.data(),
            scratch.TempLocalIndices.data(), scratch.Work.data(), scratch.Counts.data(),
            static_cast<int64_t>(scratch.TempLocalIndices.size()), scratch.Descs.get());
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        scratch.TempRowsSorted[i] = rows[scratch.TempLocalIndices[i]];
    }
    rows.swap(scratch.TempRowsSorted);
    return true;
}

void TTopSortProcessState::Materialize(IRuntimeNode& input) {
    if (Materialized_) {
        return;
    }
    if (Limit_ <= 0) {
        Materialized_ = true;
        return;
    }

    auto* outputType = static_cast<TStructType*>(OutputType_.get());
    if (!outputType) {
        throw std::runtime_error("top-sort output must have TStructType");
    }

    TRowSet batch{};
    while (input.Next(batch)) {
        auto& tempRows = Scratch_->TempRows;
        auto& picks = Scratch_->Picks;
        tempRows.clear();
        tempRows.reserve(static_cast<size_t>(batch.RowCount));
        for (int32_t row = 0; row < batch.RowCount; ++row) {
            if (RowSelected(batch, row)) {
                tempRows.push_back(static_cast<uint32_t>(row));
            }
        }

        if (!TryRadixSortBatch(batch, tempRows)) {
            std::stable_sort(tempRows.begin(), tempRows.end(),
                [&](uint32_t lhs, uint32_t rhs) {
                    return SortRowsLessColumns(batch.Columns, static_cast<int32_t>(lhs),
                        batch.Columns, static_cast<int32_t>(rhs), Keys_, KeyColumns_);
                });
        }

        const TRowSet stateView = Scratch_->State->RowSet();
        const size_t stateRows = static_cast<size_t>(Scratch_->State->RowCount);
        const size_t limit = static_cast<size_t>(Limit_);
        const size_t pickCount = std::min(limit, stateRows + tempRows.size());
        picks.resize(pickCount);

        size_t left = 0;
        size_t right = 0;
        size_t out = 0;
        while (out < pickCount && (left < stateRows || right < tempRows.size())) {
            if (right == tempRows.size()) {
                picks[out++] = TTopSortPick{0, static_cast<uint32_t>(left++)};
                continue;
            }
            if (left == stateRows) {
                picks[out++] = TTopSortPick{1, tempRows[right++]};
                continue;
            }

            const uint32_t tempRow = tempRows[right];
            if (SortRowsLessColumns(batch.Columns, static_cast<int32_t>(tempRow),
                    stateView.Columns, static_cast<int32_t>(left), Keys_, KeyColumns_)) {
                picks[out++] = TTopSortPick{1, tempRow};
                ++right;
            } else {
                picks[out++] = TTopSortPick{0, static_cast<uint32_t>(left++)};
            }
        }

        auto next = std::make_unique<TTopSortState>();
        next->Gathered.resize(outputType->Fields.size());
        next->Columns.resize(outputType->Fields.size());
        next->RowCount = static_cast<int64_t>(pickCount);
        for (size_t c = 0; c < outputType->Fields.size(); ++c) {
            const TColumn emptyState{};
            const TColumn& stateColumn = Scratch_->State->RowCount == 0 ? emptyState : stateView.Columns[c];
            GatherTopSortColumn(stateColumn, batch.Columns[c], picks, pickCount,
                outputType->Fields[c].second, next->Gathered[c]);
            next->Columns[c] = next->Gathered[c].Column;
        }
        Scratch_->State = std::move(next);
        Release(&batch);
    }

    Materialized_ = true;
}

bool TTopSortProcessState::Next(IRuntimeNode& input, TRowSet& rowSet) {
    Materialize(input);
    if (!Scratch_ || !Scratch_->State ||
        Cursor_ >= static_cast<size_t>(Scratch_->State->RowCount)) {
        return false;
    }

    const size_t n = std::min<size_t>(
        static_cast<size_t>(BatchRows_), static_cast<size_t>(Scratch_->State->RowCount) - Cursor_);
    std::vector<TTopSortPick> picks(n);
    for (size_t i = 0; i < n; ++i) {
        picks[i] = TTopSortPick{0, static_cast<uint32_t>(Cursor_ + i)};
    }

    auto* outputType = static_cast<TStructType*>(OutputType_.get());
    auto* data = new TSortedRowSetData;
    data->Gathered.resize(outputType->Fields.size());
    data->Columns.resize(outputType->Fields.size());
    const TColumn emptyTemp{};
    for (size_t c = 0; c < outputType->Fields.size(); ++c) {
        GatherTopSortColumn(Scratch_->State->Columns[c], emptyTemp, picks, n,
            outputType->Fields[c].second, data->Gathered[c]);
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

TRuntimeUnaryBlockingKernel::TProcess MakeTopSortProcess(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t limit,
    int64_t batchRows)
{
    auto state = std::make_shared<TTopSortProcessState>(
        std::move(outputType),
        std::move(keys),
        std::move(keyColumns),
        std::move(radixKernel),
        limit,
        batchRows);
    return [state = std::move(state)](IRuntimeNode& input, TRowSet& rowSet) {
        return state->Next(input, rowSet);
    };
}

TRuntimeLimit::TRuntimeLimit(std::unique_ptr<IRuntimeNode> input,
    TTypePtr outputType,
    int64_t limit,
    int64_t offset,
    int64_t batchRows)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Processor_(limit, offset)
{
    (void)batchRows;
}

bool TRuntimeLimit::Next(TRowSet& rowSet)
{
    if (Processor_.Finished()) {
        return false;
    }

    TRowSet input{};
    while (Input_->Next(input)) {
        if (Processor_.Process(input, rowSet)) {
            return true;
        }
        input = {};
    }

    return false;
}

TLimitProcessor::TLimitProcessor(int64_t limit, int64_t offset)
    : Limit_(limit)
    , Offset_(offset)
{}

bool TLimitProcessor::Finished() const
{
    if (Limit_ <= 0 || Emitted_ >= Limit_) {
        return true;
    }
    return false;
}

bool TLimitProcessor::Process(TRowSet& input, TRowSet& rowSet)
{
    if (Finished()) {
        Release(&input);
        return false;
    }

    auto* data = new TLimitRowSetData;
    data->Input = input;
    data->Selection.assign(static_cast<size_t>(input.RowCount), uint8_t{0});

    bool any = false;
    for (int32_t row = 0; row < input.RowCount && Emitted_ < Limit_; ++row) {
        if (!RowSelected(input, row)) {
            continue;
        }
        if (Skipped_ < Offset_) {
            ++Skipped_;
            continue;
        }
        data->Selection[row] = 1;
        ++Emitted_;
        any = true;
    }

    if (!any) {
        Release(&data->Input);
        delete data;
        return false;
    }

    rowSet = TRowSet{
        .Columns = input.Columns,
        .ColumnCount = input.ColumnCount,
        .RowCount = input.RowCount,
        .Selection = data->Selection.data(),
        .Destroy = DestroyLimitRowSet,
        .Private = data,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQdb

#include <qdb/exec/window_exec.h>

#include <qdb/exec/kernel_rowset.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

bool RowIsSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

// String radix keys sort {prefix, rowId} pairs and need 4 i64 work slots/row.
size_t WorkStride(const std::vector<TSortColumnRef>& keyColumns) {
    for (const auto& keyColumn : keyColumns) {
        if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(keyColumn.Type)))) {
            return 4;
        }
    }
    return 1;
}

// PostgreSQL default: ASC → NULLS LAST, DESC → NULLS FIRST.
bool NullsFirst(const TSortKey& key) {
    if (key.Nulls != ESortNulls::Default) {
        return key.Nulls == ESortNulls::First;
    }
    return key.Direction == ESortDirection::Desc;
}

} // namespace

TWindowProcessor::TWindowProcessor(
    TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel kernel)
    : OutputType_(std::move(outputType))
    , Keys_(std::move(keys))
    , KeyColumns_(std::move(keyColumns))
    , Kernel_(std::move(kernel))
{}

void TWindowProcessor::Add(TRowSet& rowSet)
{
    if (Emitted_) {
        throw std::runtime_error("window processor is already finished");
    }
    const int32_t batchIdx = Store_.PushBatch(rowSet);
    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (RowIsSelected(rowSet, row)) {
            Rows_.push_back(MakeRowId(batchIdx, row));
        }
    }
    rowSet = {};
}

void TWindowProcessor::Finish() {}

bool TWindowProcessor::Next(TRowSet& rowSet)
{
    if (Emitted_) {
        return false;
    }
    Emitted_ = true;
    if (Rows_.empty()) {
        return false;
    }
    if (!Kernel_.Enabled || !Kernel_.Dispatch) {
        throw std::runtime_error("window kernel is unavailable");
    }
    if (KeyColumns_.size() != Keys_.size()) {
        throw std::runtime_error("window: key column mismatch");
    }

    std::vector<TRowId> work(Rows_.size() * WorkStride(KeyColumns_));
    std::vector<uint32_t> counts(256);
    auto descs = std::make_unique<bool[]>(Keys_.size());
    auto nullsFirsts = std::make_unique<bool[]>(Keys_.size());
    for (size_t k = 0; k < Keys_.size(); ++k) {
        descs[k] = Keys_[k].Direction == ESortDirection::Desc;
        nullsFirsts[k] = NullsFirst(Keys_[k]);
    }

    // One call: sort the row-ids in place, then materialize the whole partition
    // (input columns + window columns) into a kernel-owned rowset.
    const int64_t out = Kernel_.Dispatch(
        const_cast<TRowSet*>(Store_.Data()),
        Rows_.data(),
        work.data(),
        counts.data(),
        static_cast<int64_t>(Rows_.size()),
        descs.get(),
        nullsFirsts.get(),
        true,
        0,
        static_cast<int64_t>(Rows_.size()),
        &rowSet);
    if (out <= 0) {
        return false;
    }
    rowSet.Destroy = DestroyKernelOwnedRowSet;
    return true;
}

} // namespace NQdb

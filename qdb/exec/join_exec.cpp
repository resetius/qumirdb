#include <qdb/exec/join_exec.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>
#include <cstring>

namespace NQdb {

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

void TakeColumnFromBatch(const TRowSet& batch, const std::vector<int32_t>& rows,
    int32_t srcColIdx, const TTypePtr& type, TGatheredColumn& out)
{
    const size_t n = rows.size();
    const size_t width = JoinColumnFixedWidth(type);
    const TColumn& col = batch.Columns[srcColIdx];

    out.Data.clear();
    out.Offsets.clear();
    out.Mask.assign((n + 7) / 8, 0xff);
    bool anyNull = false;

    auto markNull = [&](size_t j) {
        ClearBit(out.Mask, j);
        anyNull = true;
    };

    if (width == 0) {
        out.Offsets.resize(n + 1);
        out.Offsets[0] = 0;
        for (size_t j = 0; j < n; ++j) {
            const int32_t row = rows[j];
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
            const int32_t row = rows[j];
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
        out.Data.assign(n * width, 0);
        for (size_t j = 0; j < n; ++j) {
            const int32_t row = rows[j];
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

TRuntimeCrossJoin::TRuntimeCrossJoin(std::unique_ptr<IRuntimeNode> left,
    std::unique_ptr<IRuntimeNode> right,
    TTypePtr outputType)
    : TRuntimeBinaryKernel(std::move(left), std::move(right),
          std::move(outputType))
{
    std::vector<TJoinColumnRef> columns;
    auto* leftStruct = static_cast<TStructType*>(Left_->OutputType().get());
    auto* rightStruct = static_cast<TStructType*>(Right_->OutputType().get());
    if (leftStruct) {
        for (int32_t i = 0; i < static_cast<int32_t>(leftStruct->Fields.size()); ++i) {
            columns.push_back({EJoinSide::Left, i, leftStruct->Fields[i].second});
        }
    }
    if (rightStruct) {
        for (int32_t j = 0; j < static_cast<int32_t>(rightStruct->Fields.size()); ++j) {
            columns.push_back({EJoinSide::Right, j, rightStruct->Fields[j].second});
        }
    }
    Builder_.emplace(&LeftRows_, &RightRows_, std::move(columns));
}

void TRuntimeCrossJoin::EnsureRightDrained() {
    if (RightDrained_) return;
    TRowSet batch{};
    while (Right_->Next(batch)) {
        RightTotalRows_ += batch.RowCount;
        RightRows_.PushBatch(batch);
    }
    RightDrained_ = true;
}

bool TRuntimeCrossJoin::FillNextLeftBatch() {
    if (LeftDone_ || RightTotalRows_ == 0) return false;
    TRowSet batch{};
    if (!Left_->Next(batch)) {
        LeftDone_ = true;
        return false;
    }
    int32_t leftBatchIdx = LeftRows_.PushBatch(batch);
    int32_t leftCount = LeftRows_.Batch(leftBatchIdx).RowCount;
    for (int32_t li = 0; li < leftCount; ++li) {
        TRowId leftId = MakeRowId(leftBatchIdx, li);
        for (int32_t rb = 0; rb < RightRows_.BatchCount(); ++rb) {
            int32_t rightCount = RightRows_.Batch(rb).RowCount;
            for (int32_t ri = 0; ri < rightCount; ++ri) {
                Builder_->AddPair(leftId, MakeRowId(rb, ri));
            }
        }
    }
    return true;
}

bool TRuntimeCrossJoin::Next(TRowSet& rowSet) {
    EnsureRightDrained();
    for (;;) {
        if (Builder_->NextBatch(rowSet)) return true;
        Builder_->ClearPairs();
        if (!FillNextLeftBatch()) return false;
    }
}

namespace {

constexpr int64_t kJoinInitialCapacity = 256;

} // namespace

TRuntimeJoin::TRuntimeJoin(std::unique_ptr<IRuntimeNode> left,
    std::unique_ptr<IRuntimeNode> right,
    NQumir::NAst::TTypePtr outputType, TJoinKernels kernels,
    EJoinType joinType,
    bool hasResidual)
    : TRuntimeBinaryKernel(std::move(left), std::move(right),
          std::move(outputType))
    , Kernels_(std::move(kernels))
    , JoinType_(joinType)
    , HasResidual_(hasResidual)
{
    // Output columns follow ComputeJoinOutputType: all left columns, then all
    // right columns (INNER), or only left columns (LeftSemi / LeftAnti).
    std::vector<TJoinColumnRef> columns;
    auto* leftStruct = static_cast<TStructType*>(Left_->OutputType().get());
    auto* rightStruct = static_cast<TStructType*>(Right_->OutputType().get());
    if (leftStruct) {
        for (int32_t i = 0; i < static_cast<int32_t>(leftStruct->Fields.size()); ++i) {
            columns.push_back({EJoinSide::Left, i, leftStruct->Fields[i].second});
        }
    }
    const bool isSemiAnti =
        (joinType == EJoinType::LeftSemi || joinType == EJoinType::LeftAnti);
    if (rightStruct && !isSemiAnti) {
        for (int32_t j = 0; j < static_cast<int32_t>(rightStruct->Fields.size()); ++j) {
            columns.push_back({EJoinSide::Right, j, rightStruct->Fields[j].second});
        }
    }
    Builder_.emplace(&LeftRows_, &RightRows_, std::move(columns));
}

TRuntimeJoin::~TRuntimeJoin() {
    while (!ReadyOutput_.empty()) {
        Release(&ReadyOutput_.front());
        ReadyOutput_.pop_front();
    }
    if (Initialized_) {
        Kernels_.DestroyTable(LeftTable_.data());
        Kernels_.DestroyTable(RightTable_.data());
    }
    // pb_destroy is null-safe even if no pairs were ever pushed.
    Kernels_.DestroyPairs(&PairBuffer_);
}

void TRuntimeJoin::EnsureInit() {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    Kernels_.Init(LeftTable_.data(), kJoinInitialCapacity);
    Kernels_.Init(RightTable_.data(), kJoinInitialCapacity);
}

void TRuntimeJoin::DrainKernelPairs() {
    for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
        Builder_->AddPair(PairBuffer_.Data[2 * i], PairBuffer_.Data[2 * i + 1]);
    }
    PairBuffer_.Count = 0;
}

void TRuntimeJoin::CollectMatchedLeftIds() {
    for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
        MatchedLeftIds_.insert(PairBuffer_.Data[2 * i]);
    }
    PairBuffer_.Count = 0;
}

void TRuntimeJoin::DrainStreamingPairs(const TRowSet& streamBatch, EJoinSide streamSide) {
    int64_t cursor = 0;
    while (cursor < PairBuffer_.Count) {
        const size_t n = std::min<size_t>(
            static_cast<size_t>(kJoinOutputBatchRows),
            static_cast<size_t>(PairBuffer_.Count - cursor));

        auto* data = new TJoinedRowSetData;

        std::vector<TJoinColumnRef> columns;
        auto* leftStruct = static_cast<TStructType*>(Left_->OutputType().get());
        auto* rightStruct = static_cast<TStructType*>(Right_->OutputType().get());
        if (leftStruct) {
            for (int32_t i = 0; i < static_cast<int32_t>(leftStruct->Fields.size()); ++i) {
                columns.push_back({EJoinSide::Left, i, leftStruct->Fields[i].second});
            }
        }
        if (rightStruct) {
            for (int32_t j = 0; j < static_cast<int32_t>(rightStruct->Fields.size()); ++j) {
                columns.push_back({EJoinSide::Right, j, rightStruct->Fields[j].second});
            }
        }

        data->Gathered.resize(columns.size());
        data->Columns.resize(columns.size());
        for (size_t c = 0; c < columns.size(); ++c) {
            const auto& ref = columns[c];
            if (ref.Side == streamSide) {
                std::vector<int32_t> rows;
                rows.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    const int64_t pairIdx = cursor + static_cast<int64_t>(i);
                    const TRowId id = (streamSide == EJoinSide::Left)
                        ? PairBuffer_.Data[2 * pairIdx]
                        : PairBuffer_.Data[2 * pairIdx + 1];
                    rows.push_back(RowIndex(id));
                }
                TakeColumnFromBatch(streamBatch, rows, ref.SrcColIdx, ref.Type,
                    data->Gathered[c]);
            } else {
                std::vector<TRowId> ids;
                ids.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    const int64_t pairIdx = cursor + static_cast<int64_t>(i);
                    ids.push_back((ref.Side == EJoinSide::Left)
                        ? PairBuffer_.Data[2 * pairIdx]
                        : PairBuffer_.Data[2 * pairIdx + 1]);
                }
                const TRowStore& store = (ref.Side == EJoinSide::Left) ? LeftRows_ : RightRows_;
                TakeColumn(store, ids, ref.SrcColIdx, ref.Type, data->Gathered[c]);
            }
            data->Columns[c] = data->Gathered[c].Column;
        }

        ReadyOutput_.push_back(TRowSet{
            .Columns = data->Columns.data(),
            .ColumnCount = static_cast<int64_t>(columns.size()),
            .RowCount = static_cast<int64_t>(n),
            .Selection = nullptr,
            .Destroy = DestroyJoinedRowSet,
            .Private = data,
            .RefCount = 1,
        });
        cursor += static_cast<int64_t>(n);
    }
    PairBuffer_.Count = 0;
}

void TRuntimeJoin::PullOneInputBatch() {
    TRowSet batch = {};
    if (!LeftDone_) {
        if (Left_->Next(batch)) {
            StoredLeftRows_ += batch.RowCount;
            LastLeftBatchRows_ = batch.RowCount;
            int32_t batchIdx = LeftRows_.PushBatch(batch);
            Kernels_.ProcessLeft(LeftTable_.data(), RightTable_.data(),
                const_cast<TRowSet*>(&LeftRows_.Batch(batchIdx)),
                batchIdx, &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()));
            DrainKernelPairs();
            return;
        }
        LeftDone_ = true;
    }
    if (!RightDone_) {
        if (Right_->Next(batch)) {
            StoredRightRows_ += batch.RowCount;
            LastRightBatchRows_ = batch.RowCount;
            int32_t batchIdx = RightRows_.PushBatch(batch);
            Kernels_.ProcessRight(RightTable_.data(), LeftTable_.data(),
                const_cast<TRowSet*>(&RightRows_.Batch(batchIdx)),
                batchIdx, &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()));
            DrainKernelPairs();
            return;
        }
        RightDone_ = true;
    }
    BothDone_ = LeftDone_ && RightDone_;
}

EJoinSide TRuntimeJoin::ChooseSymmetricPullSide() const {
    if (StoredLeftRows_ == 0 && StoredRightRows_ == 0) {
        return EJoinSide::Left;
    }
    if (StoredLeftRows_ <= StoredRightRows_) {
        if (LastLeftBatchRows_ > 0 &&
            StoredLeftRows_ + LastLeftBatchRows_ >= StoredRightRows_) {
            return EJoinSide::Right;
        }
        return EJoinSide::Left;
    }
    if (LastRightBatchRows_ > 0 &&
        StoredRightRows_ + LastRightBatchRows_ >= StoredLeftRows_) {
        return EJoinSide::Left;
    }
    return EJoinSide::Right;
}

void TRuntimeJoin::PullOneInnerInputBatch() {
    TRowSet batch = {};
    for (;;) {
        if (StreamMode_ == EJoinStreamMode::Symmetric) {
            const EJoinSide side = ChooseSymmetricPullSide();

            if (side == EJoinSide::Left) {
                if (LeftDone_) {
                    if (RightDone_) {
                        BothDone_ = true;
                        return;
                    }
                    StreamMode_ = EJoinStreamMode::StreamRightAgainstLeft;
                    continue;
                }
                if (Left_->Next(batch)) {
                    StoredLeftRows_ += batch.RowCount;
                    LastLeftBatchRows_ = batch.RowCount;
                    int32_t batchIdx = LeftRows_.PushBatch(batch);
                    Kernels_.ProcessLeft(LeftTable_.data(), RightTable_.data(),
                        const_cast<TRowSet*>(&LeftRows_.Batch(batchIdx)),
                        batchIdx, &PairBuffer_,
                        const_cast<TRowSet*>(LeftRows_.Data()),
                        const_cast<TRowSet*>(RightRows_.Data()));
                    DrainKernelPairs();
                    return;
                }
                LeftDone_ = true;
                if (RightDone_) {
                    BothDone_ = true;
                    return;
                }
                StreamMode_ = EJoinStreamMode::StreamRightAgainstLeft;
                continue;
            }

            if (RightDone_) {
                if (LeftDone_) {
                    BothDone_ = true;
                    return;
                }
                StreamMode_ = EJoinStreamMode::StreamLeftAgainstRight;
                continue;
            }
            if (Right_->Next(batch)) {
                StoredRightRows_ += batch.RowCount;
                LastRightBatchRows_ = batch.RowCount;
                int32_t batchIdx = RightRows_.PushBatch(batch);
                Kernels_.ProcessRight(RightTable_.data(), LeftTable_.data(),
                    const_cast<TRowSet*>(&RightRows_.Batch(batchIdx)),
                    batchIdx, &PairBuffer_,
                    const_cast<TRowSet*>(LeftRows_.Data()),
                    const_cast<TRowSet*>(RightRows_.Data()));
                DrainKernelPairs();
                return;
            }
            RightDone_ = true;
            if (LeftDone_) {
                BothDone_ = true;
                return;
            }
            StreamMode_ = EJoinStreamMode::StreamLeftAgainstRight;
            continue;
        }

        if (StreamMode_ == EJoinStreamMode::StreamRightAgainstLeft) {
            if (Right_->Next(batch)) {
                Kernels_.ProbeRightStream(LeftTable_.data(), &batch, -1, &PairBuffer_,
                    const_cast<TRowSet*>(LeftRows_.Data()),
                    const_cast<TRowSet*>(RightRows_.Data()));
                DrainStreamingPairs(batch, EJoinSide::Right);
                Release(&batch);
                return;
            }
            RightDone_ = true;
            BothDone_ = true;
            return;
        }

        if (Left_->Next(batch)) {
            Kernels_.ProbeLeftStream(RightTable_.data(), &batch, -1, &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()));
            DrainStreamingPairs(batch, EJoinSide::Left);
            Release(&batch);
            return;
        }
        LeftDone_ = true;
        BothDone_ = true;
        return;
    }
}

bool TRuntimeJoin::Next(TRowSet& rowSet) {
    EnsureInit();

    if (!ReadyOutput_.empty()) {
        rowSet = ReadyOutput_.front();
        ReadyOutput_.pop_front();
        return true;
    }

    // ── LeftSemi / LeftAnti + residual filter ────────────────────────────────
    // The join kernels are compiled as INNER (they emit pairs), and the injected
    // jt_residual_filter prunes pairs in-kernel. We collect the surviving left
    // row IDs, then emit each left row once (SEMI: matched, ANTI: unmatched).
    if ((JoinType_ == EJoinType::LeftSemi || JoinType_ == EJoinType::LeftAnti)
        && HasResidual_)
    {
        if (!ResidualSemiAntiDone_) {
            // Phase 1: drain all inputs; pairs are filtered inside the kernel.
            TRowSet batch{};
            while (Left_->Next(batch)) {
                int32_t bi = LeftRows_.PushBatch(batch);
                Kernels_.ProcessLeft(LeftTable_.data(), RightTable_.data(),
                    const_cast<TRowSet*>(&LeftRows_.Batch(bi)), bi, &PairBuffer_,
                    const_cast<TRowSet*>(LeftRows_.Data()),
                    const_cast<TRowSet*>(RightRows_.Data()));
                CollectMatchedLeftIds();
            }
            while (Right_->Next(batch)) {
                Kernels_.ProbeRightStream(LeftTable_.data(), &batch, -1, &PairBuffer_,
                    const_cast<TRowSet*>(LeftRows_.Data()),
                    const_cast<TRowSet*>(RightRows_.Data()));
                CollectMatchedLeftIds();
                Release(&batch);
            }

            // Phase 2: emit each left row once based on match membership.
            for (int32_t b = 0; b < LeftRows_.BatchCount(); ++b) {
                const int32_t cnt = LeftRows_.Batch(b).RowCount;
                for (int32_t r = 0; r < cnt; ++r) {
                    const TRowId id = MakeRowId(b, r);
                    const bool matched = MatchedLeftIds_.count(id) > 0;
                    if ((JoinType_ == EJoinType::LeftSemi && matched) ||
                        (JoinType_ == EJoinType::LeftAnti && !matched)) {
                        Builder_->AddPair(id, kNullRowId);
                    }
                }
            }
            ResidualSemiAntiDone_ = true;
        }
        if (Builder_->NextBatch(rowSet)) return true;
        Builder_->ClearPairs();
        return false;
    }

    if (JoinType_ == EJoinType::LeftSemi || JoinType_ == EJoinType::LeftAnti) {
        if (!SemiAntiFinalized_) {
            // Phase 1: consume all left batches → LeftTable_ + LeftRows_.
            // Right table is still empty so jt_emit_and_insert finds no matches
            // and emits no pairs — left row IDs accumulate in LeftTable_ buckets.
            TRowSet batch{};
            while (Left_->Next(batch)) {
                int32_t idx = LeftRows_.PushBatch(batch);
                Kernels_.ProcessLeft(LeftTable_.data(), RightTable_.data(),
                    const_cast<TRowSet*>(&LeftRows_.Batch(idx)), idx, &PairBuffer_,
                    const_cast<TRowSet*>(LeftRows_.Data()),
                    const_cast<TRowSet*>(RightRows_.Data()));
                PairBuffer_.Count = 0;
            }
            // Phase 2: consume all right batches → RightTable_ (keys only,
            // no row IDs). Batches are released immediately; no RightRows_ store.
            while (Right_->Next(batch)) {
                Kernels_.InsertKeyOnly(RightTable_.data(), nullptr,
                    &batch, 0, &PairBuffer_);
                Release(&batch);
                PairBuffer_.Count = 0;
            }
            // Phase 3: finalize — scan left table, probe right table, emit.
            Kernels_.FinalizeAntiSemi(LeftTable_.data(), RightTable_.data(), &PairBuffer_);
            for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
                Builder_->AddPair(PairBuffer_.Data[2 * i], PairBuffer_.Data[2 * i + 1]);
            }
            PairBuffer_.Count = 0;
            SemiAntiFinalized_ = true;
        }
        if (Builder_->NextBatch(rowSet)) {
            return true;
        }
        Builder_->ClearPairs();
        return false;
    }

    // LEFT / RIGHT OUTER: pipelined streaming (same as INNER) + deferred final scan.
    if (JoinType_ == EJoinType::Left || JoinType_ == EJoinType::Right) {
        for (;;) {
            if (Builder_->NextBatch(rowSet)) return true;
            Builder_->ClearPairs();
            if (OuterFinalized_) return false;
            if (!BothDone_) {
                PullOneInputBatch();
            } else {
                // Scan the unmatched side: for Left, scan LeftTable_ vs RightTable_;
                // for Right, scan RightTable_ vs LeftTable_ (pairs come out as
                // (rightRowId, -1) and are swapped to (-1, rightRowId) below).
                if (JoinType_ == EJoinType::Left) {
                    Kernels_.FinalizeOuter(LeftTable_.data(), RightTable_.data(), &PairBuffer_);
                } else {
                    Kernels_.FinalizeOuter(RightTable_.data(), LeftTable_.data(), &PairBuffer_);
                    for (int64_t i = 0; i < PairBuffer_.Count; ++i)
                        std::swap(PairBuffer_.Data[2 * i], PairBuffer_.Data[2 * i + 1]);
                }
                DrainKernelPairs();
                OuterFinalized_ = true;
            }
        }
    }

    // INNER join: pipelined — pull one batch per iteration. Residual filtering
    // (if any) already happened in-kernel, so emitted pairs are final.
    for (;;) {
        if (!ReadyOutput_.empty()) {
            rowSet = ReadyOutput_.front();
            ReadyOutput_.pop_front();
            return true;
        }
        if (Builder_->NextBatch(rowSet)) {
            return true;
        }
        Builder_->ClearPairs();
        if (BothDone_) {
            return false;
        }
        PullOneInnerInputBatch();
    }
}

} // namespace NQdb

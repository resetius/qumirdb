#include <qdb/exec/join_exec.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>
#include <cstring>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

constexpr int64_t kJoinInitialCapacity = 256;

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

std::vector<TJoinColumnRef> BuildJoinColumns(
    const TTypePtr& leftType,
    const TTypePtr& rightType,
    bool includeRight)
{
    std::vector<TJoinColumnRef> columns;
    auto* leftStruct = static_cast<TStructType*>(leftType.get());
    auto* rightStruct = static_cast<TStructType*>(rightType.get());
    if (leftStruct) {
        for (int32_t i = 0; i < static_cast<int32_t>(leftStruct->Fields.size()); ++i) {
            columns.push_back({EJoinSide::Left, i, leftStruct->Fields[i].second});
        }
    }
    if (rightStruct && includeRight) {
        for (int32_t j = 0; j < static_cast<int32_t>(rightStruct->Fields.size()); ++j) {
            columns.push_back({EJoinSide::Right, j, rightStruct->Fields[j].second});
        }
    }
    return columns;
}

bool DrainBuilder(TJoinOutputBuilder& builder, TRowSet& rowSet) {
    if (builder.NextBatch(rowSet)) {
        return true;
    }
    builder.ClearPairs();
    return false;
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

TInnerJoinProcessor::TInnerJoinProcessor(
    TTypePtr leftType,
    TTypePtr rightType,
    TJoinKernels kernels,
    EJoinType joinType,
    bool hasResidual)
    : LeftType_(std::move(leftType))
    , RightType_(std::move(rightType))
    , Kernels_(std::move(kernels))
    , JoinType_(joinType)
    , HasResidual_(hasResidual)
{
    // Semi/anti joins emit only the left columns.
    Builder_.emplace(&LeftRows_, &RightRows_,
        BuildJoinColumns(LeftType_, RightType_, !IsSemiAnti()));
}

bool TInnerJoinProcessor::IsOuter() const {
    return JoinType_ == EJoinType::Left || JoinType_ == EJoinType::Right;
}

bool TInnerJoinProcessor::IsSemiAnti() const {
    return JoinType_ == EJoinType::LeftSemi || JoinType_ == EJoinType::LeftAnti;
}

void TInnerJoinProcessor::CollectMatchedLeftIds() {
    for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
        MatchedLeftIds_.insert(PairBuffer_.Data[2 * i]);
    }
    PairBuffer_.Count = 0;
}

TInnerJoinProcessor::~TInnerJoinProcessor() {
    while (!ReadyOutput_.empty()) {
        Release(&ReadyOutput_.front());
        ReadyOutput_.pop_front();
    }
    if (Initialized_) {
        Kernels_.DestroyTable(LeftTable_.data());
        Kernels_.DestroyTable(RightTable_.data());
    }
    Kernels_.DestroyPairs(&PairBuffer_);
}

void TInnerJoinProcessor::EnsureInit() {
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    Kernels_.Init(LeftTable_.data(), kJoinInitialCapacity);
    Kernels_.Init(RightTable_.data(), kJoinInitialCapacity);
}

bool TInnerJoinProcessor::DrainReadyOutput(TRowSet& rowSet) {
    if (ReadyOutput_.empty()) {
        return false;
    }
    rowSet = ReadyOutput_.front();
    ReadyOutput_.pop_front();
    return true;
}

void TInnerJoinProcessor::DrainKernelPairs() {
    for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
        Builder_->AddPair(PairBuffer_.Data[2 * i], PairBuffer_.Data[2 * i + 1]);
    }
    PairBuffer_.Count = 0;
}

void TInnerJoinProcessor::DrainStreamingPairs(
    const TRowSet& streamBatch,
    EJoinSide streamSide)
{
    int64_t cursor = 0;
    while (cursor < PairBuffer_.Count) {
        const size_t n = std::min<size_t>(
            static_cast<size_t>(kJoinOutputBatchRows),
            static_cast<size_t>(PairBuffer_.Count - cursor));

        auto* data = new TJoinedRowSetData;
        auto columns = BuildJoinColumns(LeftType_, RightType_, true);
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
                TakeColumnFromBatch(
                    streamBatch,
                    rows,
                    ref.SrcColIdx,
                    ref.Type,
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
                const TRowStore& store = (ref.Side == EJoinSide::Left)
                    ? LeftRows_
                    : RightRows_;
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

EJoinSide TInnerJoinProcessor::ChooseSymmetricPullSide() const {
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

bool TInnerJoinProcessor::PullOneInputBatch(
    const TFetch& left,
    const TFetch& right)
{
    auto processLeft = [&]() {
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            return true;
        }

        StoredLeftRows_ += batch.RowCount;
        LastLeftBatchRows_ = batch.RowCount;
        const int32_t batchIdx = LeftRows_.PushBatch(batch);
        Kernels_.ProcessLeft(
            LeftTable_.data(),
            RightTable_.data(),
            const_cast<TRowSet*>(&LeftRows_.Batch(batchIdx)),
            batchIdx,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()));
        DrainKernelPairs();
        return true;
    };

    auto processRight = [&]() {
        TRowSet batch{};
        auto fetch = right(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            RightDone_ = true;
            return true;
        }

        StoredRightRows_ += batch.RowCount;
        LastRightBatchRows_ = batch.RowCount;
        const int32_t batchIdx = RightRows_.PushBatch(batch);
        Kernels_.ProcessRight(
            RightTable_.data(),
            LeftTable_.data(),
            const_cast<TRowSet*>(&RightRows_.Batch(batchIdx)),
            batchIdx,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()));
        DrainKernelPairs();
        return true;
    };

    auto streamLeft = [&]() {
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            BothDone_ = true;
            return true;
        }

        Kernels_.ProbeLeftStream(
            RightTable_.data(),
            &batch,
            -1,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()));
        DrainStreamingPairs(batch, EJoinSide::Left);
        Release(&batch);
        return true;
    };

    auto streamRight = [&]() {
        TRowSet batch{};
        auto fetch = right(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            RightDone_ = true;
            BothDone_ = true;
            return true;
        }

        Kernels_.ProbeRightStream(
            LeftTable_.data(),
            &batch,
            -1,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()));
        DrainStreamingPairs(batch, EJoinSide::Right);
        Release(&batch);
        return true;
    };

    // Outer joins must keep both sides materialized to emit unmatched rows in
    // the final scan, so they never switch to the streaming (drop-one-side)
    // path; they keep storing the remaining side after the other finishes.
    const bool allowStreaming = !IsOuter();

    for (;;) {
        if (allowStreaming &&
            StreamMode_ == EJoinStreamMode::StreamLeftAgainstRight) {
            return streamLeft();
        }
        if (allowStreaming &&
            StreamMode_ == EJoinStreamMode::StreamRightAgainstLeft) {
            return streamRight();
        }

        if (LeftDone_ && RightDone_) {
            BothDone_ = true;
            return true;
        }
        if (LeftDone_) {
            if (allowStreaming) {
                StreamMode_ = EJoinStreamMode::StreamRightAgainstLeft;
                continue;
            }
            return processRight();
        }
        if (RightDone_) {
            if (allowStreaming) {
                StreamMode_ = EJoinStreamMode::StreamLeftAgainstRight;
                continue;
            }
            return processLeft();
        }

        const auto first = ChooseSymmetricPullSide();
        if (first == EJoinSide::Left) {
            if (processLeft()) {
                return true;
            }
            return processRight();
        }
        if (processRight()) {
            return true;
        }
        return processLeft();
    }
}

void TInnerJoinProcessor::FinalizeOuterJoin() {
    // LEFT: emit left rows unmatched against the right table (right = null).
    // RIGHT: symmetric; swap pair columns so left stays the left side.
    if (JoinType_ == EJoinType::Left) {
        Kernels_.FinalizeOuter(LeftTable_.data(), RightTable_.data(), &PairBuffer_);
    } else {
        Kernels_.FinalizeOuter(RightTable_.data(), LeftTable_.data(), &PairBuffer_);
        for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
            std::swap(PairBuffer_.Data[2 * i], PairBuffer_.Data[2 * i + 1]);
        }
    }
    DrainKernelPairs();
    OuterFinalized_ = true;
}

// Semi/anti joins are blocking: consume all of the left (building the left
// table + row store), then all of the right, then emit. Left must be fully
// processed before the right so the non-residual path builds LeftTable while
// RightTable is still empty. Pulls one side at a time; left first.
bool TInnerJoinProcessor::PullSemiAntiBatch(
    const TFetch& left,
    const TFetch& right)
{
    if (!LeftDone_) {
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            return true;
        }
        const int32_t batchIdx = LeftRows_.PushBatch(batch);
        Kernels_.ProcessLeft(
            LeftTable_.data(),
            RightTable_.data(),
            const_cast<TRowSet*>(&LeftRows_.Batch(batchIdx)),
            batchIdx,
            &PairBuffer_,
            const_cast<TRowSet*>(LeftRows_.Data()),
            const_cast<TRowSet*>(RightRows_.Data()));
        if (HasResidual_) {
            CollectMatchedLeftIds();
        } else {
            PairBuffer_.Count = 0;
        }
        return true;
    }
    if (!RightDone_) {
        TRowSet batch{};
        auto fetch = right(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return false;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            RightDone_ = true;
            BothDone_ = true;
            return true;
        }
        if (HasResidual_) {
            Kernels_.ProbeRightStream(
                LeftTable_.data(),
                &batch,
                -1,
                &PairBuffer_,
                const_cast<TRowSet*>(LeftRows_.Data()),
                const_cast<TRowSet*>(RightRows_.Data()));
            CollectMatchedLeftIds();
        } else {
            Kernels_.InsertKeyOnly(RightTable_.data(), nullptr, &batch, 0,
                &PairBuffer_);
            PairBuffer_.Count = 0;
        }
        Release(&batch);
        return true;
    }
    BothDone_ = true;
    return true;
}

void TInnerJoinProcessor::FinalizeSemiAntiJoin() {
    Kernels_.FinalizeAntiSemi(LeftTable_.data(), RightTable_.data(), &PairBuffer_);
    for (int64_t i = 0; i < PairBuffer_.Count; ++i) {
        Builder_->AddPair(PairBuffer_.Data[2 * i], PairBuffer_.Data[2 * i + 1]);
    }
    PairBuffer_.Count = 0;
    SemiAntiFinalized_ = true;
}

void TInnerJoinProcessor::FinalizeResidualSemiAntiJoin() {
    // Emit each left row once: SEMI keeps matched, ANTI keeps unmatched.
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
    SemiAntiFinalized_ = true;
}

EJoinProcessorResult TInnerJoinProcessor::ProcessSemiAnti(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    if (DrainBuilder(*Builder_, output)) {
        return EJoinProcessorResult::OK;
    }
    if (SemiAntiFinalized_) {
        return EJoinProcessorResult::FINISHED;
    }
    while (!BothDone_) {
        if (!PullSemiAntiBatch(left, right)) {
            return EJoinProcessorResult::NEED_DATA;
        }
    }
    if (HasResidual_) {
        FinalizeResidualSemiAntiJoin();
    } else {
        FinalizeSemiAntiJoin();
    }
    if (DrainBuilder(*Builder_, output)) {
        return EJoinProcessorResult::OK;
    }
    return EJoinProcessorResult::FINISHED;
}

EJoinProcessorResult TInnerJoinProcessor::Process(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    EnsureInit();

    if (IsSemiAnti()) {
        return ProcessSemiAnti(left, right, output);
    }

    // Once both inputs are drained, outer joins emit their unmatched rows.
    auto finishedResult = [&]() -> EJoinProcessorResult {
        if (IsOuter() && !OuterFinalized_) {
            FinalizeOuterJoin();
            if (DrainReadyOutput(output) || DrainBuilder(*Builder_, output)) {
                return EJoinProcessorResult::OK;
            }
        }
        return EJoinProcessorResult::FINISHED;
    };

    if (DrainReadyOutput(output) || DrainBuilder(*Builder_, output)) {
        return EJoinProcessorResult::OK;
    }
    if (BothDone_) {
        return finishedResult();
    }
    if (!PullOneInputBatch(left, right)) {
        return EJoinProcessorResult::NEED_DATA;
    }
    if (DrainReadyOutput(output) || DrainBuilder(*Builder_, output)) {
        return EJoinProcessorResult::OK;
    }
    return BothDone_ ? finishedResult() : EJoinProcessorResult::NEED_DATA;
}

namespace {

bool CrossRowSelected(const TRowSet& batch, int32_t row) {
    return !batch.Selection || batch.Selection[row] != 0;
}

} // namespace

TCrossJoinProcessor::TCrossJoinProcessor(
    TTypePtr leftType,
    TTypePtr rightType)
{
    Builder_.emplace(&LeftRows_, &RightRows_,
        BuildJoinColumns(std::move(leftType), std::move(rightType), true));
}

EJoinProcessorResult TCrossJoinProcessor::Process(
    const TFetch& left,
    const TFetch& right,
    TRowSet& output)
{
    // Phase 1: fully buffer the (broadcast) right side.
    if (!RightDrained_) {
        for (;;) {
            TRowSet batch{};
            auto fetch = right(batch);
            if (fetch == EJoinFetchResult::NO_DATA) {
                return EJoinProcessorResult::NEED_DATA;
            }
            if (fetch == EJoinFetchResult::FINISHED) {
                RightDrained_ = true;
                break;
            }
            RightTotalRows_ += batch.RowCount;
            RightRows_.PushBatch(batch);
        }
    }

    // Phase 2: stream left, pairing each selected left row with every selected
    // right row.
    for (;;) {
        if (DrainBuilder(*Builder_, output)) {
            return EJoinProcessorResult::OK;
        }
        if (LeftDone_) {
            return EJoinProcessorResult::FINISHED;
        }
        TRowSet batch{};
        auto fetch = left(batch);
        if (fetch == EJoinFetchResult::NO_DATA) {
            return EJoinProcessorResult::NEED_DATA;
        }
        if (fetch == EJoinFetchResult::FINISHED) {
            LeftDone_ = true;
            continue;
        }
        if (RightTotalRows_ == 0) {
            Release(&batch);
            continue;
        }
        const int32_t li = LeftRows_.PushBatch(batch);
        const int32_t leftCount = LeftRows_.Batch(li).RowCount;
        for (int32_t l = 0; l < leftCount; ++l) {
            if (!CrossRowSelected(LeftRows_.Batch(li), l)) {
                continue;
            }
            const TRowId leftId = MakeRowId(li, l);
            for (int32_t rb = 0; rb < RightRows_.BatchCount(); ++rb) {
                const TRowSet& rightBatch = RightRows_.Batch(rb);
                for (int32_t r = 0; r < rightBatch.RowCount; ++r) {
                    if (!CrossRowSelected(rightBatch, r)) {
                        continue;
                    }
                    Builder_->AddPair(leftId, MakeRowId(rb, r));
                }
            }
        }
    }
}

} // namespace NQdb

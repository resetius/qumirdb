#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <qumir/parser/type.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace NQdb {

// Packed row identifier: (batchIdx << 32) | rowIdx. kNullRowId marks an absent
// row (NULL padding for OUTER joins, Stage 3).
using TRowId = int64_t;

inline constexpr TRowId kNullRowId = -1;

inline TRowId MakeRowId(int32_t batchIdx, int32_t rowIdx) {
    return (static_cast<int64_t>(batchIdx) << 32)
        | static_cast<int64_t>(static_cast<uint32_t>(rowIdx));
}

inline int32_t BatchIndex(TRowId id) {
    return static_cast<int32_t>(id >> 32);
}

inline int32_t RowIndex(TRowId id) {
    return static_cast<int32_t>(static_cast<uint32_t>(id & 0xffffffffu));
}

// Holds all input batches of one join side so their rows can be materialized
// later (on match) by RowId. Reuses the TRowSet Retain/Release refcount from
// io.h.
//
// Ownership: PushBatch TAKES ownership of one reference (move semantics, like
// TProjectedRowSetData in project_exec.cpp). The caller transfers the batch and
// must not use or Release it afterwards; the store Releases every batch on
// destruction. Note RefCount lives inside the TRowSet struct, so there is
// exactly one canonical copy — the one held here.
class TRowStore {
public:
    TRowStore() = default;
    TRowStore(const TRowStore&) = delete;
    TRowStore& operator=(const TRowStore&) = delete;

    ~TRowStore() {
        for (auto& batch : Batches_) {
            Release(&batch);
        }
    }

    // Returns the batch index used to build RowIds for rows of this batch.
    int32_t PushBatch(TRowSet batch) {
        Batches_.push_back(batch);
        return static_cast<int32_t>(Batches_.size() - 1);
    }

    int32_t BatchCount() const { return static_cast<int32_t>(Batches_.size()); }

    // Contiguous base of the batch array, indexable by batch index. Passed to
    // the join kernels so the injected jt_residual_filter can read columns of
    // already-stored rows by decoding packed row IDs. The pointer may be
    // invalidated by a later PushBatch (vector growth) — re-read it each call.
    const TRowSet* Data() const { return Batches_.data(); }

    const TRowSet& Batch(int32_t batchIdx) const { return Batches_[batchIdx]; }

    const TColumn& Column(int32_t batchIdx, int32_t colIdx) const {
        return Batches_[batchIdx].Columns[colIdx];
    }

    const TColumn& Column(TRowId id, int32_t colIdx) const {
        return Batches_[BatchIndex(id)].Columns[colIdx];
    }

private:
    std::vector<TRowSet> Batches_;
};

// One materialized output column with owned backing buffers. `Column` points
// into Data / Offsets / Mask, so a TGatheredColumn must outlive any TRowSet
// referencing its Column.
struct TGatheredColumn {
    std::vector<char> Data;        // fixed bytes, or string payload bytes
    std::vector<int64_t> Offsets;  // string offsets (OffsetWidth 8); empty for fixed
    std::vector<uint8_t> Mask;     // null bitmap; empty == all valid
    TColumn Column{};
};

// Byte width of a fixed-width physical type; 0 means variable-length (string).
// Throws NQumir::TError for types the join cannot yet materialize (e.g. bool).
size_t JoinColumnFixedWidth(const NQumir::NAst::TTypePtr& type);

// Gathers `srcColIdx` of `store` at the given RowIds into `out` (owned buffers).
// kNullRowId rows and source nulls become null in the output. `type` selects
// fixed-width vs string materialization.
void TakeColumn(const TRowStore& store, const std::vector<TRowId>& rowIds,
    int32_t srcColIdx, const NQumir::NAst::TTypePtr& type, TGatheredColumn& out);

// Default target rows per output batch (drain granularity).
inline constexpr int64_t kJoinOutputBatchRows = 1024;

enum class EJoinSide { Left, Right };

enum class EJoinStreamMode {
    Symmetric,
    StreamLeftAgainstRight,
    StreamRightAgainstLeft,
};

// One output column: which side it comes from, the source column index in that
// side's batches, and its physical type. Order follows ComputeJoinOutputType
// (all left columns, then all right columns).
struct TJoinColumnRef {
    EJoinSide Side;
    int32_t SrcColIdx;
    NQumir::NAst::TTypePtr Type;
};

// Accumulates matched (left, right) RowId pairs and drains them into owned
// output TRowSets of at most BatchRows rows each (gather via TakeColumn).
// Holds (non-owning) pointers to the two stores. The matcher (kernel or stub)
// appends pairs via AddPair; the executor drains via NextBatch.
class TJoinOutputBuilder {
public:
    TJoinOutputBuilder(const TRowStore* left, const TRowStore* right,
        std::vector<TJoinColumnRef> columns,
        int64_t batchRows = kJoinOutputBatchRows)
        : Left_(left)
        , Right_(right)
        , Columns_(std::move(columns))
        , BatchRows_(batchRows)
    {}

    void AddPair(TRowId leftId, TRowId rightId) {
        LeftIds_.push_back(leftId);
        RightIds_.push_back(rightId);
    }

    bool HasPending() const { return Cursor_ < LeftIds_.size(); }
    size_t PendingCount() const { return LeftIds_.size() - Cursor_; }

    void ClearPairs() {
        LeftIds_.clear();
        RightIds_.clear();
        Cursor_ = 0;
    }

    // Materializes up to BatchRows pending pairs into `out` (owned TRowSet with
    // custom Destroy). Returns false if nothing is pending.
    bool NextBatch(TRowSet& out);

private:
    const TRowStore* Left_;
    const TRowStore* Right_;
    std::vector<TJoinColumnRef> Columns_;
    int64_t BatchRows_;
    std::vector<TRowId> LeftIds_;
    std::vector<TRowId> RightIds_;
    size_t Cursor_ = 0;
};

enum class EJoinFetchResult {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

enum class EJoinProcessorResult {
    OK = 0,
    NEED_DATA = 1,
    FINISHED = 2,
};

class TInnerJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    TInnerJoinProcessor(
        NQumir::NAst::TTypePtr leftType,
        NQumir::NAst::TTypePtr rightType,
        TJoinKernels kernels,
        EJoinType joinType = EJoinType::Inner,
        bool hasResidual = false);
    TInnerJoinProcessor(const TInnerJoinProcessor&) = delete;
    TInnerJoinProcessor& operator=(const TInnerJoinProcessor&) = delete;
    ~TInnerJoinProcessor();

    EJoinProcessorResult Process(
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

private:
    // Mirrors the PairBuffer external type (modules/qumirdb.cpp).
    struct TPairBufferState {
        int64_t Count = 0;
        int64_t Capacity = 0;
        int64_t* Data = nullptr;
    };
    static_assert(sizeof(TPairBufferState) == TKernelCompiler::kPairBufferSize);

    bool IsOuter() const;
    bool IsSemiAnti() const;
    void EnsureInit();
    bool DrainReadyOutput(TRowSet& rowSet);
    void DrainKernelPairs();
    void DrainStreamingPairs(const TRowSet& streamBatch, EJoinSide streamSide);
    void CollectMatchedLeftIds();
    bool PullOneInputBatch(const TFetch& left, const TFetch& right);
    bool PullSemiAntiBatch(const TFetch& left, const TFetch& right);
    void FinalizeOuterJoin();
    void FinalizeSemiAntiJoin();
    void FinalizeResidualSemiAntiJoin();
    EJoinProcessorResult ProcessSemiAnti(
        const TFetch& left, const TFetch& right, TRowSet& output);
    EJoinSide ChooseSymmetricPullSide() const;

private:
    NQumir::NAst::TTypePtr LeftType_;
    NQumir::NAst::TTypePtr RightType_;
    TJoinKernels Kernels_;
    EJoinType JoinType_ = EJoinType::Inner;
    bool HasResidual_ = false;
    bool OuterFinalized_ = false;
    bool SemiAntiFinalized_ = false;
    std::unordered_set<TRowId> MatchedLeftIds_;
    TRowStore LeftRows_;
    TRowStore RightRows_;
    std::array<uint8_t, TKernelCompiler::kHashTableSize> LeftTable_{};
    std::array<uint8_t, TKernelCompiler::kHashTableSize> RightTable_{};
    TPairBufferState PairBuffer_;
    std::optional<TJoinOutputBuilder> Builder_;
    std::deque<TRowSet> ReadyOutput_;
    bool Initialized_ = false;
    bool LeftDone_ = false;
    bool RightDone_ = false;
    bool BothDone_ = false;
    EJoinStreamMode StreamMode_ = EJoinStreamMode::Symmetric;
    int64_t StoredLeftRows_ = 0;
    int64_t StoredRightRows_ = 0;
    int64_t LastLeftBatchRows_ = 0;
    int64_t LastRightBatchRows_ = 0;
};

// Fetch-driven Cartesian product join for the scheduler cross-scalar path:
// buffers the (small, broadcast) right side, then streams left batches, pairing
// each left row with every right row. Residual filtering is applied downstream
// (cross -> filter).
class TCrossJoinProcessor {
public:
    using TFetch = std::function<EJoinFetchResult(TRowSet&)>;

    TCrossJoinProcessor(
        NQumir::NAst::TTypePtr leftType,
        NQumir::NAst::TTypePtr rightType);
    TCrossJoinProcessor(const TCrossJoinProcessor&) = delete;
    TCrossJoinProcessor& operator=(const TCrossJoinProcessor&) = delete;

    EJoinProcessorResult Process(
        const TFetch& left,
        const TFetch& right,
        TRowSet& output);

private:
    TRowStore LeftRows_;
    TRowStore RightRows_;
    std::optional<TJoinOutputBuilder> Builder_;
    bool RightDrained_ = false;
    bool LeftDone_ = false;
    int64_t RightTotalRows_ = 0;
};

} // namespace NQdb

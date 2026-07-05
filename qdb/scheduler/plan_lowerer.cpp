#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/executor.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/scan_split.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace NQdb {
namespace {

struct TSchedulerSourceState {
    ISource* Source = nullptr;
    std::unique_ptr<ISource> OwnedSource;
};

struct TSchedulerUnaryStage {
    std::shared_ptr<const NScheduler::TUnaryCode> Code;
    std::function<std::shared_ptr<void>(size_t)> MakeState;
    NQumir::NAst::TTypePtr OutputType;
};

struct TAggregateBlockingState {
    explicit TAggregateBlockingState(TAggregateKernels kernels)
        : Processor(std::move(kernels))
    {}

    TAggregateProcessor Processor;
    bool Done = false;
};

struct TLimitBlockingState {
    TLimitBlockingState(int64_t limit, int64_t offset)
        : Processor(limit, offset)
    {}

    TLimitProcessor Processor;
};

struct TSortBlockingState {
    TSortBlockingState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            std::move(radixKernel))
    {}

    TSortProcessor Processor;
    bool InputFinished = false;
};

struct TMergeState {
    TMergeState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        size_t runCount)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            runCount)
        , RunFinished(runCount, false)
    {}

    TMergeProcessor Processor;
    std::vector<bool> RunFinished;
    bool InputsDone = false;
};

struct TTopSortBlockingState {
    TTopSortBlockingState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        int64_t limit)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            std::move(radixKernel),
            limit)
    {}

    TTopSortProcessor Processor;
    bool InputFinished = false;
};

struct TSchedulerInnerJoinState {
    TSchedulerInnerJoinState(
        NQumir::NAst::TTypePtr leftType,
        NQumir::NAst::TTypePtr rightType,
        TJoinKernels kernels,
        EJoinType joinType,
        bool hasResidual)
        : Processor(
            std::move(leftType),
            std::move(rightType),
            std::move(kernels),
            joinType,
            hasResidual)
    {}

    TInnerJoinProcessor Processor;
};

struct TSchedulerCrossJoinState {
    TSchedulerCrossJoinState(
        NQumir::NAst::TTypePtr leftType,
        NQumir::NAst::TTypePtr rightType)
        : Processor(std::move(leftType), std::move(rightType))
    {}

    TCrossJoinProcessor Processor;
};

struct TOwnedSelectionData {
    std::vector<uint8_t> Selection;
    void (*InnerDestroy)(TRowSet*) = nullptr;
    void* InnerPrivate = nullptr;
};

void DestroyOwnedSelectionRowSet(TRowSet* rowSet) {
    auto* data = static_cast<TOwnedSelectionData*>(rowSet->Private);
    if (data->InnerDestroy) {
        TRowSet inner = *rowSet;
        inner.Selection = nullptr;
        inner.Destroy = data->InnerDestroy;
        inner.Private = data->InnerPrivate;
        inner.RefCount = 1;
        Release(&inner);
    }
    delete data;
}

// The streaming filter kernel points rowSet.Selection at a buffer it reuses on
// every batch. Detach it before the rowset crosses a scheduler connection.
void DetachSelection(TRowSet& rowSet) {
    if (!rowSet.Selection || rowSet.RowCount <= 0) {
        return;
    }
    auto* data = new TOwnedSelectionData{
        .Selection = std::vector<uint8_t>(
            rowSet.Selection,
            rowSet.Selection + rowSet.RowCount),
        .InnerDestroy = rowSet.Destroy,
        .InnerPrivate = rowSet.Private,
    };
    rowSet.Selection = data->Selection.data();
    rowSet.Destroy = DestroyOwnedSelectionRowSet;
    rowSet.Private = data;
}

TSchedulerUnaryStage BuildSchedulerFilterStage(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics)
{
    auto runtime = BuildFilterRuntimeProcess(filter, inputType, diagnostics);
    auto code = std::make_shared<NScheduler::TUnaryCode>(
        [process = std::move(runtime.Process)](void* state, TRowSet& rowSet) {
            auto* kernelState = static_cast<TUnaryStreamingKernelState*>(state);
            process(rowSet, *kernelState);
            DetachSelection(rowSet);
        });
    return {
        .Code = std::move(code),
        .MakeState = [](size_t) {
            return std::make_shared<TUnaryStreamingKernelState>();
        },
        .OutputType = std::move(runtime.OutputType),
    };
}

TSchedulerUnaryStage BuildSchedulerProjectStage(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics)
{
    auto runtime = BuildProjectRuntimeProcess(project, inputType, diagnostics);
    auto code = std::make_shared<NScheduler::TUnaryCode>(
        [process = std::move(runtime.Process)](void* state, TRowSet& rowSet) {
            auto* kernelState = static_cast<TUnaryStreamingKernelState*>(state);
            process(rowSet, *kernelState);
        });

    return {
        .Code = std::move(code),
        .MakeState = [](size_t) {
            return std::make_shared<TUnaryStreamingKernelState>();
        },
        .OutputType = std::move(runtime.OutputType),
    };
}

EJoinFetchResult MapJoinFetch(NScheduler::EFetchResult result)
{
    switch (result) {
        case NScheduler::EFetchResult::OK:
            return EJoinFetchResult::OK;
        case NScheduler::EFetchResult::NO_DATA:
            return EJoinFetchResult::NO_DATA;
        case NScheduler::EFetchResult::FINISHED:
            return EJoinFetchResult::FINISHED;
    }
    return EJoinFetchResult::FINISHED;
}

NScheduler::ETaskResult MapJoinProcessResult(EJoinProcessorResult result)
{
    switch (result) {
        case EJoinProcessorResult::OK:
            return NScheduler::ETaskResult::OK;
        case EJoinProcessorResult::NEED_DATA:
            return NScheduler::ETaskResult::NEED_DATA;
        case EJoinProcessorResult::FINISHED:
            return NScheduler::ETaskResult::FINISHED;
    }
    return NScheduler::ETaskResult::FINISHED;
}

struct TLoweredOutput {
    std::vector<NScheduler::TTaskNode*> Producers;
    NQumir::NAst::TTypePtr OutputType;
};

// A rowset-wide hash over aggregate group keys computed in C++ (no JIT), so the
// shuffle works for any key type including strings. The shuffle only needs a
// consistent hash (equal keys -> equal partition); it need not match the
// aggregate kernel's internal hash, since partitions are disjoint and each
// re-aggregates fully. One hash per physical row; selection is applied by the
// scatter, not here.
inline std::function<bool(TRowSet*, uint64_t*)> MakeGroupKeyHash(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys)
{
    using namespace NQumir::NAst;

    struct TKeyColumn {
        int32_t Index = 0;
        int32_t Width = 0; // fixed-width byte size; 0 for string/bool
        bool IsString = false;
        bool IsBool = false;
    };

    std::vector<TKeyColumn> cols;
    cols.reserve(groupKeys.size());
    for (const auto& groupKey : groupKeys) {
        int32_t index = -1;
        TTypePtr fieldType;
        for (size_t i = 0; i < inputType.Fields.size(); ++i) {
            std::string name = inputType.Fields[i].first;
            auto dot = name.rfind('.');
            const std::string bare =
                dot != std::string::npos ? name.substr(dot + 1) : name;
            if (bare == groupKey || name == groupKey) {
                index = static_cast<int32_t>(i);
                fieldType = inputType.Fields[i].second;
                break;
            }
        }
        if (index < 0) {
            throw std::runtime_error(
                "group key not found in aggregate input: " + groupKey);
        }
        TKeyColumn kc;
        kc.Index = index;
        auto inner = UnwrapNamedType(UnwrapNullableType(fieldType));
        if (TMaybeType<TStringType>(inner)) {
            kc.IsString = true;
        } else if (TMaybeType<TBoolType>(inner)) {
            kc.IsBool = true;
        } else if (auto integer = TMaybeType<TIntegerType>(inner)) {
            kc.Width = integer.Cast()->BitWidth() / 8;
        } else if (TMaybeType<TFloatType>(inner)) {
            kc.Width = 8;
        } else {
            throw std::runtime_error("unsupported group key type for hash");
        }
        cols.push_back(kc);
    }

    return [cols](TRowSet* batch, uint64_t* hashes) -> bool {
        constexpr uint64_t kOffset = 0xcbf29ce484222325ULL;
        constexpr uint64_t kPrime = 0x100000001b3ULL;
        auto mixBytes = [](uint64_t h, const char* data, size_t len) {
            for (size_t i = 0; i < len; ++i) {
                h ^= static_cast<uint8_t>(data[i]);
                h *= kPrime;
            }
            return h;
        };
        for (int64_t row = 0; row < batch->RowCount; ++row) {
            uint64_t h = kOffset;
            for (const auto& kc : cols) {
                const TColumn& col = batch->Columns[kc.Index];
                const int32_t r = static_cast<int32_t>(row);
                const bool valid = !col.Mask ||
                    ((col.Mask[(col.MaskBitOffset + r) / 8] >>
                        ((col.MaskBitOffset + r) % 8)) & 1);
                if (!valid) {
                    h ^= 0x9e3779b97f4a7c15ULL; // null sentinel
                    h *= kPrime;
                    continue;
                }
                if (kc.IsString) {
                    int64_t begin;
                    int64_t end;
                    if (col.OffsetWidth == 8) {
                        begin = static_cast<const int64_t*>(col.Offsets)[r];
                        end = static_cast<const int64_t*>(col.Offsets)[r + 1];
                    } else {
                        begin = static_cast<const int32_t*>(col.Offsets)[r];
                        end = static_cast<const int32_t*>(col.Offsets)[r + 1];
                    }
                    h = mixBytes(h, col.Data + begin,
                        static_cast<size_t>(end - begin));
                } else if (kc.IsBool) {
                    const uint8_t v = (static_cast<const uint8_t*>(
                        static_cast<const void*>(col.Data))[
                            (col.DataBitOffset + r) / 8] >>
                        ((col.DataBitOffset + r) % 8)) & 1;
                    h = mixBytes(h,
                        static_cast<const char*>(static_cast<const void*>(&v)), 1);
                } else {
                    h = mixBytes(h, col.Data + static_cast<size_t>(r) * kc.Width,
                        static_cast<size_t>(kc.Width));
                }
            }
            hashes[row] = h;
        }
        return true;
    };
}

class TSchedulerGraphLowerer {
public:
    TSchedulerGraphLowerer(
        NScheduler::TTaskGraph& graph,
        NScheduler::TSettings settings,
        std::ostream* diagnostics)
        : Graph_(graph)
        , Settings_(std::move(settings))
        , Diagnostics_(diagnostics)
    {}

    size_t OutputLanes(const TOperatorPtr& op) const {
        if (auto n = TMaybeOp<TSourceOperator>(op)) {
            return std::max<size_t>(ScanSplits(*n.Cast()).size(), 1);
        }
        if (auto n = TMaybeOp<TFilterOperator>(op)) {
            return OutputLanes(n.Cast()->Input());
        }
        if (auto n = TMaybeOp<TProjectOperator>(op)) {
            return OutputLanes(n.Cast()->Input());
        }
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            const size_t childLanes = OutputLanes(n.Cast()->Input());
            if (childLanes == 0) {
                return 0;
            }
            // Grouped aggregate over a parallel input is hash-shuffled by group
            // key into partition-local aggregates (disjoint groups), so its
            // output keeps the shuffle partition count. Global aggregates
            // (`__group__` constant) stay single.
            if (!IsGlobalAggregate(*n.Cast()) &&
                !n.Cast()->GroupKeys().empty() && childLanes > 1)
            {
                return JoinPartitions(childLanes);
            }
            return 1;
        }
        if (TMaybeOp<TLimitOperator>(op) ||
            TMaybeOp<TSortOperator>(op) ||
            TMaybeOp<TTopSortOperator>(op))
        {
            return SupportedChild(op) ? 1 : 0;
        }
        if (auto n = TMaybeOp<TJoinOperator>(op)) {
            auto join = n.Cast();
            // Keyless join: cross join, parallelizable only with a scalar
            // (broadcast) side; keeps the vector-side partition count.
            if (join->Keys().empty()) {
                return CrossScalarLanes(*join);
            }
            // Inner, left/right outer, and left semi/anti equi-joins are
            // hash-partitionable (matches co-locate by key).
            const bool supportedType =
                join->JoinType() == EJoinType::Inner ||
                join->JoinType() == EJoinType::Left ||
                join->JoinType() == EJoinType::Right ||
                join->JoinType() == EJoinType::LeftSemi ||
                join->JoinType() == EJoinType::LeftAnti;
            if (!supportedType) {
                return 0;
            }
            const size_t leftLanes = OutputLanes(join->Left());
            const size_t rightLanes = OutputLanes(join->Right());
            if (leftLanes == 0 || rightLanes == 0) {
                return 0;
            }
            return JoinPartitions(std::max(leftLanes, rightLanes));
        }
        return 0;
    }

    // A "global" aggregate is a decorrelated ungrouped aggregate: its only
    // group key is the constant `__group__` marker (a projected 1). It produces
    // one row, so hash-shuffling it (all rows to one partition) is pure
    // overhead — treat it as ungrouped and gather to a single aggregate.
    bool IsGlobalAggregate(TAggregateOperator& aggregate) const {
        const auto& keys = aggregate.GroupKeys();
        if (keys.empty()) {
            return false;
        }
        for (const auto& key : keys) {
            if (key != "__group__") {
                return false;
            }
        }
        return true;
    }

    // Vector-side lane count for a keyless inner join. This mirrors the old
    // cross-join executor: buffer/gather the right side, then stream the left.
    size_t CrossScalarLanes(TJoinOperator& join) const {
        if (join.JoinType() != EJoinType::Inner || !join.Keys().empty()) {
            return 0;
        }
        const size_t lanes = OutputLanes(join.Left());
        const size_t rightLanes = OutputLanes(join.Right());
        if (lanes == 0 || rightLanes == 0) {
            return 0;
        }
        return lanes;
    }

    TLoweredOutput Lower(
        const TOperatorPtr& op,
        NScheduler::IConnection& outConn)
    {
        if (auto n = TMaybeOp<TSourceOperator>(op)) {
            return LowerSource(*n.Cast(), outConn);
        }
        if (auto n = TMaybeOp<TFilterOperator>(op)) {
            auto filter = n.Cast();
            return LowerUnary(
                filter->Input(),
                [&](const NQumir::NAst::TTypePtr& inType) {
                    return BuildSchedulerFilterStage(*filter, inType, Diagnostics_);
                },
                outConn);
        }
        if (auto n = TMaybeOp<TProjectOperator>(op)) {
            auto project = n.Cast();
            return LowerUnary(
                project->Input(),
                [&](const NQumir::NAst::TTypePtr& inType) {
                    return BuildSchedulerProjectStage(*project, inType, Diagnostics_);
                },
                outConn);
        }
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            return LowerAggregate(*n.Cast(), outConn);
        }
        if (auto n = TMaybeOp<TLimitOperator>(op)) {
            return LowerLimit(*n.Cast(), outConn);
        }
        if (auto n = TMaybeOp<TSortOperator>(op)) {
            return LowerSort(*n.Cast(), outConn);
        }
        if (auto n = TMaybeOp<TTopSortOperator>(op)) {
            return LowerTopSort(*n.Cast(), outConn);
        }
        if (auto n = TMaybeOp<TJoinOperator>(op)) {
            if (n.Cast()->Keys().empty()) {
                return LowerCrossJoin(*n.Cast(), outConn);
            }
            return LowerJoin(*n.Cast(), outConn);
        }
        throw std::runtime_error("scheduler lowering: unsupported operator");
    }

    size_t QueueCapacity() const {
        return std::max<size_t>(Settings_.Queue.RowsetCapacityPerLane, 1);
    }

private:
    struct TBlockingTail {
        std::shared_ptr<const NScheduler::TBlockingCode> Code;
        std::function<std::shared_ptr<void>()> MakeState;
        NQumir::NAst::TTypePtr OutputType;
    };

    struct TShuffleSide {
        NScheduler::THashShuffleConnection* Connection = nullptr;
        std::vector<NScheduler::TTaskNode*> Nodes;
    };

    // Blocking code for a local (top-)sort: drain the input into the processor,
    // then stream its sorted output. Works for both TSortBlockingState and
    // TTopSortBlockingState (same Add/Next/InputFinished interface).
    template <class TState>
    std::shared_ptr<const NScheduler::TBlockingCode> MakeSortBlockingCode() {
        return std::make_shared<NScheduler::TBlockingCode>(
            [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = static_cast<TState*>(state);
                TRowSet rowSet{};
                while (!s->InputFinished) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        s->InputFinished = true;
                        break;
                    }
                    s->Processor.Add(rowSet);
                    rowSet = {};
                }
                if (s->Processor.Next(output)) {
                    return NScheduler::ETaskResult::OK;
                }
                return NScheduler::ETaskResult::FINISHED;
            });
    }

    // Blocking tail for a local sort (top-K when topLimit is set, otherwise a
    // full sort). Shared by the single-lane path and the per-lane locals of the
    // partitioned merge.
    TBlockingTail MakeSortTail(
        const NQumir::NAst::TTypePtr& childType,
        const std::vector<TSortKey>& keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        std::optional<int64_t> topLimit)
    {
        if (topLimit) {
            const int64_t limit = *topLimit;
            return TBlockingTail{
                .Code = MakeSortBlockingCode<TTopSortBlockingState>(),
                .MakeState = [childType, keys, keyColumns, radixKernel, limit]()
                    -> std::shared_ptr<void>
                {
                    return std::make_shared<TTopSortBlockingState>(
                        childType, keys, keyColumns, radixKernel, limit);
                },
                .OutputType = childType,
            };
        }
        return TBlockingTail{
            .Code = MakeSortBlockingCode<TSortBlockingState>(),
            .MakeState = [childType, keys, keyColumns, radixKernel]()
                -> std::shared_ptr<void>
            {
                return std::make_shared<TSortBlockingState>(
                    childType, keys, keyColumns, radixKernel);
            },
            .OutputType = childType,
        };
    }

    // Blocking code for a limit/offset task.
    std::shared_ptr<const NScheduler::TBlockingCode> MakeLimitBlockingCode() {
        return std::make_shared<NScheduler::TBlockingCode>(
            [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = static_cast<TLimitBlockingState*>(state);
                if (s->Processor.Finished()) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                TRowSet rowSet{};
                while (!s->Processor.Finished()) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        return NScheduler::ETaskResult::FINISHED;
                    }
                    if (s->Processor.Process(rowSet, output)) {
                        return NScheduler::ETaskResult::OK;
                    }
                    rowSet = {};
                }
                return NScheduler::ETaskResult::FINISHED;
            });
    }

    // Binary blocking code adapting scheduler input ports to a join/cross
    // processor's fetch-callback interface. TState wraps the processor.
    template <class TState>
    std::shared_ptr<const NScheduler::TBinaryBlockingCode> MakeBinaryJoinCode() {
        return std::make_shared<NScheduler::TBinaryBlockingCode>(
            [](void* state,
               NScheduler::TInputPort& left,
               NScheduler::TInputPort& right,
               TRowSet& output)
            {
                auto* s = static_cast<TState*>(state);
                auto fetchLeft = [&](TRowSet& rowSet) {
                    return MapJoinFetch(left.Fetch(rowSet));
                };
                auto fetchRight = [&](TRowSet& rowSet) {
                    return MapJoinFetch(right.Fetch(rowSet));
                };
                return MapJoinProcessResult(
                    s->Processor.Process(fetchLeft, fetchRight, output));
            });
    }

    bool SupportedChild(const TOperatorPtr& op) const {
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TLimitOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TSortOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TTopSortOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        return false;
    }

    std::vector<NScheduler::TScanSplit> ScanSplits(TSourceOperator& src) const {
        auto* parquet = dynamic_cast<TParquetSource*>(&src.GetSource());
        if (!parquet) {
            return {};
        }
        return NScheduler::BuildScanSplits(
            parquet->ScanRowGroups(),
            Settings_.ScanSplit);
    }

    // Number of hash-shuffle partitions for a shuffled operator (grouped
    // aggregate / equi-join). Defaults to the input's own parallelism
    // (`inputLanes` — its scan-split lane count, a data-size proxy) capped at
    // half the worker count: small inputs stay narrow, large inputs fan out, and
    // the half-worker cap guards against shuffle over-sharding on big inputs
    // (measured to regress a few queries at cap = WorkerCount).
    // `--shuffle-partitions` overrides.
    size_t JoinPartitions(size_t inputLanes) const {
        auto partitions = Settings_.HashShuffle.PartitionCount;
        if (partitions == 0) {
            auto cap = std::max<size_t>(Settings_.Scheduler.WorkerCount / 2, 1);
            partitions = std::min(inputLanes, cap);
        }

        auto maxPartitions = Settings_.HashShuffle.MaxPartitionCount;
        if (maxPartitions == 0) {
            maxPartitions = partitions;
        }

        return std::max<size_t>(
            std::min(partitions, maxPartitions),
            1);
    }

    size_t ShuffleQueueCapacity() const {
        return std::max<size_t>(
            Settings_.HashShuffle.MaxQueuedRowsetsPerLane,
            1);
    }

    // Create a connection of the given type, size it, name it for diagnostics,
    // register it in the graph, and return a reference to the stored object.
    template <class TConn>
    TConn& AddConn(
        size_t srcLanes,
        size_t dstLanes,
        std::string name,
        size_t capacity)
    {
        auto conn = std::make_unique<TConn>(capacity);
        conn->Resize(srcLanes, dstLanes);
        conn->SetDebugName(std::move(name));
        return static_cast<TConn&>(Graph_.AddConnection(std::move(conn)));
    }

    template <class TConn>
    TConn& AddConn(size_t srcLanes, size_t dstLanes, std::string name) {
        return AddConn<TConn>(
            srcLanes, dstLanes, std::move(name), QueueCapacity());
    }

    TLoweredOutput LowerSource(
        TSourceOperator& src,
        NScheduler::IConnection& outConn)
    {
        auto outputType = BuildSourceRuntimeType(src);
        auto splits = ScanSplits(src);
        auto* sourcePtr = &src.GetSource();
        auto* parquetSource = dynamic_cast<TParquetSource*>(sourcePtr);
        auto code = std::make_shared<NScheduler::TSourceCode>(
            [](void* state, TRowSet& rowSet) {
                auto* sourceState = static_cast<TSchedulerSourceState*>(state);
                return sourceState->Source->Next(rowSet);
            });

        const size_t lanes = std::max<size_t>(splits.size(), 1);
        TLoweredOutput result;
        result.OutputType = std::move(outputType);
        result.Producers.reserve(lanes);
        for (size_t p = 0; p < lanes; ++p) {
            const auto* split = splits.empty() ? nullptr : &splits[p];
            std::shared_ptr<void> state;
            if (parquetSource && split && split->RowGroupCount) {
                auto owned = parquetSource->MakeRowGroupRangeSource(
                    split->FirstRowGroup,
                    split->RowGroupCount);
                auto* ptr = owned.get();
                state = std::make_shared<TSchedulerSourceState>(
                    TSchedulerSourceState{
                        .Source = ptr,
                        .OwnedSource = std::move(owned),
                    });
            } else {
                state = std::make_shared<TSchedulerSourceState>(
                    TSchedulerSourceState{.Source = sourcePtr});
            }
            auto task = std::make_unique<NScheduler::TSourceTask>(
                code,
                std::move(state),
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p});
            result.Producers.push_back(&Graph_.AddOwnedNode(std::move(task)));
        }
        return result;
    }

    template <typename TStageBuilder>
    TLoweredOutput LowerUnary(
        const TOperatorPtr& child,
        TStageBuilder buildStage,
        NScheduler::IConnection& outConn)
    {
        const size_t lanes = OutputLanes(child);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "unary-input");
        auto childOut = Lower(child, childConnRef);

        auto stage = buildStage(childOut.OutputType);
        TLoweredOutput result;
        result.OutputType = std::move(stage.OutputType);
        result.Producers.reserve(lanes);
        for (size_t p = 0; p < lanes; ++p) {
            auto task = std::make_unique<NScheduler::TUnaryTask>(
                stage.Code,
                stage.MakeState(p),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = p},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*childOut.Producers[p], node, childConnRef, p, p);
            result.Producers.push_back(&node);
        }
        return result;
    }

    template <typename TTailBuilder>
    TLoweredOutput LowerBlocking(
        const TOperatorPtr& child,
        TTailBuilder buildTail,
        NScheduler::IConnection& outConn)
    {
        const size_t lanes = OutputLanes(child);
        auto& gatherRef = AddConn<NScheduler::TGatherConnection>(
            lanes, 1, "blocking-input-gather");
        auto childOut = Lower(child, gatherRef);

        TBlockingTail tail = buildTail(childOut.OutputType);
        auto task = std::make_unique<NScheduler::TBlockingTask>(
            std::move(tail.Code),
            tail.MakeState(),
            NScheduler::TInputPort{.Connection = &gatherRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        for (size_t p = 0; p < lanes; ++p) {
            Graph_.AddEdge(*childOut.Producers[p], node, gatherRef, p, 0);
        }
        return TLoweredOutput{
            .Producers = {&node},
            .OutputType = std::move(tail.OutputType),
        };
    }

    // Builds the aggregate code + partition-local state factory + output type
    // for a given (already-lowered) input type. Kernels are compiled once and
    // shared across all partition states.
    TBlockingTail BuildAggregateTail(
        const NQumir::NAst::TTypePtr& childType,
        TAggregateOperator& aggregate)
    {
        auto* inputType =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!inputType) {
            throw std::runtime_error("aggregate input must have TStructType");
        }
        auto spec = NKernel::BuildAggregateKernelSpec(
            *inputType, aggregate.GroupKeys(), aggregate.Aggs());
        TKernelCompiler compiler(Diagnostics_);
        auto kernels = std::make_shared<TAggregateKernels>(
            compiler.CompileAggregate(spec));
        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = static_cast<TAggregateBlockingState*>(state);
                if (s->Done) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                TRowSet rowSet{};
                while (true) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        s->Done = true;
                        s->Processor.Finish(output);
                        return NScheduler::ETaskResult::OK;
                    }
                    s->Processor.Add(rowSet);
                    Release(&rowSet);
                    rowSet = {};
                }
            });
        return TBlockingTail{
            .Code = std::move(code),
            .MakeState = [kernels]() -> std::shared_ptr<void> {
                return std::make_shared<TAggregateBlockingState>(*kernels);
            },
            .OutputType = ComputeAggregateOutputType(
                childType, aggregate.GroupKeys(), aggregate.Aggs()),
        };
    }

    // Build the "combine" aggregate for the final phase of a cascade: it reads
    // the partial aggregates' output columns and merges them. count becomes a
    // sum of per-lane counts; sum/min/max stay the same function over their own
    // output column. Group keys (e.g. `__group__`) carry through unchanged.
    std::shared_ptr<TAggregateOperator> BuildCombineAggregate(
        TAggregateOperator& aggregate)
    {
        std::vector<TAggregateSpec> combineAggs;
        combineAggs.reserve(aggregate.Aggs().size());
        for (const auto& agg : aggregate.Aggs()) {
            std::string func = agg.Func == "count" ? "sum" : agg.Func;
            combineAggs.push_back(TAggregateSpec{
                .Name = agg.Name,
                .Func = std::move(func),
                .Arg = std::make_shared<NQumir::NAst::TIdentExpr>(
                    NQumir::TLocation{}, agg.Name),
            });
        }
        return std::make_shared<TAggregateOperator>(
            aggregate.Input(), aggregate.GroupKeys(), std::move(combineAggs));
    }

    // child[N] -> partial-aggregate[N] -> gather(N->1) -> final-combine[1].
    TLoweredOutput LowerAggregateCascade(
        TAggregateOperator& aggregate,
        size_t lanes,
        NScheduler::IConnection& outConn)
    {
        auto& childRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "aggregate-cascade-input");
        auto childOut = Lower(aggregate.Input(), childRef);

        auto partialTail = BuildAggregateTail(childOut.OutputType, aggregate);
        auto partialType = partialTail.OutputType;

        auto& gatherRef = AddConn<NScheduler::TGatherConnection>(
            lanes, 1, "aggregate-cascade-gather");

        std::vector<NScheduler::TTaskNode*> partialNodes;
        partialNodes.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                partialTail.Code,
                partialTail.MakeState(),
                NScheduler::TInputPort{.Connection = &childRef, .Lane = m},
                NScheduler::TOutputPort{.Connection = &gatherRef, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*childOut.Producers[m], node, childRef, m, m);
            partialNodes.push_back(&node);
        }

        auto combineAgg = BuildCombineAggregate(aggregate);
        auto combineTail = BuildAggregateTail(partialType, *combineAgg);
        auto finalTask = std::make_unique<NScheduler::TBlockingTask>(
            std::move(combineTail.Code),
            combineTail.MakeState(),
            NScheduler::TInputPort{.Connection = &gatherRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0});
        auto& finalNode = Graph_.AddOwnedNode(std::move(finalTask));
        for (size_t m = 0; m < lanes; ++m) {
            Graph_.AddEdge(*partialNodes[m], finalNode, gatherRef, m, 0);
        }

        return TLoweredOutput{
            .Producers = {&finalNode},
            .OutputType = std::move(combineTail.OutputType),
        };
    }

    TLoweredOutput LowerAggregate(
        TAggregateOperator& aggregate,
        NScheduler::IConnection& outConn)
    {
        const size_t childLanes = OutputLanes(aggregate.Input());
        // Non-parallel input: single aggregate, nothing to parallelize.
        if (childLanes <= 1) {
            return LowerBlocking(
                aggregate.Input(),
                [&](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(childType, aggregate);
                },
                outConn);
        }

        // Ungrouped or global (`__group__`) aggregate over a parallel input.
        // Opt-in cascade (partial -> gather -> combine) parallelizes the
        // aggregate; otherwise gather every lane into a single aggregate, which
        // is faster for cheap aggregates (the common case).
        if (aggregate.GroupKeys().empty() || IsGlobalAggregate(aggregate)) {
            if (Settings_.Aggregate.CascadeGlobal) {
                return LowerAggregateCascade(aggregate, childLanes, outConn);
            }
            return LowerBlocking(
                aggregate.Input(),
                [&](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(childType, aggregate);
                },
                outConn);
        }

        // Grouped aggregate: hash-shuffle the input by group key into `parts`
        // partition-local aggregates. Matching keys land in one partition, so
        // each computes complete groups; the parent gathers the partitions.
        const size_t parts = JoinPartitions(childLanes);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "aggregate-shuffle-input");
        auto childOut = Lower(aggregate.Input(), childConnRef);

        auto childType = childOut.OutputType;
        auto* childStruct =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!childStruct) {
            throw std::runtime_error("aggregate input must have TStructType");
        }
        auto groupHash = MakeGroupKeyHash(*childStruct, aggregate.GroupKeys());
        auto tail = BuildAggregateTail(childType, aggregate);

        auto& shufRef = AddConn<NScheduler::THashShuffleConnection>(
            childLanes, parts, "aggregate-shuffle");
        auto* shufPtr = &shufRef;

        auto hashCode = MakeHashShuffleCode(std::move(groupHash), childType);
        std::vector<NScheduler::TTaskNode*> shufNodes;
        shufNodes.reserve(childLanes);
        for (size_t i = 0; i < childLanes; ++i) {
            auto task = std::make_unique<NScheduler::THashShuffleTask>(
                hashCode,
                std::make_shared<int>(0),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = i},
                *shufPtr,
                i);
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*childOut.Producers[i], node, childConnRef, i, i);
            shufNodes.push_back(&node);
        }

        TLoweredOutput result;
        result.OutputType = tail.OutputType;
        result.Producers.reserve(parts);
        for (size_t m = 0; m < parts; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                tail.Code,
                tail.MakeState(),
                NScheduler::TInputPort{.Connection = &shufRef, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            for (size_t s = 0; s < childLanes; ++s) {
                Graph_.AddEdge(*shufNodes[s], node, shufRef, s, m);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerLimit(
        TLimitOperator& limit,
        NScheduler::IConnection& outConn)
    {
        return LowerBlocking(
            limit.Input(),
            [&](const NQumir::NAst::TTypePtr& childType) {
                const int64_t limitValue = limit.Limit();
                const int64_t offsetValue = limit.Offset();
                return TBlockingTail{
                    .Code = MakeLimitBlockingCode(),
                    .MakeState = [limitValue, offsetValue]()
                        -> std::shared_ptr<void>
                    {
                        return std::make_shared<TLimitBlockingState>(
                            limitValue, offsetValue);
                    },
                    .OutputType = childType,
                };
            },
            outConn);
    }

    TLoweredOutput LowerSortLike(
        const TOperatorPtr& input,
        const std::vector<TSortKey>& sortKeys,
        std::string_view kernelName,
        std::optional<int64_t> topLimit,
        NScheduler::IConnection& outConn)
    {
        return LowerBlocking(
            input,
            [&](const NQumir::NAst::TTypePtr& childType) {
                auto* inputType =
                    static_cast<NQumir::NAst::TStructType*>(childType.get());
                if (!inputType) {
                    throw std::runtime_error(
                        "sort input must have TStructType");
                }
                auto runtime = BuildSortRuntimeProcess(
                    *inputType, sortKeys, kernelName, Diagnostics_);
                return MakeSortTail(
                    childType, sortKeys,
                    std::move(runtime.KeyColumns),
                    std::move(runtime.RadixKernel), topLimit);
            },
            outConn);
    }

    struct TSortMergeResult {
        NScheduler::TTaskNode* Merge = nullptr;
        NQumir::NAst::TTypePtr OutputType;
    };

    // Builds `child[N] -> local-(top-)sort[N] -> merge`, writing the merged
    // stream to mergeOut. When topLimit is set each local task is a top-sort
    // keeping its local top-K; otherwise a full local sort. Assumes N > 1.
    TSortMergeResult BuildSortMerge(
        const TOperatorPtr& input,
        const std::vector<TSortKey>& sortKeys,
        std::string_view kernelName,
        std::optional<int64_t> topLimit,
        NScheduler::IConnection& mergeOut)
    {
        const size_t lanes = OutputLanes(input);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "sort-merge-input");
        auto childOut = Lower(input, childConnRef);

        auto childType = childOut.OutputType;
        auto* inputType =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!inputType) {
            throw std::runtime_error("sort input must have TStructType");
        }
        auto runtime = BuildSortRuntimeProcess(
            *inputType, sortKeys, kernelName, Diagnostics_);
        auto keys = sortKeys;
        auto keyColumns = runtime.KeyColumns;

        // Per-lane local (top-)sort producing sorted runs for the merge.
        auto localTail = MakeSortTail(
            childType, keys, keyColumns,
            std::move(runtime.RadixKernel), topLimit);

        // One local (top-)sort per input lane, each writing a sorted run.
        std::vector<NScheduler::IConnection*> runConns;
        std::vector<NScheduler::TInputPort> mergeInputs;
        std::vector<NScheduler::TTaskNode*> localSorts;
        runConns.reserve(lanes);
        mergeInputs.reserve(lanes);
        localSorts.reserve(lanes);
        for (size_t i = 0; i < lanes; ++i) {
            auto& runRef = AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "sort-run-" + std::to_string(i));
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                localTail.Code,
                localTail.MakeState(),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = i},
                NScheduler::TOutputPort{.Connection = &runRef, .Lane = 0});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*childOut.Producers[i], node, childConnRef, i, i);
            runConns.push_back(&runRef);
            mergeInputs.push_back(
                NScheduler::TInputPort{.Connection = &runRef, .Lane = 0});
            localSorts.push_back(&node);
        }

        auto mergeCode = std::make_shared<NScheduler::TMergeCode>(
            [](void* state,
               std::vector<NScheduler::TInputPort>& inputs,
               TRowSet& output)
            {
                auto* s = static_cast<TMergeState*>(state);
                if (!s->InputsDone) {
                    bool allDone = true;
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        if (s->RunFinished[i]) {
                            continue;
                        }
                        for (;;) {
                            TRowSet batch{};
                            auto fetch = inputs[i].Fetch(batch);
                            if (fetch == NScheduler::EFetchResult::NO_DATA) {
                                allDone = false;
                                break;
                            }
                            if (fetch == NScheduler::EFetchResult::FINISHED) {
                                s->RunFinished[i] = true;
                                break;
                            }
                            s->Processor.Add(batch, i);
                        }
                    }
                    if (!allDone) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    s->Processor.Finish();
                    s->InputsDone = true;
                }
                if (s->Processor.Next(output)) {
                    return NScheduler::ETaskResult::OK;
                }
                return NScheduler::ETaskResult::FINISHED;
            });

        auto merge = std::make_unique<NScheduler::TMergeTask>(
            mergeCode,
            std::make_shared<TMergeState>(childType, keys, keyColumns, lanes),
            std::move(mergeInputs),
            NScheduler::TOutputPort{.Connection = &mergeOut, .Lane = 0});
        auto& mergeNode = Graph_.AddOwnedNode(std::move(merge));
        for (size_t i = 0; i < lanes; ++i) {
            Graph_.AddEdge(*localSorts[i], mergeNode, *runConns[i], 0, 0);
        }

        return TSortMergeResult{.Merge = &mergeNode, .OutputType = childType};
    }

    // Partitioned sort: sort each input lane locally, then k-way merge the
    // sorted runs (`sort -> merge`). Falls back to a single gathered sort when
    // there is only one input lane.
    TLoweredOutput LowerSort(
        TSortOperator& sort,
        NScheduler::IConnection& outConn)
    {
        if (OutputLanes(sort.Input()) <= 1) {
            return LowerSortLike(
                sort.Input(), sort.Keys(), "sort", std::nullopt, outConn);
        }
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "sort", std::nullopt, outConn);
        return TLoweredOutput{
            .Producers = {merged.Merge},
            .OutputType = std::move(merged.OutputType),
        };
    }

    // Partitioned top-sort: local top-K per lane, k-way merge the sorted runs,
    // then a final limit (`top-sort -> merge -> limit`). Falls back to a single
    // gathered top-sort when there is only one input lane.
    TLoweredOutput LowerTopSort(
        TTopSortOperator& sort,
        NScheduler::IConnection& outConn)
    {
        if (OutputLanes(sort.Input()) <= 1) {
            return LowerSortLike(
                sort.Input(), sort.Keys(), "top-sort", sort.Limit(), outConn);
        }
        auto& mergeConnRef = AddConn<NScheduler::TOneToOneConnection>(
            1, 1, "top-sort-merge-output");
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "top-sort", sort.Limit(), mergeConnRef);

        auto limitCode = MakeLimitBlockingCode();
        const int64_t limit = sort.Limit();
        auto limitTask = std::make_unique<NScheduler::TBlockingTask>(
            limitCode,
            std::make_shared<TLimitBlockingState>(limit, 0),
            NScheduler::TInputPort{.Connection = &mergeConnRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0});
        auto& limitNode = Graph_.AddOwnedNode(std::move(limitTask));
        Graph_.AddEdge(*merged.Merge, limitNode, mergeConnRef, 0, 0);

        return TLoweredOutput{
            .Producers = {&limitNode},
            .OutputType = std::move(merged.OutputType),
        };
    }

    // Cross join with a scalar (broadcast) right side:
    //   vector[N] x Broadcast(scalar) -> local cross[N] -> filter(residual) ->
    //   parent gather. The scalar side is lowered, gathered to one lane, and
    //   forwarded into a broadcast connection replicated to every vector lane.
    TLoweredOutput LowerCrossJoin(
        TJoinOperator& join,
        NScheduler::IConnection& outConn)
    {        const size_t lanes = OutputLanes(join.Left());

        // Vector (streamed, partitioned) side.
        auto& vectorRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "cross-vector-input");
        auto vectorOut = Lower(join.Left(), vectorRef);

        // Scalar (buffered, broadcast) side: lower it (its `__group__` aggregate
        // stays single via IsGlobalAggregate — no shuffle), gather to one lane,
        // then forward into a broadcast connection replicated to every vector
        // lane.
        const size_t scalarLanes = OutputLanes(join.Right());
        auto& scalarGatherRef = AddConn<NScheduler::TGatherConnection>(
            scalarLanes, 1, "cross-scalar-gather");
        // Lower the scalar side directly into the gather so its producers write
        // the same connection the forward reads (gather scalarLanes -> 1).
        auto scalarOut = Lower(join.Right(), scalarGatherRef);

        auto& broadcastRef = AddConn<NScheduler::TBroadcastConnection>(
            1, lanes, "cross-scalar-broadcast");

        auto fwdCode = std::make_shared<NScheduler::TUnaryCode>(
            [](void*, TRowSet&) {});
        auto fwd = std::make_unique<NScheduler::TUnaryTask>(
            fwdCode,
            std::make_shared<int>(0),
            NScheduler::TInputPort{.Connection = &scalarGatherRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &broadcastRef, .Lane = 0});
        auto& fwdNode = Graph_.AddOwnedNode(std::move(fwd));
        for (size_t m = 0; m < scalarLanes; ++m) {
            Graph_.AddEdge(*scalarOut.Producers[m], fwdNode, scalarGatherRef, m, 0);
        }

        auto leftType = vectorOut.OutputType;
        auto rightType = scalarOut.OutputType;
        auto crossTypeExp =
            ComputeJoinOutputType(leftType, rightType, join.JoinType());
        if (!crossTypeExp) {
            throw std::runtime_error(
                "cross join: " + crossTypeExp.error().ToString());
        }
        auto crossType = *crossTypeExp;

        auto crossCode = MakeBinaryJoinCode<TSchedulerCrossJoinState>();

        const bool hasResidual = join.Filter() != nullptr;
        NScheduler::IConnection* crossOut = &outConn;
        NScheduler::IConnection* residualConn = nullptr;
        if (hasResidual) {
            residualConn = &AddConn<NScheduler::TOneToOneConnection>(
                lanes, lanes, "cross-residual-input");
            crossOut = residualConn;
        }

        std::vector<NScheduler::TTaskNode*> crossNodes;
        crossNodes.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                crossCode,
                std::make_shared<TSchedulerCrossJoinState>(leftType, rightType),
                NScheduler::TInputPort{.Connection = &vectorRef, .Lane = m},
                NScheduler::TInputPort{.Connection = &broadcastRef, .Lane = m},
                NScheduler::TOutputPort{.Connection = crossOut, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*vectorOut.Producers[m], node, vectorRef, m, m);
            Graph_.AddEdge(fwdNode, node, broadcastRef, 0, m);
            crossNodes.push_back(&node);
        }

        if (!hasResidual) {
            return TLoweredOutput{
                .Producers = std::move(crossNodes),
                .OutputType = std::move(crossType),
            };
        }

        // Residual predicate is a plain filter over the glued schema.
        auto filterOp =
            std::make_shared<TFilterOperator>(join.Left(), join.Filter());
        auto stage =
            BuildSchedulerFilterStage(*filterOp, crossType, Diagnostics_);
        TLoweredOutput result;
        result.OutputType = std::move(stage.OutputType);
        result.Producers.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TUnaryTask>(
                stage.Code,
                stage.MakeState(m),
                NScheduler::TInputPort{.Connection = residualConn, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*crossNodes[m], node, *residualConn, m, m);
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerJoin(
        TJoinOperator& join,
        NScheduler::IConnection& outConn)
    {
        using namespace NQumir::NAst;
        const size_t leftLanes = OutputLanes(join.Left());
        const size_t rightLanes = OutputLanes(join.Right());
        const size_t joinParts = JoinPartitions(std::max(leftLanes, rightLanes));
        auto& leftPipeRef = AddConn<NScheduler::TOneToOneConnection>(
            leftLanes, leftLanes, "join-left-input");
        auto leftOut = Lower(join.Left(), leftPipeRef);

        auto& rightPipeRef = AddConn<NScheduler::TOneToOneConnection>(
            rightLanes, rightLanes, "join-right-input");
        auto rightOut = Lower(join.Right(), rightPipeRef);

        auto leftType = leftOut.OutputType;
        auto rightType = rightOut.OutputType;
        auto* leftStruct = static_cast<TStructType*>(leftType.get());
        auto* rightStruct = static_cast<TStructType*>(rightType.get());
        if (!leftStruct || !rightStruct) {
            throw std::runtime_error("scheduler join inputs must have TStructType");
        }

        auto kernelSpec = NKernel::BuildJoinKernelSpec(
            *leftStruct, *rightStruct, join.Keys(), join.JoinType(), join.Filter());
        TKernelCompiler compiler(Diagnostics_);
        auto joinKernels = std::make_shared<TJoinKernels>(
            compiler.CompileJoin(kernelSpec));
        auto hashKernels = compiler.CompileJoinHash(kernelSpec);

        auto leftShuf = BuildShuffleNodes(
            leftOut,
            leftPipeRef,
            leftLanes,
            joinParts,
            MakeHashShuffleCode(hashKernels.Left, leftType),
            "join-left-shuffle");
        auto rightShuf = BuildShuffleNodes(
            rightOut,
            rightPipeRef,
            rightLanes,
            joinParts,
            MakeHashShuffleCode(hashKernels.Right, rightType),
            "join-right-shuffle");

        auto joinCode = MakeBinaryJoinCode<TSchedulerInnerJoinState>();

        TLoweredOutput result;
        result.OutputType = kernelSpec.OutputSchema;
        result.Producers.reserve(joinParts);
        for (size_t j = 0; j < joinParts; ++j) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                joinCode,
                std::make_shared<TSchedulerInnerJoinState>(
                    leftType, rightType, *joinKernels, join.JoinType(),
                    join.Filter() != nullptr),
                NScheduler::TInputPort{
                    .Connection = leftShuf.Connection, .Lane = j},
                NScheduler::TInputPort{
                    .Connection = rightShuf.Connection, .Lane = j},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = j});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            for (size_t s = 0; s < leftLanes; ++s) {
                Graph_.AddEdge(*leftShuf.Nodes[s], node, *leftShuf.Connection, s, j);
            }
            for (size_t s = 0; s < rightLanes; ++s) {
                Graph_.AddEdge(
                    *rightShuf.Nodes[s], node, *rightShuf.Connection, s, j);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TShuffleSide BuildShuffleNodes(
        const TLoweredOutput& input,
        NScheduler::IConnection& inputConn,
        size_t inputLanes,
        size_t joinParts,
        std::shared_ptr<NScheduler::THashShuffleCode> hashCode,
        std::string debugName)
    {
        auto* shufPtr = &AddConn<NScheduler::THashShuffleConnection>(
            inputLanes, joinParts, std::move(debugName), ShuffleQueueCapacity());

        TShuffleSide side;
        side.Connection = shufPtr;
        side.Nodes.reserve(inputLanes);
        for (size_t p = 0; p < inputLanes; ++p) {
            auto task = std::make_unique<NScheduler::THashShuffleTask>(
                hashCode,
                std::make_shared<int>(0),
                NScheduler::TInputPort{.Connection = &inputConn, .Lane = p},
                *shufPtr,
                p);
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*input.Producers[p], node, inputConn, p, p);
            side.Nodes.push_back(&node);
        }
        return side;
    }

    std::shared_ptr<NScheduler::THashShuffleCode> MakeHashShuffleCode(
        NScheduler::THashShuffleCode::THash hash,
        NQumir::NAst::TTypePtr inputType) const
    {
        return std::make_shared<NScheduler::THashShuffleCode>(
            std::move(hash),
            std::move(inputType),
            Settings_.HashShuffle.TargetOutputBatchRows,
            Settings_.HashShuffle.MaxOutputBatchRows,
            Settings_.HashShuffle.TargetOutputBatchBytes);
    }

private:
    NScheduler::TTaskGraph& Graph_;
    NScheduler::TSettings Settings_;
    std::ostream* Diagnostics_;
};

} // namespace

namespace NScheduler {

namespace {

std::shared_ptr<TSinkCode> MakeSinkWriterCode() {
    return std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            static_cast<ISink*>(state)->Write(rowSet);
        });
}

void PrintGraphDiagnostics(
    std::ostream& out, const TTaskGraph& graph, const TSettings& settings)
{
    out << "\n========== SCHEDULER GRAPH ==========\n";
    out << "mode=" << static_cast<int>(settings.Scheduler.Mode)
        << " workers=" << settings.Scheduler.WorkerCount
        << " ready_queue=" << settings.Scheduler.ReadyQueueCapacity
        << " rowset_queue=" << settings.Queue.RowsetCapacityPerLane
        << " scan_tasks=" << settings.ScanSplit.MaxScanTasks;
    if (settings.HashShuffle.PartitionCount == 0) {
        out << " shuffle_parts=auto";
    } else {
        out << " shuffle_parts=" << settings.HashShuffle.PartitionCount;
    }
    out << " shuffle_queue=" << settings.HashShuffle.MaxQueuedRowsetsPerLane
        << " shuffle_target_rows=" << settings.HashShuffle.TargetOutputBatchRows
        << " shuffle_max_rows=" << settings.HashShuffle.MaxOutputBatchRows
        << " shuffle_target_bytes=" << settings.HashShuffle.TargetOutputBatchBytes
        << "\n";
    graph.Print(out);
    out << "=====================================\n";
}

void PrintCounterDiagnostics(
    std::ostream& out, const TTaskGraph& graph, const TSchedulerRunStats& stats)
{
    out << "\n========== SCHEDULER COUNTERS ==========\n"
        << "scheduled=" << stats.Scheduled
        << " popped=" << stats.Popped
        << " executed=" << stats.Executed
        << " ok=" << stats.Ok
        << " need_data=" << stats.NeedData
        << " blocked_output=" << stats.BlockedOutput
        << " finished=" << stats.Finished
        << " rescheduled=" << stats.Rescheduled
        << " ready_push_retries=" << stats.ReadyPushRetries
        << " empty_ready_polls=" << stats.EmptyReadyPolls
        << "\n";
    graph.PrintConnectionStats(out);
    out << "========================================\n";
}

} // namespace

TLoweredPlan LowerPlanToGraph(
    const TOperatorPtr& root,
    TSettings settings,
    std::ostream* diagnostics)
{
    auto graph = std::make_unique<TTaskGraph>();
    TSchedulerGraphLowerer lowerer(*graph, settings, diagnostics);
    const size_t lanes = lowerer.OutputLanes(root);
    if (lanes == 0) {
        throw std::runtime_error(
            "scheduler lowering produced no output lanes for the plan");
    }

    auto gather = std::make_unique<TGatherConnection>(lowerer.QueueCapacity());
    gather->Resize(lanes, 1);
    gather->SetDebugName("final-gather");
    auto& gatherRef = graph->AddConnection(std::move(gather));
    auto out = lowerer.Lower(root, gatherRef);

    return TLoweredPlan{
        .Graph = std::move(graph),
        .OutputType = std::move(out.OutputType),
        .FinalGather = &gatherRef,
        .Producers = std::move(out.Producers),
        .Lanes = lanes,
    };
}

bool RunPlanIntoSink(
    TLoweredPlan lowered,
    ISink& sink,
    TSettings settings,
    std::ostream* diagnostics,
    std::string* error)
{
    // Aliasing shared_ptr: the sink task's state points at the caller-owned
    // sink without taking ownership of it.
    std::shared_ptr<void> sinkState(std::shared_ptr<void>{}, &sink);
    auto sinkTask = std::make_unique<TSinkTask>(
        MakeSinkWriterCode(),
        std::move(sinkState),
        TInputPort{.Connection = lowered.FinalGather, .Lane = 0});
    auto& sinkNode = lowered.Graph->AddOwnedNode(std::move(sinkTask));
    for (size_t p = 0; p < lowered.Lanes; ++p) {
        lowered.Graph->AddEdge(
            *lowered.Producers[p], sinkNode, *lowered.FinalGather, p, 0);
    }

    lowered.Graph->Build();
    if (!lowered.Graph->Validate(error)) {
        return false;
    }
    lowered.Graph->SetConnectionStatsEnabled(settings.Queue.EnableDebugCounters);
    if (diagnostics) {
        PrintGraphDiagnostics(*diagnostics, *lowered.Graph, settings);
    }

    TSchedulerExecutor executor(*lowered.Graph, settings);
    if (!executor.Run(error)) {
        return false;
    }
    if (settings.Queue.EnableDebugCounters && diagnostics) {
        PrintCounterDiagnostics(*diagnostics, *lowered.Graph, executor.Stats());
    }
    return true;
}

} // namespace NScheduler
} // namespace NQdb

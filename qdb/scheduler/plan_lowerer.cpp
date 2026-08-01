#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/window_exec.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/executor.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/scan_split.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

struct TGroupingSetsBlockingState {
    TGroupingSetsBlockingState(
        TAggregateKernels kernels, std::vector<std::vector<size_t>> sets, size_t numKeys)
        : Processor(std::move(kernels), std::move(sets), numKeys)
    {}

    TGroupingSetsAggregateProcessor Processor;
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

struct TWindowBlockingState {
    TWindowBlockingState(
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

    TWindowProcessor Processor;
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
        TJoinKernels kernels,
        EJoinType joinType)
        : Processor(std::move(kernels), joinType)
    {}

    TInnerJoinProcessor Processor;
};

struct TSchedulerCrossJoinState {
    explicit TSchedulerCrossJoinState(TCrossJoinKernels kernels)
        : Processor(std::move(kernels))
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

class TStageDiagnosticsScope {
public:
    TStageDiagnosticsScope(std::ostream* out, const std::string& stage)
        : Out_(out)
    {
        if (Out_) {
            *Out_ << "\n========== RUNTIME STAGE: " << stage << " ==========\n";
        }
    }

    ~TStageDiagnosticsScope() {
        if (Out_) {
            *Out_ << "========== END RUNTIME STAGE ==========\n";
        }
    }

private:
    std::ostream* Out_;
};

TSchedulerUnaryStage BuildSchedulerFilterStage(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    auto runtime = BuildFilterRuntimeProcess(
        filter, inputType, std::move(options));
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
    TKernelCompilerOptions options)
{
    auto runtime = BuildProjectRuntimeProcess(
        project, inputType, std::move(options));
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
        auto valueType = UnwrapNullableType(fieldType);
        if (IsDecimalType(valueType) || IsBinIntStorageType(valueType)) {
            kc.Width = 16;
        } else if (auto inner = UnwrapNamedType(valueType);
            TMaybeType<TStringType>(inner)) {
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
        std::ostream* diagnostics,
        std::vector<TGeneratedKernel>* kernelSink)
        : Graph_(graph)
        , Settings_(std::move(settings))
        , Diagnostics_(diagnostics)
        , KernelSink_(kernelSink)
    {}

    // Kernel options for one stage: generation only (deferred finalization),
    // kernels collected into the lowered plan.
    TKernelCompilerOptions KernelOptions(
        std::string stage, const void* op) const
    {
        return TKernelCompilerOptions{
            .Diagnostics = Diagnostics_,
            .Stage = std::move(stage),
            .Operator = op,
            .Sink = KernelSink_,
            .BindNow = false,
        };
    }

    size_t OutputLanes(const TOperatorPtr& op) const {
        if (auto n = TMaybeOp<TSourceOperator>(op)) {
            return std::max<size_t>(ScanSplits(*n.Cast()).size(), 1);
        }
        if (TMaybeOp<TCteConsumer>(op)) {
            return 1;
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
            // Grouping sets are not shuffled yet (the shuffle key would need the
            // set id) — gather to a single lane.
            if (!IsGlobalAggregate(*n.Cast()) && n.Cast()->GroupingSets().empty() &&
                !n.Cast()->GroupKeys().empty() && childLanes > 1)
            {
                return JoinPartitions(childLanes);
            }
            return 1;
        }
        if (auto n = TMaybeOp<TWindowOperator>(op)) {
            const size_t childLanes = OutputLanes(n.Cast()->Input());
            if (childLanes == 0) {
                return 0;
            }
            if (!n.Cast()->PartitionKeys().empty() && childLanes > 1) {
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
            // Inner, left/right/full outer, and left semi/anti equi-joins are
            // hash-partitionable (matches co-locate by key).
            const bool supportedType =
                join->JoinType() == EJoinType::Inner ||
                join->JoinType() == EJoinType::Left ||
                join->JoinType() == EJoinType::Right ||
                join->JoinType() == EJoinType::Full ||
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
        if (auto n = TMaybeOp<TUnionAllOperator>(op)) {
            size_t total = 0;
            for (const auto& branch : n.Cast()->Inputs()) {
                size_t branchLanes = OutputLanes(branch);
                if (branchLanes == 0) {
                    return 0;
                }
                total += branchLanes;
            }
            return total;
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

    // outLaneOffset shifts this operator's producer lanes within outConn, so a
    // UNION ALL can fan several branches into one gather without lane collisions.
    TLoweredOutput Lower(
        const TOperatorPtr& op,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset = 0)
    {
        if (auto n = TMaybeOp<TSourceOperator>(op)) {
            return LowerSource(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TCteConsumer>(op)) {
            return LowerCteConsumer(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TFilterOperator>(op)) {
            auto filter = n.Cast();
            auto stageGroup = StageGroup("filter", filter.get());
            return LowerUnary(
                filter->Input(),
                "filter",
                stageGroup,
                [&](const NQumir::NAst::TTypePtr& inType) {
                    return BuildSchedulerFilterStage(
                        *filter,
                        inType,
                        KernelOptions(stageGroup, filter.get()));
                },
                outConn,
                outLaneOffset);
        }
        if (auto n = TMaybeOp<TProjectOperator>(op)) {
            auto project = n.Cast();
            auto stageGroup = StageGroup("project", project.get());
            return LowerUnary(
                project->Input(),
                "project",
                stageGroup,
                [&](const NQumir::NAst::TTypePtr& inType) {
                    return BuildSchedulerProjectStage(
                        *project,
                        inType,
                        KernelOptions(stageGroup, project.get()));
                },
                outConn,
                outLaneOffset);
        }
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            return LowerAggregate(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TWindowOperator>(op)) {
            return LowerWindow(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TLimitOperator>(op)) {
            return LowerLimit(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TSortOperator>(op)) {
            return LowerSort(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TTopSortOperator>(op)) {
            return LowerTopSort(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TJoinOperator>(op)) {
            if (n.Cast()->Keys().empty()) {
                return LowerCrossJoin(*n.Cast(), outConn, outLaneOffset);
            }
            return LowerJoin(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TUnionAllOperator>(op)) {
            return LowerUnionAll(*n.Cast(), outConn, outLaneOffset);
        }
        throw std::runtime_error("scheduler lowering: unsupported operator");
    }

    // UNION ALL is a no-op at runtime: lower every branch straight into outConn,
    // giving each branch a contiguous lane range so the gather sees all rows.
    TLoweredOutput LowerUnionAll(
        TUnionAllOperator& un,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        TLoweredOutput result;
        size_t offset = outLaneOffset;
        for (const auto& branch : un.Inputs()) {
            auto branchOut = Lower(branch, outConn, offset);
            if (result.Producers.empty()) {
                result.OutputType = branchOut.OutputType;
            }
            for (auto* producer : branchOut.Producers) {
                result.Producers.push_back(producer);
            }
            offset += branchOut.Producers.size();
        }
        return result;
    }

    size_t QueueCapacity() const {
        return std::max<size_t>(Settings_.Queue.RowsetCapacityPerLane, 1);
    }

    void AssertMaterializationsWired() const {
        for (const auto& entry : Materialized_) {
            assert(entry.second.NextConsumerLane == entry.first->RefCount);
            (void)entry;
        }
    }

private:
    static std::string StageGroup(std::string_view kind, const void* owner) {
        return std::string(kind) + ":" +
            std::to_string(reinterpret_cast<uintptr_t>(owner));
    }

    static void MarkNode(
        NScheduler::TTaskNode& node,
        std::string kind,
        std::string group,
        std::string label)
    {
        node.DebugKind = std::move(kind);
        node.DebugGroup = std::move(group);
        node.DebugLabel = std::move(label);
    }

    static std::string JoinDebugLabel(const TJoinOperator& join) {
        if (join.Keys().empty()) {
            return "cross-join " + std::string(JoinTypeName(join.JoinType()));
        }
        std::string label = "join " + std::string(JoinTypeName(join.JoinType()));
        const auto& keys = join.Keys();
        for (size_t i = 0; i < keys.size(); ++i) {
            label += (i ? ", " : " [") + keys[i].Left + " = " + keys[i].Right;
        }
        label += "]";
        if (join.Filter()) {
            label += " residual";
        }
        return label;
    }

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
        if (auto n = TMaybeOp<TWindowOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        return false;
    }

    std::vector<NScheduler::TScanSplit> ScanSplits(TSourceOperator& src) const {
        auto* metadata =
            dynamic_cast<NScheduler::IScanMetadataSource*>(&src.GetSource());
        if (!metadata) {
            return {};
        }
        return NScheduler::BuildScanSplits(
            metadata->ScanRowGroups(),
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
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
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
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "source",
                StageGroup("source", &src),
                src.GetAlias().empty() ? "source" : "source " + src.GetAlias());
            result.Producers.push_back(&node);
        }
        return result;
    }

    struct TMaterializedProducer {
        std::shared_ptr<NScheduler::TCteSharedState> State;
        NScheduler::TTaskNode* Producer = nullptr;
        NScheduler::IConnection* Completion = nullptr;
        size_t NextConsumerLane = 0;
        NQumir::NAst::TTypePtr OutputType;
    };

    TLoweredOutput LowerCteConsumer(
        TCteConsumer& consumer,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const auto* mat = consumer.Materialization().get();
        auto it = Materialized_.find(mat);
        if (it == Materialized_.end()) {
            it = Materialized_.emplace(
                mat, BuildMaterializedProducer(consumer)).first;
        }
        auto& producer = it->second;

        const size_t lane = producer.NextConsumerLane++;
        assert(lane < consumer.Materialization()->RefCount);
        auto task = std::make_unique<NScheduler::TCteConsumerTask>(
            producer.State,
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = outLaneOffset});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        MarkNode(node,
            "cte-consumer",
            StageGroup("cte-consumer", mat),
            "cte-consumer " + std::to_string(consumer.Def()->Id));
        Graph_.AddEdge(*producer.Producer, node, *producer.Completion, 0, lane);
        return TLoweredOutput{
            .Producers = {&node},
            .OutputType = producer.OutputType,
        };
    }

    TMaterializedProducer BuildMaterializedProducer(TCteConsumer& consumer) {
        const auto& materialization = consumer.Materialization();
        const size_t lanes = OutputLanes(materialization->Plan);
        NScheduler::IConnection* inputConn = nullptr;
        if (lanes == 1) {
            inputConn = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "cte-producer-input");
        } else {
            inputConn = &AddConn<NScheduler::TGatherConnection>(
                lanes, 1, "cte-producer-gather");
        }
        auto planOut = Lower(materialization->Plan, *inputConn);

        auto state = std::make_shared<NScheduler::TCteSharedState>();
        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](void* st, NScheduler::TInputPort& input, TRowSet&) {
                auto* shared = static_cast<NScheduler::TCteSharedState*>(st);
                for (;;) {
                    TRowSet batch{};
                    auto fetch = input.Fetch(batch);
                    if (fetch == NScheduler::EFetchResult::OK) {
                        shared->Batches.push_back(batch);
                        continue;
                    }
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    shared->Status.store(
                        NScheduler::TCteSharedState::EStatus::Ready,
                        std::memory_order_release);
                    return NScheduler::ETaskResult::FINISHED;
                }
            });
        assert(materialization->RefCount > 0);
        auto& completion = AddConn<NScheduler::TBroadcastConnection>(
            1, materialization->RefCount, "cte-completion");
        auto task = std::make_unique<NScheduler::TBlockingTask>(
            code,
            state,
            NScheduler::TInputPort{.Connection = inputConn, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &completion, .Lane = 0});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        MarkNode(node,
            "cte-producer",
            StageGroup("cte-producer", materialization.get()),
            "cte-producer " + std::to_string(consumer.Def()->Id));
        for (size_t p = 0; p < lanes; ++p) {
            Graph_.AddEdge(*planOut.Producers[p], node, *inputConn, p, 0);
        }
        return TMaterializedProducer{
            .State = std::move(state),
            .Producer = &node,
            .Completion = &completion,
            .NextConsumerLane = 0,
            .OutputType = std::move(planOut.OutputType),
        };
    }

    template <typename TStageBuilder>
    TLoweredOutput LowerUnary(
        const TOperatorPtr& child,
        std::string stageKind,
        std::string stageGroup,
        TStageBuilder buildStage,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t lanes = OutputLanes(child);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "unary-input");
        auto childOut = Lower(child, childConnRef);

        TStageDiagnosticsScope diagnosticsScope(Diagnostics_, stageGroup);
        auto stage = buildStage(childOut.OutputType);
        TLoweredOutput result;
        result.OutputType = std::move(stage.OutputType);
        result.Producers.reserve(lanes);
        for (size_t p = 0; p < lanes; ++p) {
            auto task = std::make_unique<NScheduler::TUnaryTask>(
                stage.Code,
                stage.MakeState(p),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = p},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node, stageKind, stageGroup, stageKind);
            Graph_.AddEdge(*childOut.Producers[p], node, childConnRef, p, p);
            result.Producers.push_back(&node);
        }
        return result;
    }

    template <typename TTailBuilder>
    TLoweredOutput LowerBlocking(
        const TOperatorPtr& child,
        std::string stageKind,
        std::string stageGroup,
        TTailBuilder buildTail,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t lanes = OutputLanes(child);
        NScheduler::IConnection* inputConn = nullptr;
        if (lanes == 1) {
            inputConn = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "blocking-input");
        } else {
            inputConn = &AddConn<NScheduler::TGatherConnection>(
                lanes, 1, "blocking-input-gather");
        }
        auto childOut = Lower(child, *inputConn);

        TStageDiagnosticsScope diagnosticsScope(Diagnostics_, stageGroup);
        TBlockingTail tail = buildTail(childOut.OutputType);
        auto task = std::make_unique<NScheduler::TBlockingTask>(
            std::move(tail.Code),
            tail.MakeState(),
            NScheduler::TInputPort{.Connection = inputConn, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        MarkNode(node, stageKind, stageGroup, stageKind);
        for (size_t p = 0; p < lanes; ++p) {
            Graph_.AddEdge(*childOut.Producers[p], node, *inputConn, p, 0);
        }
        return TLoweredOutput{
            .Producers = {&node},
            .OutputType = std::move(tail.OutputType),
        };
    }

    // Builds the aggregate code + partition-local state factory + output type
    // for a given (already-lowered) input type. Kernels are compiled once and
    // shared across all partition states.
    TBlockingTail BuildGroupingSetsAggregateTail(
        const NQumir::NAst::TTypePtr& childType,
        const NQumir::NAst::TStructType& inputType,
        TAggregateOperator& aggregate,
        std::string stage)
    {
        using namespace NQumir::NAst;
        const size_t numKeys = aggregate.GroupKeys().size();

        // Kernel input = leading i32 __grouping_id__ ++ nullable keys ++ args; the
        // masked batches the driver feeds match this layout column-for-column.
        std::vector<std::pair<std::string, TTypePtr>> extFields;
        extFields.emplace_back("__grouping_id__", std::make_shared<TIntegerType>(TIntegerType::I32));
        for (size_t i = 0; i < inputType.Fields.size(); ++i) {
            auto [name, type] = inputType.Fields[i];
            if (i < numKeys && !IsNullableType(type)) {
                type = std::make_shared<TNullable>(type);
            }
            extFields.emplace_back(name, std::move(type));
        }
        auto extInput = std::make_shared<TStructType>(std::move(extFields));

        std::vector<std::string> extKeys;
        extKeys.reserve(numKeys + 1);
        extKeys.push_back("__grouping_id__");
        for (const auto& key : aggregate.GroupKeys()) {
            extKeys.push_back(key);
        }

        auto spec = NKernel::BuildAggregateKernelSpec(*extInput, extKeys, aggregate.Aggs());
        TKernelCompiler compiler(KernelOptions(std::move(stage), &aggregate));
        auto kernels = std::make_shared<TAggregateKernels>(compiler.CompileAggregate(spec));
        auto sets = aggregate.GroupingSets();

        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = static_cast<TGroupingSetsBlockingState*>(state);
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
            .MakeState = [kernels, sets, numKeys]() -> std::shared_ptr<void> {
                return std::make_shared<TGroupingSetsBlockingState>(*kernels, sets, numKeys);
            },
            .OutputType = ComputeAggregateOutputType(
                childType, aggregate.GroupKeys(), aggregate.Aggs(), true),
        };
    }

    TBlockingTail BuildAggregateTail(
        const NQumir::NAst::TTypePtr& childType,
        TAggregateOperator& aggregate,
        std::string stage)
    {
        auto* inputType =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!inputType) {
            throw std::runtime_error("aggregate input must have TStructType");
        }
        if (!aggregate.GroupingSets().empty()) {
            return BuildGroupingSetsAggregateTail(childType, *inputType, aggregate, std::move(stage));
        }
        auto spec = NKernel::BuildAggregateKernelSpec(
            *inputType, aggregate.GroupKeys(), aggregate.Aggs());
        TKernelCompiler compiler(KernelOptions(std::move(stage), &aggregate));
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
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        auto& childRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "aggregate-cascade-input");
        auto childOut = Lower(aggregate.Input(), childRef);

        const auto partialGroup = StageGroup("aggregate-partial", &aggregate);
        TBlockingTail partialTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, partialGroup);
                return BuildAggregateTail(childOut.OutputType, aggregate, partialGroup);
            }();
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
            MarkNode(node,
                "aggregate",
                partialGroup,
                "aggregate partial");
            Graph_.AddEdge(*childOut.Producers[m], node, childRef, m, m);
            partialNodes.push_back(&node);
        }

        auto combineAgg = BuildCombineAggregate(aggregate);
        const auto combineGroup = StageGroup("aggregate-combine", &aggregate);
        TBlockingTail combineTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, combineGroup);
                return BuildAggregateTail(partialType, *combineAgg, combineGroup);
            }();
        auto finalTask = std::make_unique<NScheduler::TBlockingTask>(
            std::move(combineTail.Code),
            combineTail.MakeState(),
            NScheduler::TInputPort{.Connection = &gatherRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
        auto& finalNode = Graph_.AddOwnedNode(std::move(finalTask));
        MarkNode(finalNode,
            "aggregate",
            combineGroup,
            "aggregate combine");
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
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t childLanes = OutputLanes(aggregate.Input());
        // Non-parallel input: single aggregate, nothing to parallelize.
        if (childLanes <= 1) {
            auto group = StageGroup("aggregate", &aggregate);
            return LowerBlocking(
                aggregate.Input(),
                "aggregate",
                group,
                [&, group](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(childType, aggregate, group);
                },
                outConn,
                outLaneOffset);
        }

        // Grouping sets: gather to one lane and run a single grouping-sets pass
        // (no shuffle/cascade yet).
        if (!aggregate.GroupingSets().empty()) {
            auto group = StageGroup("aggregate", &aggregate);
            return LowerBlocking(
                aggregate.Input(),
                "aggregate",
                group,
                [&, group](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(childType, aggregate, group);
                },
                outConn,
                outLaneOffset);
        }

        // Ungrouped or global (`__group__`) aggregate over a parallel input.
        // Opt-in cascade (partial -> gather -> combine) parallelizes the
        // aggregate; otherwise gather every lane into a single aggregate, which
        // is faster for cheap aggregates (the common case).
        if (aggregate.GroupKeys().empty() || IsGlobalAggregate(aggregate)) {
            if (Settings_.Aggregate.CascadeGlobal) {
                return LowerAggregateCascade(aggregate, childLanes, outConn, outLaneOffset);
            }
            auto group = StageGroup("aggregate", &aggregate);
            return LowerBlocking(
                aggregate.Input(),
                "aggregate",
                group,
                [&, group](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(childType, aggregate, group);
                },
                outConn,
                outLaneOffset);
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
        const auto aggregateGroup = StageGroup("aggregate", &aggregate);
        TBlockingTail tail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, aggregateGroup);
                return BuildAggregateTail(childType, aggregate, aggregateGroup);
            }();

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
            MarkNode(node,
                "hash-shuffle",
                StageGroup("aggregate-shuffle", &aggregate),
                "hash-shuffle");
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
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "aggregate",
                aggregateGroup,
                "aggregate");
            for (size_t s = 0; s < childLanes; ++s) {
                Graph_.AddEdge(*shufNodes[s], node, shufRef, s, m);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerLimit(
        TLimitOperator& limit,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        return LowerBlocking(
            limit.Input(),
            "limit",
            StageGroup("limit", &limit),
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
            outConn,
            outLaneOffset);
    }

    TLoweredOutput LowerSortLike(
        const TOperatorPtr& input,
        const std::vector<TSortKey>& sortKeys,
        std::string_view kernelName,
        std::optional<int64_t> topLimit,
        const void* sortOp,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        auto group = StageGroup(kernelName, input.get());
        return LowerBlocking(
            input,
            std::string(kernelName),
            group,
            [&, group](const NQumir::NAst::TTypePtr& childType) {
                auto* inputType =
                    static_cast<NQumir::NAst::TStructType*>(childType.get());
                if (!inputType) {
                    throw std::runtime_error(
                        "sort input must have TStructType");
                }
                auto runtime = BuildSortRuntimeProcess(
                    *inputType, sortKeys, kernelName,
                    KernelOptions(group, sortOp));
                return MakeSortTail(
                    childType, sortKeys,
                    std::move(runtime.KeyColumns),
                    std::move(runtime.RadixKernel), topLimit);
            },
            outConn,
            outLaneOffset);
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
        const void* sortOp,
        NScheduler::IConnection& mergeOut,
        size_t mergeOutLaneOffset)
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
        auto localSortGroup = StageGroup(kernelName, input.get());
        TSortRuntimeProcess runtime =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, localSortGroup);
                return BuildSortRuntimeProcess(
                    *inputType, sortKeys, kernelName,
                    KernelOptions(localSortGroup, sortOp));
            }();
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
            MarkNode(node,
                std::string(kernelName),
                localSortGroup,
                std::string(kernelName));
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
            NScheduler::TOutputPort{.Connection = &mergeOut, .Lane = 0 + mergeOutLaneOffset});
        auto& mergeNode = Graph_.AddOwnedNode(std::move(merge));
        MarkNode(mergeNode,
            "merge",
            StageGroup("merge", input.get()),
            "merge");
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
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        if (OutputLanes(sort.Input()) <= 1) {
            return LowerSortLike(
                sort.Input(), sort.Keys(), "sort", std::nullopt, &sort, outConn, outLaneOffset);
        }
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "sort", std::nullopt, &sort, outConn, outLaneOffset);
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
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        if (OutputLanes(sort.Input()) <= 1) {
            return LowerSortLike(
                sort.Input(), sort.Keys(), "top-sort", sort.Limit(), &sort, outConn, outLaneOffset);
        }
        auto& mergeConnRef = AddConn<NScheduler::TOneToOneConnection>(
            1, 1, "top-sort-merge-output");
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "top-sort", sort.Limit(), &sort, mergeConnRef, 0);

        auto limitCode = MakeLimitBlockingCode();
        const int64_t limit = sort.Limit();
        auto limitTask = std::make_unique<NScheduler::TBlockingTask>(
            limitCode,
            std::make_shared<TLimitBlockingState>(limit, 0),
            NScheduler::TInputPort{.Connection = &mergeConnRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
        auto& limitNode = Graph_.AddOwnedNode(std::move(limitTask));
        MarkNode(limitNode,
            "limit",
            StageGroup("top-sort-limit", &sort),
            "limit");
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
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {        const size_t lanes = OutputLanes(join.Left());

        // Vector (streamed, partitioned) side.
        auto& vectorRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "cross-vector-input");
        auto vectorOut = Lower(join.Left(), vectorRef);

        const size_t scalarLanes = OutputLanes(join.Right());
        TLoweredOutput scalarOut;
        NScheduler::IConnection* scalarInput = nullptr;
        NScheduler::TTaskNode* scalarBroadcastProducer = nullptr;
        const bool directScalar = lanes == 1 && scalarLanes == 1;
        if (directScalar) {
            scalarInput = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "cross-right-input");
            scalarOut = Lower(join.Right(), *scalarInput);
        } else {
            // Scalar (buffered, broadcast) side: lower it, gather to one lane,
            // then forward into a broadcast connection replicated to every
            // vector lane.
            auto& scalarGatherRef = AddConn<NScheduler::TGatherConnection>(
                scalarLanes, 1, "cross-scalar-gather");
            scalarOut = Lower(join.Right(), scalarGatherRef);

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
            MarkNode(fwdNode,
                "broadcast",
                StageGroup("cross-scalar-broadcast", &join),
                "broadcast");
            for (size_t m = 0; m < scalarLanes; ++m) {
                Graph_.AddEdge(
                    *scalarOut.Producers[m], fwdNode, scalarGatherRef, m, 0);
            }
            scalarInput = &broadcastRef;
            scalarBroadcastProducer = &fwdNode;
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

        auto* leftStruct = static_cast<NQumir::NAst::TStructType*>(leftType.get());
        auto* rightStruct = static_cast<NQumir::NAst::TStructType*>(rightType.get());
        if (!leftStruct || !rightStruct) {
            throw std::runtime_error("scheduler cross join inputs must have TStructType");
        }
        auto kernelSpec = NKernel::BuildCrossJoinKernelSpec(*leftStruct, *rightStruct);
        const auto crossGroup = StageGroup("join", &join);
        TKernelCompiler compiler(KernelOptions(crossGroup, &join));
        auto crossKernels = std::make_shared<TCrossJoinKernels>(
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, crossGroup);
                return compiler.CompileCrossJoin(kernelSpec);
            }());
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
                std::make_shared<TSchedulerCrossJoinState>(*crossKernels),
                NScheduler::TInputPort{.Connection = &vectorRef, .Lane = m},
                NScheduler::TInputPort{.Connection = scalarInput, .Lane = m},
                NScheduler::TOutputPort{.Connection = crossOut, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "join",
                StageGroup("join", &join),
                JoinDebugLabel(join));
            Graph_.AddEdge(*vectorOut.Producers[m], node, vectorRef, m, m);
            if (directScalar) {
                Graph_.AddEdge(*scalarOut.Producers[0], node, *scalarInput, 0, 0);
            } else {
                Graph_.AddEdge(
                    *scalarBroadcastProducer, node, *scalarInput, 0, m);
            }
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
        const auto residualGroup = StageGroup("cross-residual-filter", &join);
        TSchedulerUnaryStage stage =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, residualGroup);
                // The residual filter kernel belongs to the join operator: the
                // exec exporter looks it up by the join's identity.
                return BuildSchedulerFilterStage(
                    *filterOp, crossType,
                    KernelOptions(residualGroup, &join));
            }();
        TLoweredOutput result;
        result.OutputType = std::move(stage.OutputType);
        result.Producers.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TUnaryTask>(
                stage.Code,
                stage.MakeState(m),
                NScheduler::TInputPort{.Connection = residualConn, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "filter",
                residualGroup,
                "filter");
            Graph_.AddEdge(*crossNodes[m], node, *residualConn, m, m);
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerJoin(
        TJoinOperator& join,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
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
        const auto joinGroup = StageGroup("join", &join);
        TKernelCompiler compiler(KernelOptions(joinGroup, &join));
        auto joinKernels = std::make_shared<TJoinKernels>(
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, joinGroup);
                return compiler.CompileJoin(kernelSpec);
            }());
        auto joinCode = MakeBinaryJoinCode<TSchedulerInnerJoinState>();

        if (leftLanes == 1 && rightLanes == 1 && joinParts == 1) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                joinCode,
                std::make_shared<TSchedulerInnerJoinState>(
                    *joinKernels, join.JoinType()),
                NScheduler::TInputPort{
                    .Connection = &leftPipeRef, .Lane = 0},
                NScheduler::TInputPort{
                    .Connection = &rightPipeRef, .Lane = 0},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "join",
                joinGroup,
                JoinDebugLabel(join));
            Graph_.AddEdge(*leftOut.Producers[0], node, leftPipeRef, 0, 0);
            Graph_.AddEdge(*rightOut.Producers[0], node, rightPipeRef, 0, 0);
            return TLoweredOutput{
                .Producers = {&node},
                .OutputType = kernelSpec.OutputSchema,
            };
        }

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

        TLoweredOutput result;
        result.OutputType = kernelSpec.OutputSchema;
        result.Producers.reserve(joinParts);
        for (size_t j = 0; j < joinParts; ++j) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                joinCode,
                std::make_shared<TSchedulerInnerJoinState>(
                    *joinKernels, join.JoinType()),
                NScheduler::TInputPort{
                    .Connection = leftShuf.Connection, .Lane = j},
                NScheduler::TInputPort{
                    .Connection = rightShuf.Connection, .Lane = j},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = j + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "join",
                joinGroup,
                JoinDebugLabel(join));
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

    TBlockingTail BuildWindowTail(
        const NQumir::NAst::TTypePtr& childType,
        TWindowOperator& window,
        std::string stage)
    {
        TKernelCompilerOptions options = KernelOptions(std::move(stage), &window);
        auto runtime = BuildWindowRuntimeProcess(window, childType, std::move(options));
        return TBlockingTail{
            .Code = MakeSortBlockingCode<TWindowBlockingState>(),
            .MakeState = [runtime]() -> std::shared_ptr<void> {
                return std::make_shared<TWindowBlockingState>(
                    runtime.OutputType,
                    runtime.Keys,
                    runtime.KeyColumns,
                    runtime.Kernel);
            },
            .OutputType = runtime.OutputType,
        };
    }

    TLoweredOutput LowerWindow(
        TWindowOperator& window,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t childLanes = OutputLanes(window.Input());
        const auto windowGroup = StageGroup("window", &window);

        if (childLanes <= 1 || window.PartitionKeys().empty()) {
            return LowerBlocking(
                window.Input(),
                "window",
                windowGroup,
                [&, windowGroup](const NQumir::NAst::TTypePtr& childType) {
                    return BuildWindowTail(childType, window, windowGroup);
                },
                outConn,
                outLaneOffset);
        }

        const size_t parts = JoinPartitions(childLanes);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "window-shuffle-input");
        auto childOut = Lower(window.Input(), childConnRef);

        auto childType = childOut.OutputType;
        auto* childStruct =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!childStruct) {
            throw std::runtime_error("window input must have TStructType");
        }

        auto partitionHash = MakeGroupKeyHash(*childStruct, window.PartitionKeys());
        auto hashCode = MakeHashShuffleCode(std::move(partitionHash), childType);
        auto shuf = BuildShuffleNodes(
            childOut,
            childConnRef,
            childLanes,
            parts,
            std::move(hashCode),
            "window-shuffle");

        TBlockingTail tail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, windowGroup);
                return BuildWindowTail(childType, window, windowGroup);
            }();

        TLoweredOutput result;
        result.OutputType = tail.OutputType;
        result.Producers.reserve(parts);
        for (size_t m = 0; m < parts; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                tail.Code,
                tail.MakeState(),
                NScheduler::TInputPort{.Connection = shuf.Connection, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "window",
                windowGroup,
                "window");
            for (size_t s = 0; s < childLanes; ++s) {
                Graph_.AddEdge(*shuf.Nodes[s], node, *shuf.Connection, s, m);
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
        const std::string label = debugName;
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
            MarkNode(node,
                "hash-shuffle",
                StageGroup(label, hashCode.get()),
                label);
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
    std::vector<TGeneratedKernel>* KernelSink_ = nullptr;
    std::unordered_map<const TCteMaterialization*, TMaterializedProducer> Materialized_;
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
    std::vector<TGeneratedKernel> kernels;
    TSchedulerGraphLowerer lowerer(*graph, settings, diagnostics, &kernels);
    const size_t lanes = lowerer.OutputLanes(root);
    if (lanes == 0) {
        throw std::runtime_error(
            "scheduler lowering produced no output lanes for the plan");
    }

    IConnection* finalRef = nullptr;
    if (lanes == 1) {
        auto conn = std::make_unique<TOneToOneConnection>(lowerer.QueueCapacity());
        conn->Resize(1, 1);
        conn->SetDebugName("final-output");
        finalRef = &graph->AddConnection(std::move(conn));
    } else {
        auto gather = std::make_unique<TGatherConnection>(lowerer.QueueCapacity());
        gather->Resize(lanes, 1);
        gather->SetDebugName("final-gather");
        finalRef = &graph->AddConnection(std::move(gather));
    }
    auto out = lowerer.Lower(root, *finalRef);
    lowerer.AssertMaterializationsWired();

    return TLoweredPlan{
        .Graph = std::move(graph),
        .OutputType = std::move(out.OutputType),
        .FinalGather = finalRef,
        .Producers = std::move(out.Producers),
        .Lanes = lanes,
        .Kernels = std::move(kernels),
    };
}

bool RunPlanIntoSink(
    TLoweredPlan lowered,
    ISink& sink,
    TSettings settings,
    std::ostream* diagnostics,
    std::string* error)
{
    // Bind every kernel generated during lowering before any task can execute
    // (no-op for kernels the caller already finalized).
    JitFinalizeKernels(lowered.Kernels, diagnostics);

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

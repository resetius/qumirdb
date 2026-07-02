#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/unary_stream_exec.h>
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
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_node.h>
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
        TJoinKernels kernels)
        : Processor(
            std::move(leftType),
            std::move(rightType),
            std::move(kernels))
    {}

    TInnerJoinProcessor Processor;
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
            // output keeps the shuffle partition count.
            if (!n.Cast()->GroupKeys().empty() && childLanes > 1) {
                return JoinPartitions();
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
            if (join->JoinType() != EJoinType::Inner || join->Keys().empty()) {
                return 0;
            }
            if (OutputLanes(join->Left()) == 0 ||
                OutputLanes(join->Right()) == 0)
            {
                return 0;
            }
            return JoinPartitions();
        }
        return 0;
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

    size_t JoinPartitions() const {
        auto partitions = Settings_.HashShuffle.PartitionCount;
        if (partitions <= 1) {
            partitions = Settings_.Scheduler.WorkerCount;
        }

        auto maxPartitions = Settings_.HashShuffle.MaxPartitionCount;
        if (maxPartitions <= 1) {
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
        auto childConn = std::make_unique<NScheduler::TOneToOneConnection>(
            QueueCapacity());
        childConn->Resize(lanes, lanes);
        auto& childConnRef = Graph_.AddConnection(std::move(childConn));
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
        auto gather = std::make_unique<NScheduler::TGatherConnection>(
            QueueCapacity());
        gather->Resize(lanes, 1);
        auto& gatherRef = Graph_.AddConnection(std::move(gather));
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

    TLoweredOutput LowerAggregate(
        TAggregateOperator& aggregate,
        NScheduler::IConnection& outConn)
    {
        const size_t childLanes = OutputLanes(aggregate.Input());
        // Ungrouped or non-parallel input: single gathered aggregate.
        if (aggregate.GroupKeys().empty() || childLanes <= 1) {
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
        const size_t parts = JoinPartitions();
        const size_t cap = QueueCapacity();

        auto childConn = std::make_unique<NScheduler::TOneToOneConnection>(cap);
        childConn->Resize(childLanes, childLanes);
        auto& childConnRef = Graph_.AddConnection(std::move(childConn));
        auto childOut = Lower(aggregate.Input(), childConnRef);

        auto childType = childOut.OutputType;
        auto* childStruct =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!childStruct) {
            throw std::runtime_error("aggregate input must have TStructType");
        }
        auto groupHash = MakeGroupKeyHash(*childStruct, aggregate.GroupKeys());
        auto tail = BuildAggregateTail(childType, aggregate);

        auto shuffle =
            std::make_unique<NScheduler::THashShuffleConnection>(cap);
        shuffle->Resize(childLanes, parts);
        auto* shufPtr = shuffle.get();
        auto& shufRef = Graph_.AddConnection(std::move(shuffle));

        auto hashCode = std::make_shared<NScheduler::THashShuffleCode>(
            std::move(groupHash));
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
                auto code = std::make_shared<NScheduler::TBlockingCode>(
                    [](void* state,
                       NScheduler::TInputPort& input,
                       TRowSet& output)
                    {
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
                const int64_t limitValue = limit.Limit();
                const int64_t offsetValue = limit.Offset();
                return TBlockingTail{
                    .Code = std::move(code),
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
                auto keys = sortKeys;
                auto keyColumns = std::move(runtime.KeyColumns);
                auto radixKernel = std::move(runtime.RadixKernel);
                if (topLimit) {
                    auto code = std::make_shared<NScheduler::TBlockingCode>(
                        [](void* state,
                           NScheduler::TInputPort& input,
                           TRowSet& output)
                        {
                            auto* s = static_cast<TTopSortBlockingState*>(state);
                            TRowSet rowSet{};
                            while (!s->InputFinished) {
                                auto fetch = input.Fetch(rowSet);
                                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                                    return NScheduler::ETaskResult::NEED_DATA;
                                }
                                if (fetch ==
                                    NScheduler::EFetchResult::FINISHED)
                                {
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
                    const int64_t limit = *topLimit;
                    return TBlockingTail{
                        .Code = std::move(code),
                        .MakeState = [childType, keys, keyColumns, radixKernel,
                                      limit]() -> std::shared_ptr<void> {
                            return std::make_shared<TTopSortBlockingState>(
                                childType, keys, keyColumns, radixKernel, limit);
                        },
                        .OutputType = childType,
                    };
                }
                auto code = std::make_shared<NScheduler::TBlockingCode>(
                    [](void* state,
                       NScheduler::TInputPort& input,
                       TRowSet& output)
                    {
                        auto* s = static_cast<TSortBlockingState*>(state);
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
                return TBlockingTail{
                    .Code = std::move(code),
                    .MakeState = [childType, keys, keyColumns, radixKernel]()
                        -> std::shared_ptr<void>
                    {
                        return std::make_shared<TSortBlockingState>(
                            childType, keys, keyColumns, radixKernel);
                    },
                    .OutputType = childType,
                };
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
        const size_t cap = QueueCapacity();
        auto childConn = std::make_unique<NScheduler::TOneToOneConnection>(cap);
        const size_t lanes = OutputLanes(input);
        childConn->Resize(lanes, lanes);
        auto& childConnRef = Graph_.AddConnection(std::move(childConn));
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
        auto radixKernel = runtime.RadixKernel;

        std::shared_ptr<const NScheduler::TBlockingCode> localCode;
        std::function<std::shared_ptr<void>()> makeLocalState;
        if (topLimit) {
            localCode = std::make_shared<NScheduler::TBlockingCode>(
                [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
                    auto* s = static_cast<TTopSortBlockingState*>(state);
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
            const int64_t limit = *topLimit;
            makeLocalState = [childType, keys, keyColumns, radixKernel, limit]()
                -> std::shared_ptr<void>
            {
                return std::make_shared<TTopSortBlockingState>(
                    childType, keys, keyColumns, radixKernel, limit);
            };
        } else {
            localCode = std::make_shared<NScheduler::TBlockingCode>(
                [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
                    auto* s = static_cast<TSortBlockingState*>(state);
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
            makeLocalState = [childType, keys, keyColumns, radixKernel]()
                -> std::shared_ptr<void>
            {
                return std::make_shared<TSortBlockingState>(
                    childType, keys, keyColumns, radixKernel);
            };
        }

        // One local (top-)sort per input lane, each writing a sorted run.
        std::vector<NScheduler::IConnection*> runConns;
        std::vector<NScheduler::TInputPort> mergeInputs;
        std::vector<NScheduler::TTaskNode*> localSorts;
        runConns.reserve(lanes);
        mergeInputs.reserve(lanes);
        localSorts.reserve(lanes);
        for (size_t i = 0; i < lanes; ++i) {
            auto runConn =
                std::make_unique<NScheduler::TOneToOneConnection>(cap);
            runConn->Resize(1, 1);
            auto& runRef = Graph_.AddConnection(std::move(runConn));
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                localCode,
                makeLocalState(),
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

        const size_t cap = QueueCapacity();
        auto mergeConn = std::make_unique<NScheduler::TOneToOneConnection>(cap);
        mergeConn->Resize(1, 1);
        auto& mergeConnRef = Graph_.AddConnection(std::move(mergeConn));
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "top-sort", sort.Limit(), mergeConnRef);

        auto limitCode = std::make_shared<NScheduler::TBlockingCode>(
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

    TLoweredOutput LowerJoin(
        TJoinOperator& join,
        NScheduler::IConnection& outConn)
    {
        using namespace NQumir::NAst;
        const size_t leftLanes = OutputLanes(join.Left());
        const size_t rightLanes = OutputLanes(join.Right());
        const size_t joinParts = JoinPartitions();
        const size_t cap = QueueCapacity();

        auto leftPipe = std::make_unique<NScheduler::TOneToOneConnection>(cap);
        leftPipe->Resize(leftLanes, leftLanes);
        auto& leftPipeRef = Graph_.AddConnection(std::move(leftPipe));
        auto leftOut = Lower(join.Left(), leftPipeRef);

        auto rightPipe = std::make_unique<NScheduler::TOneToOneConnection>(cap);
        rightPipe->Resize(rightLanes, rightLanes);
        auto& rightPipeRef = Graph_.AddConnection(std::move(rightPipe));
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
            std::make_shared<NScheduler::THashShuffleCode>(hashKernels.Left));
        auto rightShuf = BuildShuffleNodes(
            rightOut,
            rightPipeRef,
            rightLanes,
            joinParts,
            std::make_shared<NScheduler::THashShuffleCode>(hashKernels.Right));

        auto joinCode = std::make_shared<NScheduler::TBinaryBlockingCode>(
            [](void* state,
               NScheduler::TInputPort& left,
               NScheduler::TInputPort& right,
               TRowSet& output)
            {
                auto* joinState = static_cast<TSchedulerInnerJoinState*>(state);
                auto fetchLeft = [&](TRowSet& rowSet) {
                    return MapJoinFetch(left.Fetch(rowSet));
                };
                auto fetchRight = [&](TRowSet& rowSet) {
                    return MapJoinFetch(right.Fetch(rowSet));
                };
                return MapJoinProcessResult(
                    joinState->Processor.Process(fetchLeft, fetchRight, output));
            });

        TLoweredOutput result;
        result.OutputType = kernelSpec.OutputSchema;
        result.Producers.reserve(joinParts);
        for (size_t j = 0; j < joinParts; ++j) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                joinCode,
                std::make_shared<TSchedulerInnerJoinState>(
                    leftType, rightType, *joinKernels),
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
        std::shared_ptr<NScheduler::THashShuffleCode> hashCode)
    {
        auto shuffle = std::make_unique<NScheduler::THashShuffleConnection>(
            ShuffleQueueCapacity());
        shuffle->Resize(inputLanes, joinParts);
        auto* shufPtr = shuffle.get();
        Graph_.AddConnection(std::move(shuffle));

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

private:
    NScheduler::TTaskGraph& Graph_;
    NScheduler::TSettings Settings_;
    std::ostream* Diagnostics_;
};

} // namespace

namespace NScheduler {

std::unique_ptr<IRuntimeNode> BuildSchedulerPlanPipeline(
    const TOperatorPtr& root,
    TSettings settings,
    std::ostream* diagnostics)
{
    auto graph = std::make_unique<TTaskGraph>();
    TSchedulerGraphLowerer lowerer(*graph, settings, diagnostics);
    const size_t lanes = lowerer.OutputLanes(root);
    if (lanes == 0) {
        return {};
    }

    auto gather = std::make_unique<TGatherConnection>(
        lowerer.QueueCapacity());
    gather->Resize(lanes, 1);
    auto& gatherRef = graph->AddConnection(std::move(gather));
    auto out = lowerer.Lower(root, gatherRef);

    auto output = std::make_shared<TBufferedSchedulerOutput>();
    auto sink = std::make_unique<TSinkTask>(
        MakeBufferedSchedulerSinkCode(),
        output,
        TInputPort{.Connection = &gatherRef, .Lane = 0});
    auto& sinkNode = graph->AddOwnedNode(std::move(sink));
    for (size_t p = 0; p < lanes; ++p) {
        graph->AddEdge(*out.Producers[p], sinkNode, gatherRef, p, 0);
    }

    graph->Build();
    std::string error;
    if (!graph->Validate(&error)) {
        throw std::runtime_error("scheduler graph invalid: " + error);
    }
    if (diagnostics) {
        *diagnostics << "\n========== SCHEDULER GRAPH ==========\n";
        *diagnostics << "mode="
            << static_cast<int>(settings.Scheduler.Mode)
            << " workers=" << settings.Scheduler.WorkerCount
            << " ready_queue=" << settings.Scheduler.ReadyQueueCapacity
            << " rowset_queue=" << settings.Queue.RowsetCapacityPerLane
            << " scan_tasks=" << settings.ScanSplit.MaxScanTasks
            << " shuffle_parts=" << settings.HashShuffle.PartitionCount
            << " shuffle_queue="
            << settings.HashShuffle.MaxQueuedRowsetsPerLane
            << "\n";
        graph->Print(*diagnostics);
        *diagnostics << "=====================================\n";
    }
    return std::make_unique<TRuntimeSchedulerPipeline>(
        std::move(graph),
        std::move(settings),
        std::move(out.OutputType),
        std::move(output));
}

} // namespace NScheduler
} // namespace NQdb

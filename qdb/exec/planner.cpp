#include <qdb/exec/planner.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/source_exec.h>
#include <qdb/exec/unary_block_exec.h>
#include <qdb/exec/unary_stream_exec.h>
#include <qdb/io/parquet/source.h>
#include <qdb/scheduler/runtime_node.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>

#include <qdb/kernel/project_type.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace NQdb {

namespace {

struct TSchedulerSourceState {
    ISource* Source = nullptr;
    std::unique_ptr<ISource> OwnedSource;
};

struct TSchedulerUnaryStage {
    NScheduler::TUnaryPartitionSpec Spec;
    NQumir::NAst::TTypePtr OutputType;
};

struct TUnaryRuntimeProcess {
    TRuntimeUnaryStreamingKernel::TProcess Process;
    NQumir::NAst::TTypePtr OutputType;
};

struct TSortRuntimeProcess {
    std::vector<TSortColumnRef> KeyColumns;
    TSortRadixKernel RadixKernel;
};

struct TSchedulerPipelineBuild {
    NScheduler::TPipelinePartitionSpec Spec;
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

// Byte width of the kernel output buffer per row for a computed column.
// For TStringType (string computed columns), the JIT kernel writes one
// TStringView struct (16 bytes) per row; the executor post-converts those.
size_t ProjectColumnWidth(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(inner)) {
        return 1;
    }
    if (TMaybeType<TStringType>(inner)) {
        return 16; // sizeof(TStringView): ptr(8) + size(8)
    }
    throw std::runtime_error(
        "project: unsupported computed column type " +
        (type ? type->ToString() : std::string("<null>")));
}

// Returns the kernel-level type the project kernel should write for a computed
// column. StringView is resolved from the qumirdb source module before lowering.
NQumir::NAst::TTypePtr ProjectJitType(const NQumir::NAst::TTypePtr& outType) {
    using namespace NQumir::NAst;
    if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(outType)))) {
        return std::make_shared<TNamedType>("StringView", nullptr);
    }
    return outType;
}

bool IsRadixSortableType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    return static_cast<bool>(TMaybeType<TIntegerType>(inner)) ||
        static_cast<bool>(TMaybeType<TFloatType>(inner));
}

struct TSortKernelInputs {
    std::vector<TSortColumnRef> KeyColumns;
    std::vector<NQumir::NAst::TTypePtr> RadixTypes;
    bool AllKeysRadixSortable = true;
};

TSortKernelInputs BuildSortKernelInputs(const NKernel::TOperatorKernelSpec& spec) {
    TSortKernelInputs inputs;
    inputs.KeyColumns.reserve(spec.SortKeys.size());
    inputs.RadixTypes.reserve(spec.SortKeys.size());
    for (const auto& key : spec.SortKeys) {
        inputs.AllKeysRadixSortable =
            inputs.AllKeysRadixSortable && IsRadixSortableType(key.Column.Type);
        inputs.RadixTypes.push_back(key.Column.Type);
        inputs.KeyColumns.push_back({
            .Index = key.Column.Index,
            .Type = key.Column.Type,
        });
    }
    return inputs;
}

void PrintKernelSpec(std::ostream* out, const NKernel::TOperatorKernelSpec& spec);

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    std::ostream* diagnostics)
{
    auto spec = NKernel::BuildSortKernelSpec(inputType, keys, std::string(kernelName));
    PrintKernelSpec(diagnostics, spec);
    auto sortInputs = BuildSortKernelInputs(spec);
    TSortRadixKernel radixKernel;
    if (sortInputs.AllKeysRadixSortable && !sortInputs.RadixTypes.empty()) {
        TKernelCompiler compiler(diagnostics);
        radixKernel = {
            .Enabled = true,
            .Dispatch = compiler.CompileRadixSortComposite(sortInputs.RadixTypes),
            .NullableDispatch = compiler.CompileRadixSortCompositeNullable(
                sortInputs.RadixTypes),
        };
    }
    return {
        .KeyColumns = std::move(sortInputs.KeyColumns),
        .RadixKernel = std::move(radixKernel),
    };
}

void PrintKernelSpec(std::ostream* out, const NKernel::TOperatorKernelSpec& spec) {
    if (!out) {
        return;
    }
    *out << "\n========== KERNEL SPEC ==========\n";
    NKernel::PrintKernelSpec(*out, spec);
    *out << "=================================\n";
}

NQumir::NAst::TTypePtr BuildSourceRuntimeType(TSourceOperator& src)
{
    if (auto required = src.RequiredColumns()) {
        auto* st = static_cast<NQumir::NAst::TStructType*>(required.get());
        std::unordered_set<std::string> cols;
        for (auto& [name, _] : st->Fields) {
            auto dot = name.rfind('.');
            cols.insert(dot != std::string::npos ? name.substr(dot + 1) : name);
        }
        src.GetSource().RestrictColumns(cols);
    }

    auto* qualSt = static_cast<NQumir::NAst::TStructType*>(
        src.OutputColumns().get());
    std::unordered_map<std::string,
        std::pair<std::string, NQumir::NAst::TTypePtr>> bareToQual;
    if (qualSt) {
        for (const auto& [qname, ftype] : qualSt->Fields) {
            auto dot = qname.rfind('.');
            auto bare = dot != std::string::npos ? qname.substr(dot + 1) : qname;
            bareToQual.try_emplace(bare, qname, ftype);
        }
    }

    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    for (const auto& col : src.GetSource().Schema().Columns) {
        auto bare = std::string(col.Name);
        auto it = bareToQual.find(bare);
        if (it != bareToQual.end()) {
            fields.emplace_back(it->second.first, it->second.second);
        } else {
            fields.emplace_back(bare, col.Type);
        }
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TUnaryRuntimeProcess BuildFilterRuntimeProcess(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("filter input must have TStructType");
    }

    auto spec = NKernel::BuildFilterKernelSpec(*inputStruct, filter.Predicate());
    TKernelCompiler compiler(diagnostics);
    return {
        .Process = MakeFilterProcess(compiler.CompileFilter(spec)),
        .OutputType = inputType,
    };
}

// Holds a selection buffer owned by a scheduler rowset, chaining destruction to
// the wrapped rowset's original owner.
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
// every batch (TUnaryStreamingKernelState::Selection). That is safe for the
// serial pipeline where each batch is consumed before the next is produced, but
// in the scheduler pipeline a filtered rowset can sit in a bounded queue while
// the filter overwrites the buffer for the next batch. Detach the selection
// into a rowset-owned copy so the rowset stays self-contained across the edge.
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
        .Spec = {
            .Code = std::move(code),
            .MakeState = [](size_t) {
                return std::make_shared<TUnaryStreamingKernelState>();
            },
        },
        .OutputType = std::move(runtime.OutputType),
    };
}

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("project input must have TStructType");
    }

    std::vector<TProjectColumn> columns;
    std::vector<NQumir::NAst::TExprPtr> computedExprs;
    std::vector<NQumir::NAst::TTypePtr> computedJitTypes;
    std::vector<size_t> computedWidths;
    std::vector<bool> computedIsString;
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    for (const auto& projection : project.Projections()) {
        if (auto identNode = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
                projection.Expression)) {
            const std::string& exprName = identNode.Cast()->Name;
            auto it = std::find_if(
                inputStruct->Fields.begin(), inputStruct->Fields.end(),
                [&](const auto& field) { return field.first == exprName; });
            if (it == inputStruct->Fields.end()) {
                throw std::runtime_error("project column not found: " + exprName);
            }
            columns.push_back({
                .Computed = false,
                .Index = static_cast<int32_t>(
                    std::distance(inputStruct->Fields.begin(), it)),
            });
            outFields.emplace_back(projection.Name, it->second);
        } else {
            auto outType = NKernel::InferProjectExprType(
                projection.Expression,
                *inputStruct);
            auto jitType = ProjectJitType(outType);
            using namespace NQumir::NAst;
            bool isStr = static_cast<bool>(TMaybeType<TStringType>(
                UnwrapNamedType(UnwrapNullableType(outType))));
            columns.push_back({
                .Computed = true,
                .Index = static_cast<int32_t>(computedExprs.size()),
            });
            computedExprs.push_back(projection.Expression);
            computedJitTypes.push_back(jitType);
            computedWidths.push_back(ProjectColumnWidth(outType));
            computedIsString.push_back(isStr);
            outFields.emplace_back(projection.Name, outType);
        }
    }

    TKernelCompiler::TProjectDispatch dispatch;
    if (!computedExprs.empty()) {
        auto spec = NKernel::BuildProjectKernelSpec(
            *inputStruct,
            computedExprs,
            computedJitTypes);
        TKernelCompiler compiler(diagnostics);
        dispatch = compiler.CompileProject(spec);
    }

    auto process = MakeProjectProcess(
        std::move(columns),
        std::move(dispatch),
        std::move(computedWidths),
        std::move(computedIsString));

    return {
        .Process = std::move(process),
        .OutputType = std::make_shared<NQumir::NAst::TStructType>(
            std::move(outFields)),
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
        .Spec = {
            .Code = std::move(code),
            .MakeState = [](size_t) {
                return std::make_shared<TUnaryStreamingKernelState>();
            },
        },
        .OutputType = std::move(runtime.OutputType),
    };
}

std::optional<TSchedulerPipelineBuild> BuildSchedulerUnarySpec(
    const TOperatorPtr& root,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    std::vector<TOperatorPtr> stages;
    auto current = root;
    while (current) {
        if (auto filter = TMaybeOp<TFilterOperator>(current)) {
            stages.push_back(current);
            current = filter.Cast()->Input();
        } else if (auto project = TMaybeOp<TProjectOperator>(current)) {
            stages.push_back(current);
            current = project.Cast()->Input();
        } else {
            break;
        }
    }

    auto source = TMaybeOp<TSourceOperator>(current);
    if (!source) {
        return std::nullopt;
    }

    auto outputType = BuildSourceRuntimeType(*source.Cast());
    auto* sourcePtr = &source.Cast()->GetSource();
    auto* parquetSource = dynamic_cast<TParquetSource*>(sourcePtr);
    auto sourceCode = std::make_shared<NScheduler::TSourceCode>(
        [](void* state, TRowSet& rowSet) {
            auto* sourceState = static_cast<TSchedulerSourceState*>(state);
            return sourceState->Source->Next(rowSet);
        });

    NScheduler::TPipelinePartitionSpec spec;
    spec.Source = NScheduler::TSourcePartitionSpec{
        .Code = std::move(sourceCode),
        .MakeState = [source = sourcePtr, parquetSource](
            size_t,
            const NScheduler::TScanSplit* split)
        {
            if (parquetSource && split && split->RowGroupCount) {
                auto owned = parquetSource->MakeRowGroupRangeSource(
                    split->FirstRowGroup,
                    split->RowGroupCount);
                auto* ptr = owned.get();
                return std::make_shared<TSchedulerSourceState>(
                    TSchedulerSourceState{
                        .Source = ptr,
                        .OwnedSource = std::move(owned),
                    });
            }
            return std::make_shared<TSchedulerSourceState>(TSchedulerSourceState{
                .Source = source,
            });
        },
    };
    if (parquetSource) {
        spec.Source.Splits = NScheduler::BuildScanSplits(
            parquetSource->ScanRowGroups(),
            settings.ScanSplit);
    }

    for (auto it = stages.rbegin(); it != stages.rend(); ++it) {
        if (auto filter = TMaybeOp<TFilterOperator>(*it)) {
            auto stage = BuildSchedulerFilterStage(
                *filter.Cast(),
                outputType,
                diagnostics);
            outputType = std::move(stage.OutputType);
            spec.UnaryStages.push_back(std::move(stage.Spec));
        } else if (auto project = TMaybeOp<TProjectOperator>(*it)) {
            auto stage = BuildSchedulerProjectStage(
                *project.Cast(),
                outputType,
                diagnostics);
            outputType = std::move(stage.OutputType);
            spec.UnaryStages.push_back(std::move(stage.Spec));
        }
    }

    spec.Settings = settings;
    if (spec.Source.Splits.empty()) {
        spec.Settings.Partitioning.DefaultPartitionCount = 1;
        spec.Settings.Partitioning.MaxPartitionCount = 1;
    }

    return TSchedulerPipelineBuild{
        .Spec = std::move(spec),
        .OutputType = std::move(outputType),
    };
}

std::unique_ptr<IRuntimeNode> BuildSchedulerRuntimePipeline(
    TSchedulerPipelineBuild build)
{
    std::string error;
    auto runtime = NScheduler::BuildBufferedSchedulerRuntimePipeline(
        std::move(build.Spec),
        std::move(build.OutputType),
        &error);
    if (!runtime) {
        throw std::runtime_error(error);
    }
    return runtime;
}

NScheduler::TPipelineSidePartitionSpec TakePipelineSide(TSchedulerPipelineBuild& build)
{
    return NScheduler::TPipelineSidePartitionSpec{
        .Source = std::move(build.Spec.Source),
        .UnaryStages = std::move(build.Spec.UnaryStages),
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

std::unique_ptr<IRuntimeNode> TryBuildSchedulerUnaryPipeline(
    const TOperatorPtr& root,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    auto build = BuildSchedulerUnarySpec(root, std::move(settings), diagnostics);
    if (!build) {
        return {};
    }
    return BuildSchedulerRuntimePipeline(std::move(*build));
}

std::unique_ptr<IRuntimeNode> TryBuildSchedulerJoinPipeline(
    TJoinOperator& join,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    using namespace NQumir::NAst;

    if (join.JoinType() != EJoinType::Inner || join.Keys().empty()) {
        return {};
    }

    auto left = BuildSchedulerUnarySpec(
        join.Left(),
        settings,
        diagnostics);
    if (!left) {
        return {};
    }
    auto right = BuildSchedulerUnarySpec(
        join.Right(),
        settings,
        diagnostics);
    if (!right) {
        return {};
    }

    auto leftType = left->OutputType;
    auto rightType = right->OutputType;
    auto* leftStruct = static_cast<TStructType*>(leftType.get());
    auto* rightStruct = static_cast<TStructType*>(rightType.get());
    if (!leftStruct || !rightStruct) {
        throw std::runtime_error("scheduler join inputs must have TStructType");
    }

    auto kernelSpec = NKernel::BuildJoinKernelSpec(
        *leftStruct,
        *rightStruct,
        join.Keys(),
        join.JoinType(),
        join.Filter());
    TKernelCompiler compiler(diagnostics);
    auto joinKernels = std::make_shared<TJoinKernels>(
        compiler.CompileJoin(kernelSpec));
    auto hashKernels = compiler.CompileJoinHash(kernelSpec);

    if (left->Spec.Source.Splits.empty() || right->Spec.Source.Splits.empty()) {
        settings.Partitioning.DefaultPartitionCount = 1;
        settings.Partitioning.MaxPartitionCount = 1;
    }
    if (settings.HashShuffle.PartitionCount <= 1) {
        settings.HashShuffle.PartitionCount = std::max<size_t>(
            settings.Partitioning.DefaultPartitionCount,
            1);
    }
    if (settings.HashShuffle.MaxPartitionCount <= 1) {
        settings.HashShuffle.MaxPartitionCount = std::max<size_t>(
            settings.Partitioning.MaxPartitionCount,
            settings.HashShuffle.PartitionCount);
    }

    auto code = std::make_shared<NScheduler::TBinaryBlockingCode>(
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

    NScheduler::TJoinPipelinePartitionSpec spec;
    spec.Left = TakePipelineSide(*left);
    spec.Right = TakePipelineSide(*right);
    spec.LeftShuffle = NScheduler::THashShufflePartitionSpec{
        .Code = std::make_shared<NScheduler::THashShuffleCode>(hashKernels.Left),
        .MakeState = [](size_t) {
            return std::make_shared<int>(0);
        },
    };
    spec.RightShuffle = NScheduler::THashShufflePartitionSpec{
        .Code = std::make_shared<NScheduler::THashShuffleCode>(hashKernels.Right),
        .MakeState = [](size_t) {
            return std::make_shared<int>(0);
        },
    };
    spec.Join = NScheduler::TBinaryPartitionSpec{
        .Code = std::move(code),
        .MakeState = [
            leftType,
            rightType,
            joinKernels](size_t)
        {
            return std::make_shared<TSchedulerInnerJoinState>(
                leftType,
                rightType,
                *joinKernels);
        },
    };
    spec.Settings = settings;

    std::string error;
    auto runtime = NScheduler::BuildBufferedSchedulerJoinRuntimePipeline(
        std::move(spec),
        std::move(kernelSpec.OutputSchema),
        &error);
    if (!runtime) {
        throw std::runtime_error(error);
    }
    return runtime;
}

std::unique_ptr<IRuntimeNode> TryBuildSchedulerAggregatePipeline(
    TAggregateOperator& aggregate,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    auto build = BuildSchedulerUnarySpec(
        aggregate.Input(),
        std::move(settings),
        diagnostics);
    if (!build) {
        return {};
    }

    auto* inputType = static_cast<NQumir::NAst::TStructType*>(
        build->OutputType.get());
    if (!inputType) {
        throw std::runtime_error("aggregate input must have TStructType");
    }

    auto spec = NKernel::BuildAggregateKernelSpec(
        *inputType,
        aggregate.GroupKeys(),
        aggregate.Aggs());
    TKernelCompiler compiler(diagnostics);
    auto kernels = std::make_shared<TAggregateKernels>(
        compiler.CompileAggregate(spec));

    auto code = std::make_shared<NScheduler::TBlockingCode>(
        [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* aggregateState = static_cast<TAggregateBlockingState*>(state);
            if (aggregateState->Done) {
                return NScheduler::ETaskResult::FINISHED;
            }

            TRowSet rowSet{};
            while (true) {
                auto fetch = input.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    aggregateState->Done = true;
                    aggregateState->Processor.Finish(output);
                    return NScheduler::ETaskResult::OK;
                }
                aggregateState->Processor.Add(rowSet);
                Release(&rowSet);
                rowSet = {};
            }
        });

    build->Spec.BlockingTail = NScheduler::TBlockingPartitionSpec{
        .Code = std::move(code),
        .MakeState = [kernels]() {
            return std::make_shared<TAggregateBlockingState>(*kernels);
        },
    };
    build->OutputType = ComputeAggregateOutputType(
        build->OutputType,
        aggregate.GroupKeys(),
        aggregate.Aggs());

    return BuildSchedulerRuntimePipeline(std::move(*build));
}

std::unique_ptr<IRuntimeNode> TryBuildSchedulerLimitPipeline(
    TLimitOperator& limit,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    auto build = BuildSchedulerUnarySpec(
        limit.Input(),
        std::move(settings),
        diagnostics);
    if (!build) {
        return {};
    }

    auto code = std::make_shared<NScheduler::TBlockingCode>(
        [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* limitState = static_cast<TLimitBlockingState*>(state);
            if (limitState->Processor.Finished()) {
                return NScheduler::ETaskResult::FINISHED;
            }

            TRowSet rowSet{};
            while (!limitState->Processor.Finished()) {
                auto fetch = input.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                if (limitState->Processor.Process(rowSet, output)) {
                    return NScheduler::ETaskResult::OK;
                }
                rowSet = {};
            }
            return NScheduler::ETaskResult::FINISHED;
        });

    const int64_t limitValue = limit.Limit();
    const int64_t offsetValue = limit.Offset();
    build->Spec.BlockingTail = NScheduler::TBlockingPartitionSpec{
        .Code = std::move(code),
        .MakeState = [limitValue, offsetValue]() {
            return std::make_shared<TLimitBlockingState>(
                limitValue,
                offsetValue);
        },
    };

    return BuildSchedulerRuntimePipeline(std::move(*build));
}

std::unique_ptr<IRuntimeNode> TryBuildSchedulerSortPipeline(
    TSortOperator& sort,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    auto build = BuildSchedulerUnarySpec(
        sort.Input(),
        std::move(settings),
        diagnostics);
    if (!build) {
        return {};
    }

    auto* inputType = static_cast<NQumir::NAst::TStructType*>(
        build->OutputType.get());
    if (!inputType) {
        throw std::runtime_error("sort input must have TStructType");
    }

    auto runtime = BuildSortRuntimeProcess(
        *inputType,
        sort.Keys(),
        "sort",
        diagnostics);
    auto outputType = build->OutputType;
    auto keys = sort.Keys();
    auto keyColumns = std::move(runtime.KeyColumns);
    auto radixKernel = std::move(runtime.RadixKernel);

    auto code = std::make_shared<NScheduler::TBlockingCode>(
        [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* sortState = static_cast<TSortBlockingState*>(state);
            TRowSet rowSet{};
            while (!sortState->InputFinished) {
                auto fetch = input.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    sortState->InputFinished = true;
                    break;
                }
                sortState->Processor.Add(rowSet);
                rowSet = {};
            }
            if (sortState->Processor.Next(output)) {
                return NScheduler::ETaskResult::OK;
            }
            return NScheduler::ETaskResult::FINISHED;
        });

    build->Spec.BlockingTail = NScheduler::TBlockingPartitionSpec{
        .Code = std::move(code),
        .MakeState = [
            outputType = std::move(outputType),
            keys = std::move(keys),
            keyColumns = std::move(keyColumns),
            radixKernel = std::move(radixKernel)]()
        {
            return std::make_shared<TSortBlockingState>(
                outputType,
                keys,
                keyColumns,
                radixKernel);
        },
    };

    return BuildSchedulerRuntimePipeline(std::move(*build));
}

std::unique_ptr<IRuntimeNode> TryBuildSchedulerTopSortPipeline(
    TTopSortOperator& sort,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    auto build = BuildSchedulerUnarySpec(
        sort.Input(),
        std::move(settings),
        diagnostics);
    if (!build) {
        return {};
    }

    auto* inputType = static_cast<NQumir::NAst::TStructType*>(
        build->OutputType.get());
    if (!inputType) {
        throw std::runtime_error("top-sort input must have TStructType");
    }

    auto runtime = BuildSortRuntimeProcess(
        *inputType,
        sort.Keys(),
        "top-sort",
        diagnostics);
    auto outputType = build->OutputType;
    auto keys = sort.Keys();
    auto keyColumns = std::move(runtime.KeyColumns);
    auto radixKernel = std::move(runtime.RadixKernel);
    const int64_t limit = sort.Limit();

    auto code = std::make_shared<NScheduler::TBlockingCode>(
        [](void* state, NScheduler::TInputPort& input, TRowSet& output) {
            auto* sortState = static_cast<TTopSortBlockingState*>(state);
            TRowSet rowSet{};
            while (!sortState->InputFinished) {
                auto fetch = input.Fetch(rowSet);
                if (fetch == NScheduler::EFetchResult::NO_DATA) {
                    return NScheduler::ETaskResult::NEED_DATA;
                }
                if (fetch == NScheduler::EFetchResult::FINISHED) {
                    sortState->InputFinished = true;
                    break;
                }
                sortState->Processor.Add(rowSet);
                rowSet = {};
            }
            if (sortState->Processor.Next(output)) {
                return NScheduler::ETaskResult::OK;
            }
            return NScheduler::ETaskResult::FINISHED;
        });

    build->Spec.BlockingTail = NScheduler::TBlockingPartitionSpec{
        .Code = std::move(code),
        .MakeState = [
            outputType = std::move(outputType),
            keys = std::move(keys),
            keyColumns = std::move(keyColumns),
            radixKernel = std::move(radixKernel),
            limit]()
        {
            return std::make_shared<TTopSortBlockingState>(
                outputType,
                keys,
                keyColumns,
                radixKernel,
                limit);
        },
    };

    return BuildSchedulerRuntimePipeline(std::move(*build));
}

// ---------------------------------------------------------------------------
// Single-graph lowering.
//
// Lowers a physical plan (sub)tree into ONE TTaskGraph executed by ONE
// scheduler, per the Single-Graph Invariant. Operators are stitched with
// connections instead of nested TRuntimeSchedulerPipeline runs:
//   scan/filter/project -> OneToOne partition lanes
//   blocking tail (aggregate/limit/sort/top-sort) -> Gather -> single task
//   inner equi-join -> HashShuffle -> partition-local join tasks
// The output connection of each operator is created by its consumer (which
// knows whether it wants per-lane, gather, or shuffle topology) and passed
// down; segment width is computed up front by OutputLanes.
// ---------------------------------------------------------------------------

struct TLoweredOutput {
    std::vector<NScheduler::TTaskNode*> Producers;
    NQumir::NAst::TTypePtr OutputType;
};

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

    // Number of output partition lanes for op's pipeline segment, or 0 if the
    // subtree contains an operator that cannot be lowered.
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
        if (TMaybeOp<TAggregateOperator>(op) ||
            TMaybeOp<TLimitOperator>(op) ||
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

    // Builds op's tasks writing to outConn (lane p per producer p). Returns the
    // producer nodes and the operator output type.
    TLoweredOutput Lower(const TOperatorPtr& op, NScheduler::IConnection& outConn) {
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

private:
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
                stage.Spec.Code,
                stage.Spec.MakeState(p),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = p},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            Graph_.AddEdge(*childOut.Producers[p], node, childConnRef, p, p);
            result.Producers.push_back(&node);
        }
        return result;
    }

    struct TBlockingTail {
        std::shared_ptr<const NScheduler::TBlockingCode> Code;
        std::function<std::shared_ptr<void>()> MakeState;
        NQumir::NAst::TTypePtr OutputType;
    };

    // Gathers a child segment to one lane, runs a single blocking task, and
    // writes the result to outConn lane 0. buildTail receives the child's real
    // output type, so tail kernels are compiled exactly once.
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

    TLoweredOutput LowerAggregate(
        TAggregateOperator& aggregate,
        NScheduler::IConnection& outConn)
    {
        return LowerBlocking(
            aggregate.Input(),
            [&](const NQumir::NAst::TTypePtr& childType) {
                auto* inputType =
                    static_cast<NQumir::NAst::TStructType*>(childType.get());
                if (!inputType) {
                    throw std::runtime_error(
                        "aggregate input must have TStructType");
                }
                auto spec = NKernel::BuildAggregateKernelSpec(
                    *inputType, aggregate.GroupKeys(), aggregate.Aggs());
                TKernelCompiler compiler(Diagnostics_);
                auto kernels = std::make_shared<TAggregateKernels>(
                    compiler.CompileAggregate(spec));
                auto code = std::make_shared<NScheduler::TBlockingCode>(
                    [](void* state,
                       NScheduler::TInputPort& input,
                       TRowSet& output)
                    {
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
            },
            outConn);
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

    TLoweredOutput LowerSort(
        TSortOperator& sort,
        NScheduler::IConnection& outConn)
    {
        return LowerSortLike(
            sort.Input(), sort.Keys(), "sort", std::nullopt, outConn);
    }

    TLoweredOutput LowerTopSort(
        TTopSortOperator& sort,
        NScheduler::IConnection& outConn)
    {
        return LowerSortLike(
            sort.Input(), sort.Keys(), "top-sort", sort.Limit(), outConn);
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

        // Lower both inputs into their own OneToOne segments.
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

        // Hash shuffle tasks per input lane scatter into joinParts.
        auto leftShuf = BuildShuffleNodes(
            leftOut, leftPipeRef, leftLanes, joinParts,
            std::make_shared<NScheduler::THashShuffleCode>(hashKernels.Left));
        auto rightShuf = BuildShuffleNodes(
            rightOut, rightPipeRef, rightLanes, joinParts,
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

    struct TShuffleSide {
        NScheduler::THashShuffleConnection* Connection = nullptr;
        std::vector<NScheduler::TTaskNode*> Nodes;
    };

    // Creates a HashShuffle connection plus one shuffle task per source lane,
    // reading the lowered input segment.
    TShuffleSide BuildShuffleNodes(
        const TLoweredOutput& input,
        NScheduler::IConnection& inputConn,
        size_t inputLanes,
        size_t joinParts,
        std::shared_ptr<NScheduler::THashShuffleCode> hashCode)
    {
        auto shuffle = std::make_unique<NScheduler::THashShuffleConnection>(
            QueueCapacity());
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

    NScheduler::TTaskGraph& Graph_;
    NScheduler::TSettings Settings_;
    std::ostream* Diagnostics_;
};

std::unique_ptr<IRuntimeNode> BuildSchedulerPlanPipeline(
    const TOperatorPtr& root,
    NScheduler::TSettings settings,
    std::ostream* diagnostics)
{
    auto graph = std::make_unique<NScheduler::TTaskGraph>();
    TSchedulerGraphLowerer lowerer(*graph, settings, diagnostics);
    const size_t lanes = lowerer.OutputLanes(root);
    if (lanes == 0) {
        return {};
    }

    // Final gather to one lane feeding the buffered sink (the single root).
    auto gather = std::make_unique<NScheduler::TGatherConnection>(
        lowerer.QueueCapacity());
    gather->Resize(lanes, 1);
    auto& gatherRef = graph->AddConnection(std::move(gather));
    auto out = lowerer.Lower(root, gatherRef);

    auto output = std::make_shared<NScheduler::TBufferedSchedulerOutput>();
    auto sink = std::make_unique<NScheduler::TSinkTask>(
        NScheduler::MakeBufferedSchedulerSinkCode(),
        output,
        NScheduler::TInputPort{.Connection = &gatherRef, .Lane = 0});
    auto& sinkNode = graph->AddOwnedNode(std::move(sink));
    for (size_t p = 0; p < lanes; ++p) {
        graph->AddEdge(*out.Producers[p], sinkNode, gatherRef, p, 0);
    }

    graph->Build();
    std::string error;
    if (!graph->Validate(&error)) {
        throw std::runtime_error("scheduler graph invalid: " + error);
    }
    return std::make_unique<NScheduler::TRuntimeSchedulerPipeline>(
        std::move(graph),
        std::move(settings),
        std::move(out.OutputType),
        std::move(output));
}

} // namespace

void TPhysicalPlanner::PrintRuntimePlan(const TOperatorPtr& root) const {
    if (!Diagnostics_) {
        return;
    }
    *Diagnostics_ << "\n========== RUNTIME PLAN ==========\n";
    PrintRuntimePlan(root, 0);
    *Diagnostics_ << "==================================\n";
}

void TPhysicalPlanner::PrintRuntimePlan(const TOperatorPtr& root, int depth) const {
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    *Diagnostics_ << indent;
    if (auto node = TMaybeOp<TSourceOperator>(root)) {
        *Diagnostics_ << "source " << node.Cast()->SourcePath() << "\n";
        return;
    }
    if (auto node = TMaybeOp<TFilterOperator>(root)) {
        *Diagnostics_ << "filter [JIT: AST -> IR -> LLVM]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TProjectOperator>(root)) {
        *Diagnostics_ << "project [column mapping]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TAggregateOperator>(root)) {
        *Diagnostics_ << "aggregate [JIT: update + finalize]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TJoinOperator>(root)) {
        *Diagnostics_ << "join [symmetric hash, JIT probe+insert]\n";
        PrintRuntimePlan(node.Cast()->Left(), depth + 1);
        PrintRuntimePlan(node.Cast()->Right(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TSortOperator>(root)) {
        *Diagnostics_ << "sort [stable indices]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TTopSortOperator>(root)) {
        *Diagnostics_ << "top-sort [bounded stable state]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TLimitOperator>(root)) {
        *Diagnostics_ << "limit [" << node.Cast()->Limit()
            << ", offset " << node.Cast()->Offset() << "]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    *Diagnostics_ << "unknown\n";
}

std::unique_ptr<IRuntimeNode> TPhysicalPlanner::Build(const TOperatorPtr& root) {
    if (SchedulerSettings_.Scheduler.Mode != NScheduler::EExecutionMode::Serial) {
        // Lower the whole plan into a single scheduler graph. Returns null when
        // the plan contains an operator the lowering does not yet support, in
        // which case we fall through to the serial builders below.
        if (auto runtime = BuildSchedulerPlanPipeline(
                root,
                SchedulerSettings_,
                Diagnostics_))
        {
            return runtime;
        }
    }

    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        auto src = maybe.Cast();
        return std::make_unique<TRuntimeSource>(
            src->GetSource(),
            BuildSourceRuntimeType(*src));
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto filter = maybe.Cast();
        auto input = Build(filter->Input());
        auto runtime = BuildFilterRuntimeProcess(
            *filter,
            input->OutputType(),
            Diagnostics_);
        return std::make_unique<TRuntimeUnaryStreamingKernel>(
            std::move(input),
            std::move(runtime.OutputType),
            std::move(runtime.Process));
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto project = maybe.Cast();
        auto input = Build(project->Input());
        auto runtime = BuildProjectRuntimeProcess(
            *project,
            input->OutputType(),
            Diagnostics_);
        return std::make_unique<TRuntimeUnaryStreamingKernel>(
            std::move(input),
            std::move(runtime.OutputType),
            std::move(runtime.Process));
    }

    if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
        auto agg = maybe.Cast();
        auto input = Build(agg->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("aggregate input must have TStructType");
        }

        auto spec = NKernel::BuildAggregateKernelSpec(
            *inputType, agg->GroupKeys(), agg->Aggs());
        TKernelCompiler compiler(Diagnostics_);
        auto kernels = compiler.CompileAggregate(spec);

        // Output type from the physical (pruned) input type, not the logical
        // OutputColumns() (which was computed from the pre-pruning schema).
        auto outputType = ComputeAggregateOutputType(input->OutputType(), agg->GroupKeys(), agg->Aggs());

        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            std::move(outputType),
            MakeAggregateProcess(std::move(kernels)));
    }

    if (auto maybe = TMaybeOp<TJoinOperator>(root)) {
        using namespace NQumir::NAst;
        auto join = maybe.Cast();
        auto left = Build(join->Left());
        auto right = Build(join->Right());
        auto* leftType = static_cast<TStructType*>(left->OutputType().get());
        auto* rightType = static_cast<TStructType*>(right->OutputType().get());
        if (!leftType || !rightType) {
            throw std::runtime_error("join inputs must have TStructType");
        }

        // Cross join: no key columns → Cartesian product executor.
        if (join->Keys().empty()) {
            auto outputType = ComputeJoinOutputType(
                left->OutputType(), right->OutputType(), join->JoinType());
            if (!outputType) {
                throw std::runtime_error("cross join: " + outputType.error().ToString());
            }
            return std::make_unique<TRuntimeCrossJoin>(
                std::move(left), std::move(right), std::move(*outputType));
        }

        auto spec = NKernel::BuildJoinKernelSpec(
            *leftType, *rightType, join->Keys(), join->JoinType(), join->Filter());

        TKernelCompiler compiler(Diagnostics_);
        auto kernels = compiler.CompileJoin(spec);

        return std::make_unique<TRuntimeJoin>(
            std::move(left), std::move(right), std::move(spec.OutputSchema),
            std::move(kernels),
            join->JoinType(),
            /*hasResidual=*/join->Filter() != nullptr);
    }

    if (auto maybe = TMaybeOp<TSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto input = Build(sort->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("sort input must have TStructType");
        }

        auto runtime = BuildSortRuntimeProcess(
            *inputType,
            sort->Keys(),
            "sort",
            Diagnostics_);

        auto outputType = input->OutputType();
        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            outputType,
            MakeSortProcess(
                outputType,
                sort->Keys(),
                std::move(runtime.KeyColumns),
                std::move(runtime.RadixKernel)));
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto input = Build(sort->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("top-sort input must have TStructType");
        }

        auto runtime = BuildSortRuntimeProcess(
            *inputType,
            sort->Keys(),
            "top-sort",
            Diagnostics_);

        auto outputType = input->OutputType();
        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            outputType,
            MakeTopSortProcess(
                outputType,
                sort->Keys(),
                std::move(runtime.KeyColumns),
                std::move(runtime.RadixKernel),
                sort->Limit()));
    }

    if (auto maybe = TMaybeOp<TLimitOperator>(root)) {
        auto limit = maybe.Cast();
        auto input = Build(limit->Input());
        return std::make_unique<TRuntimeLimit>(
            std::move(input),
            input->OutputType(),
            limit->Limit(),
            limit->Offset());
    }

    throw std::runtime_error("TPhysicalPlanner: unknown operator");
}

} // namespace NQdb

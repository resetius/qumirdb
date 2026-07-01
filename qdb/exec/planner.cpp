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
        if (auto runtime = TryBuildSchedulerUnaryPipeline(
                root,
                SchedulerSettings_,
                Diagnostics_))
        {
            return runtime;
        }
        if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
            if (auto runtime = TryBuildSchedulerAggregatePipeline(
                    *maybe.Cast(),
                    SchedulerSettings_,
                    Diagnostics_))
            {
                return runtime;
            }
        }
        if (auto maybe = TMaybeOp<TLimitOperator>(root)) {
            if (auto runtime = TryBuildSchedulerLimitPipeline(
                    *maybe.Cast(),
                    SchedulerSettings_,
                    Diagnostics_))
            {
                return runtime;
            }
        }
        if (auto maybe = TMaybeOp<TSortOperator>(root)) {
            if (auto runtime = TryBuildSchedulerSortPipeline(
                    *maybe.Cast(),
                    SchedulerSettings_,
                    Diagnostics_))
            {
                return runtime;
            }
        }
        if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
            if (auto runtime = TryBuildSchedulerTopSortPipeline(
                    *maybe.Cast(),
                    SchedulerSettings_,
                    Diagnostics_))
            {
                return runtime;
            }
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

#include <qdb/exec/planner.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/source_exec.h>
#include <qdb/exec/unary_block_exec.h>
#include <qdb/exec/unary_stream_exec.h>
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
#include <stdexcept>

namespace NQdb {

namespace {

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

void PrintKernelSpec(std::ostream* out, const NKernel::TOperatorKernelSpec& spec) {
    if (!out) {
        return;
    }
    *out << "\n========== KERNEL SPEC ==========\n";
    NKernel::PrintKernelSpec(*out, spec);
    *out << "=================================\n";
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
    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        auto src = maybe.Cast();
        // After column pruning, RequiredColumns() holds the narrowed struct.
        // Column names may be qualified ("alias.col") after QualifyColumns —
        // strip the prefix so the physical source gets bare column names.
        if (auto required = src->RequiredColumns()) {
            auto* st = static_cast<NQumir::NAst::TStructType*>(required.get());
            std::unordered_set<std::string> cols;
            for (auto& [name, _] : st->Fields) {
                auto dot = name.rfind('.');
                cols.insert(dot != std::string::npos ? name.substr(dot + 1) : name);
            }
            src->GetSource().RestrictColumns(cols);
        }
        // Build a runtime type that uses qualified names (so kernel variable names
        // match predicate idents) but in physical column ORDER (post-RestrictColumns).
        // The physical and logical orders may differ for mock sources in tests.
        {
            auto* qualSt = static_cast<NQumir::NAst::TStructType*>(
                src->OutputColumns().get());
            // Map bare name → (qualified name, type)
            std::unordered_map<std::string,
                std::pair<std::string, NQumir::NAst::TTypePtr>> bareToQual;
            if (qualSt) {
                for (const auto& [qname, ftype] : qualSt->Fields) {
                    auto dot = qname.rfind('.');
                    auto bare = (dot != std::string::npos)
                        ? qname.substr(dot + 1) : qname;
                    bareToQual.try_emplace(bare, qname, ftype);
                }
            }
            std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
            for (const auto& col : src->GetSource().Schema().Columns) {
                auto bare = std::string(col.Name);
                auto it = bareToQual.find(bare);
                if (it != bareToQual.end()) {
                    fields.emplace_back(it->second.first, it->second.second);
                } else {
                    fields.emplace_back(bare, col.Type);
                }
            }
            auto actualType = std::make_shared<NQumir::NAst::TStructType>(
                std::move(fields));
            return std::make_unique<TRuntimeSource>(src->GetSource(), actualType);
        }
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto filter = maybe.Cast();
        auto input = Build(filter->Input());
        // Use the physical (pruned) type, not the logical type.
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("filter input must have TStructType");
        }
        auto spec = NKernel::BuildFilterKernelSpec(*inputType, filter->Predicate());
        TKernelCompiler compiler(Diagnostics_);
        auto dispatch = compiler.CompileFilter(spec);
        return std::make_unique<TRuntimeUnaryStreamingKernel>(
            std::move(input),
            input->OutputType(),
            MakeFilterProcess(std::move(dispatch)));
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto project = maybe.Cast();
        auto input = Build(project->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("project input must have TStructType");
        }

        // Hybrid: ident projections stay zero-copy; computed projections go
        // through the project kernel into owned buffers.
        std::vector<TProjectColumn> columns;
        std::vector<NQumir::NAst::TExprPtr> computedExprs;
        std::vector<NQumir::NAst::TTypePtr> computedJitTypes;
        std::vector<size_t> computedWidths;
        std::vector<bool> computedIsString;
        std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
        for (const auto& projection : project->Projections()) {
            if (auto identNode = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
                    projection.Expression)) {
                const std::string& exprName = identNode.Cast()->Name;
                auto it = std::find_if(
                    inputType->Fields.begin(), inputType->Fields.end(),
                    [&](const auto& field) { return field.first == exprName; });
                if (it == inputType->Fields.end()) {
                    throw std::runtime_error("project column not found: " + exprName);
                }
                columns.push_back({.Computed = false,
                    .Index = static_cast<int32_t>(std::distance(inputType->Fields.begin(), it))});
                outFields.emplace_back(projection.Name, it->second);
            } else {
                auto outType = NKernel::InferProjectExprType(projection.Expression, *inputType);
                auto jitType = ProjectJitType(outType);
                using namespace NQumir::NAst;
                bool isStr = static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedType(UnwrapNullableType(outType))));
                columns.push_back({.Computed = true,
                    .Index = static_cast<int32_t>(computedExprs.size())});
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
                *inputType, computedExprs, computedJitTypes);
            TKernelCompiler compiler(Diagnostics_);
            dispatch = compiler.CompileProject(spec);
        }

        return std::make_unique<TRuntimeUnaryStreamingKernel>(
            std::move(input),
            std::make_shared<NQumir::NAst::TStructType>(std::move(outFields)),
            MakeProjectProcess(
                std::move(columns),
                std::move(dispatch),
                std::move(computedWidths),
                std::move(computedIsString)));
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

        auto spec = NKernel::BuildSortKernelSpec(*inputType, sort->Keys());
        PrintKernelSpec(Diagnostics_, spec);
        auto sortInputs = BuildSortKernelInputs(spec);
        TSortRadixKernel radixKernel;
        if (sortInputs.AllKeysRadixSortable && !sortInputs.RadixTypes.empty()) {
            TKernelCompiler compiler(Diagnostics_);
            radixKernel = {
                .Enabled = true,
                .Dispatch = compiler.CompileRadixSortComposite(sortInputs.RadixTypes),
                .NullableDispatch = compiler.CompileRadixSortCompositeNullable(
                    sortInputs.RadixTypes),
            };
        }

        auto outputType = input->OutputType();
        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            outputType,
            MakeSortProcess(
                outputType,
                sort->Keys(),
                std::move(sortInputs.KeyColumns),
                std::move(radixKernel)));
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto input = Build(sort->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("top-sort input must have TStructType");
        }

        auto spec = NKernel::BuildSortKernelSpec(*inputType, sort->Keys(), "top-sort");
        PrintKernelSpec(Diagnostics_, spec);
        auto sortInputs = BuildSortKernelInputs(spec);
        TSortRadixKernel radixKernel;
        if (sortInputs.AllKeysRadixSortable && !sortInputs.RadixTypes.empty()) {
            TKernelCompiler compiler(Diagnostics_);
            radixKernel = {
                .Enabled = true,
                .Dispatch = compiler.CompileRadixSortComposite(sortInputs.RadixTypes),
                .NullableDispatch = compiler.CompileRadixSortCompositeNullable(
                    sortInputs.RadixTypes),
            };
        }

        auto outputType = input->OutputType();
        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            outputType,
            MakeTopSortProcess(
                outputType,
                sort->Keys(),
                std::move(sortInputs.KeyColumns),
                std::move(radixKernel),
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

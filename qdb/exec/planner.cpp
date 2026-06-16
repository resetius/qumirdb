#include <qdb/exec/planner.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/source_exec.h>
#include <qdb/ops/aggregate.h>
#include <qdb/ops/source.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/join.h>
#include <qdb/ops/project.h>

#include <qdb/kernel/project_type.h>
#include <qdb/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <stdexcept>

namespace NQqb {

namespace {

// Byte width of a computed project column's physical type.
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
    throw std::runtime_error(
        "project: unsupported computed column type " +
        (type ? type->ToString() : std::string("<null>")));
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
    *Diagnostics_ << "unknown\n";
}

std::unique_ptr<IRuntimeNode> TPhysicalPlanner::Build(const TOperatorPtr& root) {
    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        auto src = maybe.Cast();
        // After column pruning, RequiredColumns() holds the narrowed struct.
        // If set, restrict the physical scan to those columns.
        if (auto required = src->RequiredColumns()) {
            auto* st = static_cast<NQumir::NAst::TStructType*>(required.get());
            std::unordered_set<std::string> cols;
            for (auto& [name, _] : st->Fields) cols.insert(name);
            src->GetSource().RestrictColumns(cols);
        }
        auto actualType = StructTypeFromSchema(src->GetSource().Schema());
        return std::make_unique<TRuntimeSource>(src->GetSource(), actualType);
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto filter = maybe.Cast();
        auto input = Build(filter->Input());
        // Use the physical (pruned) type, not the logical type.
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("filter input must have TStructType");
        }
        TKernelCompiler compiler(Diagnostics_);
        auto dispatch = compiler.CompileFilter(*inputType, filter->Predicate());
        return std::make_unique<TRuntimeFilter>(
            std::move(input),
            input->OutputType(),
            std::move(dispatch));
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
        std::vector<NQumir::NAst::TTypePtr> computedTypes;
        std::vector<size_t> computedWidths;
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
                auto type = NKernel::InferProjectExprType(projection.Expression, *inputType);
                columns.push_back({.Computed = true,
                    .Index = static_cast<int32_t>(computedExprs.size())});
                computedExprs.push_back(projection.Expression);
                computedTypes.push_back(type);
                computedWidths.push_back(ProjectColumnWidth(type));
                outFields.emplace_back(projection.Name, type);
            }
        }

        TKernelCompiler::TProjectDispatch dispatch;
        if (!computedExprs.empty()) {
            TKernelCompiler compiler(Diagnostics_);
            dispatch = compiler.CompileProject(*inputType, computedExprs, computedTypes);
        }

        return std::make_unique<TRuntimeProject>(
            std::move(input),
            std::make_shared<NQumir::NAst::TStructType>(std::move(outFields)),
            std::move(columns), std::move(dispatch), std::move(computedWidths));
    }

    if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
        auto agg = maybe.Cast();
        auto input = Build(agg->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("aggregate input must have TStructType");
        }

        TKernelCompiler compiler(Diagnostics_);
        auto kernels = compiler.CompileAggregate(*inputType, agg->GroupKeys(), agg->Aggs());

        // Output type from the physical (pruned) input type, not the logical
        // OutputColumns() (which was computed from the pre-pruning schema).
        auto outputType = ComputeAggregateOutputType(input->OutputType(), agg->GroupKeys(), agg->Aggs());

        return std::make_unique<TRuntimeAggregate>(
            std::move(input),
            std::move(outputType),
            std::move(kernels));
    }

    if (auto maybe = TMaybeOp<TJoinOperator>(root)) {
        auto join = maybe.Cast();
        if (join->Filter()) {
            throw std::runtime_error(
                "join residual filter is not supported yet (Stage 1)");
        }
        auto left = Build(join->Left());
        auto right = Build(join->Right());
        auto* leftType = static_cast<NQumir::NAst::TStructType*>(left->OutputType().get());
        auto* rightType = static_cast<NQumir::NAst::TStructType*>(right->OutputType().get());
        if (!leftType || !rightType) {
            throw std::runtime_error("join inputs must have TStructType");
        }

        std::vector<std::pair<std::string, std::string>> keys;
        keys.reserve(join->Keys().size());
        for (const auto& key : join->Keys()) {
            keys.emplace_back(key.Left, key.Right);
        }

        TKernelCompiler compiler(Diagnostics_);
        auto kernels = compiler.CompileJoin(
            *leftType, *rightType, keys, join->JoinType());

        // Output type from the physical (pruned) input types.
        auto outputType = ComputeJoinOutputType(
            left->OutputType(), right->OutputType(), join->JoinType());
        if (!outputType) {
            throw std::runtime_error("join: " + outputType.error().ToString());
        }

        return std::make_unique<TRuntimeJoin>(
            std::move(left), std::move(right), std::move(*outputType), std::move(kernels));
    }

    throw std::runtime_error("TPhysicalPlanner: unknown operator");
}

} // namespace NQqb

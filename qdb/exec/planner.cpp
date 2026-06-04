#include <qdb/exec/planner.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/source_exec.h>
#include <qdb/ops/source.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <stdexcept>

namespace NQqb {

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
        TKernelCompiler compiler;
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

        std::vector<int32_t> columnIndices;
        columnIndices.reserve(project->Projections().size());
        for (const auto& projection : project->Projections()) {
            auto identNode = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(projection.Expression);
            if (!identNode) {
                throw std::runtime_error("project expression kernels are not implemented yet: " + projection.Name);
            }
            const std::string& exprName = identNode.Cast()->Name;
            auto it = std::find_if(
                inputType->Fields.begin(),
                inputType->Fields.end(),
                [&](const auto& field) { return field.first == exprName; });
            if (it == inputType->Fields.end()) {
                throw std::runtime_error("project column not found: " + exprName);
            }
            columnIndices.push_back(static_cast<int32_t>(std::distance(inputType->Fields.begin(), it)));
        }

        return std::make_unique<TRuntimeProject>(
            std::move(input),
            project->OutputColumns(),
            std::move(columnIndices));
    }

    throw std::runtime_error("TPhysicalPlanner: unknown operator");
}

} // namespace NQqb

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
        return std::make_unique<TRuntimeSource>(src->GetSource(), src->Type);
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto filter = maybe.Cast();
        auto input = Build(filter->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(filter->Input()->Type.get());
        if (!inputType) {
            throw std::runtime_error("filter input must have TStructType");
        }
        TKernelCompiler compiler;
        auto dispatch = compiler.CompileFilter(*inputType, filter->Predicate());
        return std::make_unique<TRuntimeFilter>(
            std::move(input),
            filter->Type,
            std::move(dispatch));
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto project = maybe.Cast();
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(project->Input()->Type.get());
        if (!inputType) {
            throw std::runtime_error("project input must have TStructType");
        }

        std::vector<int32_t> columnIndices;
        columnIndices.reserve(project->Projections().size());
        for (const auto& projection : project->Projections()) {
            if (projection.Expression != projection.Name) {
                throw std::runtime_error("project expression kernels are not implemented yet: " + projection.Name);
            }
            auto it = std::find_if(
                inputType->Fields.begin(),
                inputType->Fields.end(),
                [&](const auto& field) { return field.first == projection.Expression; });
            if (it == inputType->Fields.end()) {
                throw std::runtime_error("project column not found: " + projection.Expression);
            }
            columnIndices.push_back(static_cast<int32_t>(std::distance(inputType->Fields.begin(), it)));
        }

        return std::make_unique<TRuntimeProject>(
            Build(project->Input()),
            project->Type,
            std::move(columnIndices));
    }

    throw std::runtime_error("TPhysicalPlanner: unknown operator");
}

} // namespace NQqb

#include <qdb/ops/source.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/pipeline/unbound_vars.h>

#include <qumir/parser/type.h>

#include <functional>

namespace NQqb {

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema) {
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    fields.reserve(schema.Columns.size());
    for (const auto& col : schema.Columns) {
        fields.emplace_back(std::string(col.Name), col.Type);
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TSourceOperator::TSourceOperator(ISource& source, std::string path)
    : Source_(source)
    , SourcePath_(std::move(path))
{
    Type = StructTypeFromSchema(source.Schema());
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

static std::unordered_set<std::string> CollectUsedColumns(const TOperatorPtr& op) {
    if (TMaybeOp<TSourceOperator>(op)) {
        return {};
    }
    if (auto maybe = TMaybeOp<TFilterOperator>(op)) {
        auto filter = maybe.Cast();
        auto cols = CollectUsedColumns(filter->Input());
        for (auto& c : FindUnboundVars(filter->Predicate())) {
            cols.insert(c);
        }
        return cols;
    }
    if (auto maybe = TMaybeOp<TProjectOperator>(op)) {
        auto project = maybe.Cast();
        auto cols = CollectUsedColumns(project->Input());
        for (const auto& proj : project->Projections()) {
            for (auto& c : FindUnboundVars(proj.Expression)) {
                cols.insert(c);
            }
        }
        return cols;
    }
    return {};
}

void ApplyColumnPruning(const TOperatorPtr& root) {
    auto used = CollectUsedColumns(root);
    if (used.empty()) {
        return;
    }

    std::function<void(const TOperatorPtr&)> walk = [&](const TOperatorPtr& op) {
        if (auto maybe = TMaybeOp<TSourceOperator>(op)) {
            maybe.Cast()->SetRequiredColumns(used);
            return;
        }
        for (const auto& child : op->Children()) {
            if (auto maybeOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
                walk(maybeOp.Cast());
            }
        }
    };
    walk(root);
}

} // namespace NQqb

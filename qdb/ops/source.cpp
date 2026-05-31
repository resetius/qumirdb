#include <qdb/ops/source.h>
#include <qdb/ops/used_columns.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>

#include <qumir/parser/type.h>

namespace NQqb {

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema) {
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    fields.reserve(schema.Columns.size());
    for (const auto& col : schema.Columns) {
        fields.emplace_back(std::string(col.Name), col.Type);
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TSourceOperator::TSourceOperator(ISource& source)
    : Source_(source)
{
    Type = StructTypeFromSchema(source.Schema());
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

void ApplyColumnPruning(const TOperatorPtr& root) {
    auto used = CollectUsedColumns(root);
    if (used.empty()) {
        return;
    }

    // Walk to every TSourceOperator in the subtree and set required columns.
    // For a linear plan (no joins yet) there is exactly one source.
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

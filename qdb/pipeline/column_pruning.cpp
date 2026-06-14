#include <qdb/pipeline/column_pruning.h>

#include <qdb/ops/aggregate.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

#include <qumir/parser/type.h>

#include <functional>
#include <unordered_set>

namespace NQqb {

using namespace NQumir::NAst;

void ApplyColumnPruning(const TOperatorPtr& root) {
    // Seed: the root's consumer needs all of its output columns.
    std::unordered_set<std::string> initial;
    if (auto* st = static_cast<TStructType*>(root->OutputColumns().get())) {
        for (auto& [name, _] : st->Fields) {
            initial.insert(name);
        }
    }

    auto narrowStruct = [](const TTypePtr& type,
        const std::unordered_set<std::string>& keep) -> TTypePtr {
        auto* st = static_cast<TStructType*>(type.get());
        if (!st) {
            return type;
        }
        std::vector<std::pair<std::string, TTypePtr>> fields;
        for (auto& [name, fieldType] : st->Fields) {
            if (keep.contains(name)) {
                fields.emplace_back(name, fieldType);
            }
        }
        return std::make_shared<TStructType>(std::move(fields));
    };

    std::function<void(const TOperatorPtr&, std::unordered_set<std::string>)>
        walk = [&](const TOperatorPtr& op, std::unordered_set<std::string> needed) {

        auto* fun = static_cast<TFunctionType*>(op->Type.get());

        if (TMaybeOp<TSourceOperator>(op)) {
            // Source has no upstream: required = needed ∪ own refs. Narrow both
            // ReturnType and ParamTypes[0] so downstream sees only what's read.
            auto required = op->RequiredColumnsForChild(0, needed);
            if (fun) {
                auto narrowed = narrowStruct(fun->ReturnType, required);
                fun->ParamTypes = {narrowed};
                fun->ReturnType = narrowed;
            }
            return;
        }

        // Each child gets its own required set; narrow the matching ParamTypes[i].
        auto children = op->Children();
        for (size_t i = 0; i < children.size(); ++i) {
            auto required = op->RequiredColumnsForChild(i, needed);
            if (fun && i < fun->ParamTypes.size()) {
                fun->ParamTypes[i] = narrowStruct(fun->ParamTypes[i], required);
            }
            if (auto maybeOp = TMaybeNode<IOperator>(children[i])) {
                walk(maybeOp.Cast(), std::move(required));
            }
        }
    };

    walk(root, initial);
}

} // namespace NQqb

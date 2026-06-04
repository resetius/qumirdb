#include <qdb/pipeline/column_pruning.h>

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

    std::function<void(const TOperatorPtr&, std::unordered_set<std::string>)>
        walk = [&](const TOperatorPtr& op, std::unordered_set<std::string> needed) {

        auto own = op->ComputeReferencedColumns();

        std::unordered_set<std::string> required;
        if (TMaybeOp<TProjectOperator>(op)) {
            // Project defines a new schema: its required input = only what its
            // projection expressions reference.
            required = std::move(own);
        } else {
            // Filter/source pass rows through: must satisfy own refs + parent needs.
            required = std::move(needed);
            required.insert(own.begin(), own.end());
        }

        // Narrow ParamTypes[0] to the required subset of the input schema.
        auto* fun = static_cast<TFunctionType*>(op->Type.get());
        if (fun && !fun->ParamTypes.empty()) {
            if (auto* param = static_cast<TStructType*>(fun->ParamTypes[0].get())) {
                std::vector<std::pair<std::string, TTypePtr>> fields;
                for (auto& [name, type] : param->Fields) {
                    if (required.contains(name)) {
                        fields.emplace_back(name, type);
                    }
                }
                fun->ParamTypes[0] = std::make_shared<TStructType>(std::move(fields));
            }
        }

        if (TMaybeOp<TSourceOperator>(op)) {
            // Source has no upstream: store required columns in ParamTypes[0]
            // and narrow ReturnType so downstream sees only what's actually read.
            auto* fun = static_cast<TFunctionType*>(op->Type.get());
            if (auto* full = static_cast<TStructType*>(fun->ReturnType.get())) {
                std::vector<std::pair<std::string, TTypePtr>> fields;
                for (auto& [name, type] : full->Fields) {
                    if (required.contains(name)) {
                        fields.emplace_back(name, type);
                    }
                }
                auto narrowed = std::make_shared<TStructType>(std::move(fields));
                fun->ParamTypes = {narrowed};
                fun->ReturnType = narrowed;
            }
            return;
        }

        for (const auto& child : op->Children()) {
            if (auto maybeOp = TMaybeNode<IOperator>(child)) {
                walk(maybeOp.Cast(), required);
            }
        }
    };

    walk(root, initial);
}

} // namespace NQqb

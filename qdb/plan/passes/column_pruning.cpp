#include <qdb/plan/passes/column_pruning.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>

#include <qumir/parser/type.h>

#include <cassert>
#include <functional>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

bool HasOutputColumn(const TOperatorPtr& op, const std::string& name) {
    if (auto* st = static_cast<TStructType*>(op->OutputColumns().get())) {
        for (auto& [field, _] : st->Fields) {
            if (field == name) {
                return true;
            }
        }
    }
    return false;
}

void Prune(const TOperatorPtr& root, const TColumnSet* explicitRootDemand, TCteUsageMap* usage) {
    TColumnSet initial;
    if (explicitRootDemand) {
        initial = *explicitRootDemand;
    } else if (auto* st = static_cast<TStructType*>(root->OutputColumns().get())) {
        for (auto& [name, _] : st->Fields) {
            initial.insert(name);
        }
    }

    auto narrowStruct = [](const TTypePtr& type, const TColumnSet& keep) -> TTypePtr {
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

    std::function<void(const TOperatorPtr&, TColumnSet)> walk =
        [&](const TOperatorPtr& op, TColumnSet needed) {

        auto* fun = static_cast<TFunctionType*>(op->Type.get());

        if (TMaybeOp<TSourceOperator>(op)) {
            auto required = op->RequiredColumnsForChild(0, needed);
            if (fun) {
                auto narrowed = narrowStruct(fun->ReturnType, required);
                fun->ParamTypes = {narrowed};
                fun->ReturnType = narrowed;
            }
            return;
        }

        if (auto ref = TMaybeOp<TCteRef>(op)) {
            if (usage) {
                auto& info = (*usage)[ref.Cast()->Def().get()];
                ++info.StaticRefCount;
                for (const auto& column : needed) {
                    if (HasOutputColumn(op, column)) {
                        info.RequiredOutputs.insert(column);
                    }
                }
            }
            return;
        }

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

} // namespace

void ApplyColumnPruning(const TOperatorPtr& root) {
    Prune(root, nullptr, nullptr);
}

void ApplyColumnPruning(
    const TOperatorPtr& root, const TColumnSet* explicitRootDemand, TCteUsageMap* usage) {
    Prune(root, explicitRootDemand, usage);
}

TCteUsageMap PropagateCteDemands(const TOperatorPtr& main) {
    TCteUsageMap usage;
    ApplyColumnPruning(main, nullptr, &usage);
    auto defs = CollectCteDefinitions(main); // dependency-first
    for (auto it = defs.rbegin(); it != defs.rend(); ++it) {
        // Every collected definition was reached through a TCteRef, so its usage
        // (with a non-zero refcount) must already be recorded.
        auto u = usage.find(it->get());
        assert(u != usage.end() && u->second.StaticRefCount > 0);
        ApplyColumnPruning((*it)->Plan, &u->second.RequiredOutputs, &usage);
    }
    return usage;
}

} // namespace NQdb

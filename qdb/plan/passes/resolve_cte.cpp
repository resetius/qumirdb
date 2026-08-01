#include <qdb/plan/passes/resolve_cte.h>

#include <qdb/plan/clone_operator.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>

#include <unordered_map>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

TOperatorPtr Resolve(const TOperatorPtr& op,
                     std::unordered_map<TCteDefinition*, TOperatorPtr>& memo) {
    if (!op) {
        return op;
    }
    if (auto maybeRef = TMaybeOp<TCteRef>(op)) {
        auto def = maybeRef.Cast()->Def();
        auto it = memo.find(def.get());
        if (it == memo.end()) {
            // Resolve a clone so the canonical definition (its nested TCteRefs)
            // stays intact for the reuse phase.
            it = memo.emplace(def.get(), Resolve(CloneOperator(def->Plan), memo)).first;
        }
        return CloneOperator(it->second);
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        auto filter = maybeFilter.Cast();
        filter->MutableInput() = Resolve(filter->Input(), memo);
    } else if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        auto project = maybeProject.Cast();
        project->MutableInput() = Resolve(project->Input(), memo);
    } else if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(op)) {
        auto aggregate = maybeAggregate.Cast();
        aggregate->MutableInput() = Resolve(aggregate->Input(), memo);
    } else if (auto maybeSort = TMaybeOp<TSortOperator>(op)) {
        auto sort = maybeSort.Cast();
        sort->MutableInput() = Resolve(sort->Input(), memo);
    } else if (auto maybeTopSort = TMaybeOp<TTopSortOperator>(op)) {
        auto topSort = maybeTopSort.Cast();
        topSort->MutableInput() = Resolve(topSort->Input(), memo);
    } else if (auto maybeLimit = TMaybeOp<TLimitOperator>(op)) {
        auto limit = maybeLimit.Cast();
        limit->MutableInput() = Resolve(limit->Input(), memo);
    } else if (auto maybeWindow = TMaybeOp<TWindowOperator>(op)) {
        auto window = maybeWindow.Cast();
        window->MutableInput() = Resolve(window->Input(), memo);
    } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        auto join = maybeJoin.Cast();
        join->MutableLeft() = Resolve(join->Left(), memo);
        join->MutableRight() = Resolve(join->Right(), memo);
    } else if (auto maybeUnion = TMaybeOp<TUnionAllOperator>(op)) {
        for (auto& branch : maybeUnion.Cast()->MutableInputs()) {
            branch = Resolve(branch, memo);
        }
    }
    return op;
}

} // namespace

TOperatorPtr ResolveCteRefs(TOperatorPtr plan) {
    std::unordered_map<TCteDefinition*, TOperatorPtr> memo;
    return Resolve(std::move(plan), memo);
}

} // namespace NQdb

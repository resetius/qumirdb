#include <qdb/plan/passes/cte_reuse.h>

#include <qdb/plan/clone_operator.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>

#include <cassert>
#include <unordered_map>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

using TMemo = std::unordered_map<TCteDefinition*, TOperatorPtr>;

TOperatorPtr Resolve(const TOperatorPtr& op, const TCteReuseDecisions& decisions, TMemo& memo);

TOperatorPtr ResolveInline(
    const TCteDefinitionPtr& def, const TCteReuseDecisions& decisions, TMemo& memo) {
    auto it = memo.find(def.get());
    if (it == memo.end()) {
        // Resolve a clone so the canonical definition (its nested TCteRefs) stays
        // intact for the reuse phase.
        it = memo.emplace(
            def.get(), Resolve(CloneOperator(def->Plan), decisions, memo)).first;
    }
    return CloneOperator(it->second);
}

TOperatorPtr Resolve(const TOperatorPtr& op, const TCteReuseDecisions& decisions, TMemo& memo) {
    if (!op) {
        return op;
    }
    if (auto maybeRef = TMaybeOp<TCteRef>(op)) {
        auto def = maybeRef.Cast()->Def();
        const auto& decision = decisions.at(def.get());
        switch (decision.Mode) {
            case ECteReuseMode::Inline:
                return ResolveInline(def, decisions, memo);
            case ECteReuseMode::Materialize:
                // TODO(phase 5b): return std::make_shared<TCteConsumer>(def,
                // decision.RequiredOutputs) to stop inlining and share one producer.
                return ResolveInline(def, decisions, memo);
        }
        std::unreachable();
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        auto filter = maybeFilter.Cast();
        filter->MutableInput() = Resolve(filter->Input(), decisions, memo);
    } else if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        auto project = maybeProject.Cast();
        project->MutableInput() = Resolve(project->Input(), decisions, memo);
    } else if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(op)) {
        auto aggregate = maybeAggregate.Cast();
        aggregate->MutableInput() = Resolve(aggregate->Input(), decisions, memo);
    } else if (auto maybeSort = TMaybeOp<TSortOperator>(op)) {
        auto sort = maybeSort.Cast();
        sort->MutableInput() = Resolve(sort->Input(), decisions, memo);
    } else if (auto maybeTopSort = TMaybeOp<TTopSortOperator>(op)) {
        auto topSort = maybeTopSort.Cast();
        topSort->MutableInput() = Resolve(topSort->Input(), decisions, memo);
    } else if (auto maybeLimit = TMaybeOp<TLimitOperator>(op)) {
        auto limit = maybeLimit.Cast();
        limit->MutableInput() = Resolve(limit->Input(), decisions, memo);
    } else if (auto maybeWindow = TMaybeOp<TWindowOperator>(op)) {
        auto window = maybeWindow.Cast();
        window->MutableInput() = Resolve(window->Input(), decisions, memo);
    } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        auto join = maybeJoin.Cast();
        join->MutableLeft() = Resolve(join->Left(), decisions, memo);
        join->MutableRight() = Resolve(join->Right(), decisions, memo);
    } else if (auto maybeUnion = TMaybeOp<TUnionAllOperator>(op)) {
        for (auto& branch : maybeUnion.Cast()->MutableInputs()) {
            branch = Resolve(branch, decisions, memo);
        }
    }
    return op;
}

} // namespace

TCteReuseDecisions ChooseCteReuse(const TCteUsageMap& usage) {
    TCteReuseDecisions decisions;
    for (const auto& [def, info] : usage) {
        assert(info.StaticRefCount > 0);
        decisions.emplace(def, TCteReuseDecision{
            .Mode = info.StaticRefCount >= 2
                ? ECteReuseMode::Materialize
                : ECteReuseMode::Inline,
            .RequiredOutputs = info.RequiredOutputs,
        });
    }
    return decisions;
}

TOperatorPtr ApplyCteReuse(TOperatorPtr plan, const TCteReuseDecisions& decisions) {
    // TODO(phase 5b): Materialize -> TCteConsumer + shared producer.
    std::unordered_map<TCteDefinition*, TOperatorPtr> memo;
    return Resolve(std::move(plan), decisions, memo);
}

} // namespace NQdb

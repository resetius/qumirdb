#include <qdb/plan/passes/cte_reuse.h>

#include <qdb/plan/clone_operator.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_consumer.h>
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
using TMaterializedMap = std::unordered_map<TCteDefinition*, TCteMaterializationPtr>;

struct TResolveState {
    const TCteReuseDecisions& Decisions;
    TMemo Inlined;
    TMaterializedMap Materialized;
};

TOperatorPtr Resolve(const TOperatorPtr& op, TResolveState& state);

TOperatorPtr ResolveInline(const TCteDefinitionPtr& def, TResolveState& state) {
    auto it = state.Inlined.find(def.get());
    if (it == state.Inlined.end()) {
        // Resolve a clone so the canonical definition (its nested TCteRefs) stays
        // intact for the reuse phase.
        it = state.Inlined.emplace(
            def.get(), Resolve(CloneOperator(def->Plan), state)).first;
    }
    return CloneOperator(it->second);
}

TOperatorPtr ResolveMaterialized(const TCteDefinitionPtr& def, TResolveState& state) {
    auto it = state.Materialized.find(def.get());
    TCteMaterializationPtr mat;
    if (it != state.Materialized.end()) {
        mat = it->second;
    } else {
        mat = std::make_shared<TCteMaterialization>();
        state.Materialized.emplace(def.get(), mat);
        mat->Plan = Resolve(CloneOperator(def->Plan), state);
    }
    return std::make_shared<TCteConsumer>(def, mat);
}

TOperatorPtr Resolve(const TOperatorPtr& op, TResolveState& state) {
    if (!op) {
        return op;
    }
    if (auto maybeRef = TMaybeOp<TCteRef>(op)) {
        auto def = maybeRef.Cast()->Def();
        switch (state.Decisions.at(def.get()).Mode) {
            case ECteReuseMode::Inline:
                return ResolveInline(def, state);
            case ECteReuseMode::Materialize:
                return ResolveMaterialized(def, state);
        }
        std::unreachable();
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        auto filter = maybeFilter.Cast();
        filter->MutableInput() = Resolve(filter->Input(), state);
    } else if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        auto project = maybeProject.Cast();
        project->MutableInput() = Resolve(project->Input(), state);
    } else if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(op)) {
        auto aggregate = maybeAggregate.Cast();
        aggregate->MutableInput() = Resolve(aggregate->Input(), state);
    } else if (auto maybeSort = TMaybeOp<TSortOperator>(op)) {
        auto sort = maybeSort.Cast();
        sort->MutableInput() = Resolve(sort->Input(), state);
    } else if (auto maybeTopSort = TMaybeOp<TTopSortOperator>(op)) {
        auto topSort = maybeTopSort.Cast();
        topSort->MutableInput() = Resolve(topSort->Input(), state);
    } else if (auto maybeLimit = TMaybeOp<TLimitOperator>(op)) {
        auto limit = maybeLimit.Cast();
        limit->MutableInput() = Resolve(limit->Input(), state);
    } else if (auto maybeWindow = TMaybeOp<TWindowOperator>(op)) {
        auto window = maybeWindow.Cast();
        window->MutableInput() = Resolve(window->Input(), state);
    } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        auto join = maybeJoin.Cast();
        join->MutableLeft() = Resolve(join->Left(), state);
        join->MutableRight() = Resolve(join->Right(), state);
    } else if (auto maybeUnion = TMaybeOp<TUnionAllOperator>(op)) {
        for (auto& branch : maybeUnion.Cast()->MutableInputs()) {
            branch = Resolve(branch, state);
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
    TResolveState state{.Decisions = decisions};
    auto resolved = Resolve(std::move(plan), state);
    AssignMaterializationRefCounts(resolved);
    return resolved;
}

} // namespace NQdb

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

#include <stdexcept>
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

// Narrows the spool to the union of columns demanded by all consumers, in the
// producer's own field order.
TOperatorPtr WrapBoundaryProjection(
    TOperatorPtr plan, const std::unordered_set<std::string>& required) {
    auto* schema = static_cast<TStructType*>(plan->OutputColumns().get());
    if (!schema) {
        throw std::runtime_error(
            "cte boundary projection: producer output is not a struct");
    }
    if (required.empty()) {
        // Rowsets need >=1 physical column; keep the full schema until a
        // row-count carrier exists.
        return plan;
    }

    std::vector<TProjectionSpec> specs;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    for (const auto& [name, type] : schema->Fields) {
        if (!required.contains(name)) {
            continue;
        }
        specs.push_back(TProjectionSpec{
            .Name = name,
            .Expression = std::make_shared<TIdentExpr>(NQumir::TLocation{}, name),
        });
        fields.emplace_back(name, type);
    }
    // Every demanded column must exist in the producer schema; a mismatch means
    // demand analysis and the producer plan have diverged.
    if (specs.size() != required.size()) {
        throw std::runtime_error(
            "cte boundary projection: demanded column missing from producer schema");
    }
    if (specs.size() == schema->Fields.size()) {
        return plan;
    }

    auto project = std::make_shared<TProjectOperator>(std::move(plan), std::move(specs));
    project->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{project->Input()->OutputColumns()},
        std::make_shared<TStructType>(std::move(fields)));
    return project;
}

TOperatorPtr ResolveMaterialized(const TCteDefinitionPtr& def, TResolveState& state) {
    auto it = state.Materialized.find(def.get());
    TCteMaterializationPtr mat;
    if (it != state.Materialized.end()) {
        mat = it->second;
    } else {
        mat = std::make_shared<TCteMaterialization>();
        state.Materialized.emplace(def.get(), mat);
        mat->Plan = WrapBoundaryProjection(
            Resolve(CloneOperator(def->Plan), state),
            state.Decisions.at(def.get()).RequiredOutputs);
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
        if (info.StaticRefCount == 0) {
            throw std::runtime_error("cte reuse: definition recorded with no references");
        }
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

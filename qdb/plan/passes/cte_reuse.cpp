#include <qdb/plan/passes/cte_reuse.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/clone_operator.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/flatten_conjuncts.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>

#include <qumir/parser/core/printer.h>

#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

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

namespace {

// Spool traffic cost per byte, in the abstract byte-work units EstimateStats
// uses for Cost. Rough placeholders — they still need calibration against the
// real memory-vs-scan cost ratio.
constexpr double SpoolWriteCostPerByte = 1.0;
constexpr double SpoolReadCostPerByte = 1.0;

// Bytes actually spooled: the boundary projection narrows the producer to the
// demanded columns (or the full schema when demand is empty), so cost the spool
// over RequiredOutputs, not the definition's full output.
double OutputBytes(const TCteDefinition& def, const std::unordered_set<std::string>& required) {
    const TStatsPtr stats = def.Plan ? def.Plan->Stats_ : nullptr;
    const double rows = stats ? static_cast<double>(stats->RowCount) : 0.0;
    const bool keepFullSchema = required.empty();
    double width = 0.0;
    auto* schema = static_cast<TStructType*>(def.Plan->OutputColumns().get());
    assert(schema); // a null schema would silently zero the spool cost
    if (schema) {
        for (const auto& [name, type] : schema->Fields) {
            if (keepFullSchema || required.contains(name)) {
                width += EstimateTypeWidth(type);
            }
        }
    }
    return rows * width;
}

// Inline re-executes the definition per reference; materialize runs it once and
// pays spool traffic (one write + one read per consumer). Pick whichever is
// cheaper. Without a cost estimate keep the safe reuse rule (materialize).
//
// StaticRefCount is the *syntactic* reference count, and decisions are made
// independently on pre-reuse costs. Not modeled yet: nested inline multiplicity
// (inlining a parent multiplies how often its dependencies run) and the reverse
// (materializing a dependency lowers a parent's real cost). A later pass can
// compute effective multiplicity and recompute decisions dependency-aware.
ECteReuseMode ChooseMode(const TCteDefinition& def, const TCteUsageInfo& usage) {
    if (usage.StaticRefCount < 2) {
        return ECteReuseMode::Inline;
    }
    const TStatsPtr stats = def.Plan ? def.Plan->Stats_ : nullptr;
    if (!stats) {
        return ECteReuseMode::Materialize;
    }
    const double n = static_cast<double>(usage.StaticRefCount);
    const double bytes = OutputBytes(def, usage.RequiredOutputs);
    const double inlineCost = n * stats->Cost;
    const double materializeCost = stats->Cost
        + bytes * (SpoolWriteCostPerByte + n * SpoolReadCostPerByte);
    return materializeCost < inlineCost
        ? ECteReuseMode::Materialize
        : ECteReuseMode::Inline;
}

} // namespace

namespace {

using TRefPredicates = std::unordered_map<TCteDefinition*, std::vector<TExprPtr>>;

// One entry per reference; nullptr marks a reference with no filter directly
// above it (needs every row, so nothing is shareable for its definition).
void CollectRefPredicates(const TOperatorPtr& op, TRefPredicates& out) {
    if (!op) {
        return;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        if (auto ref = TMaybeOp<TCteRef>(filter.Cast()->Input())) {
            out[ref.Cast()->Def().get()].push_back(filter.Cast()->Predicate());
            return;
        }
    }
    if (auto ref = TMaybeOp<TCteRef>(op)) {
        out[ref.Cast()->Def().get()].push_back(nullptr);
        return;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = TMaybeNode<IOperator>(child)) {
            CollectRefPredicates(childOp.Cast(), out);
        }
    }
}

TExprPtr Reduce(std::vector<TExprPtr> parts, const char* op) {
    TExprPtr acc;
    for (auto& part : parts) {
        acc = acc
            ? std::make_shared<TBinaryExpr>(NQumir::TLocation{}, TOperator(op), acc, part)
            : std::move(part);
    }
    return acc;
}

// Per column, OR the references' constraints; AND the columns. A column
// qualifies only if every reference constrains it with a single-column conjunct.
TExprPtr BuildPushablePredicate(const TCteDefinition& def, const std::vector<TExprPtr>& refs) {
    const size_t n = refs.size();
    if (n < 2) {
        return nullptr;
    }
    for (const auto& pred : refs) {
        if (!pred) {
            return nullptr;
        }
    }
    auto* schema = static_cast<TStructType*>(def.OutputType.get());
    if (!schema) {
        return nullptr;
    }
    std::unordered_set<std::string> outputs;
    for (const auto& [name, type] : schema->Fields) {
        outputs.insert(name);
    }

    std::vector<std::unordered_map<std::string, std::vector<TExprPtr>>> byColumn(n);
    for (size_t i = 0; i < n; ++i) {
        std::vector<TExprPtr> conjuncts;
        FlattenConjuncts(refs[i], conjuncts);
        for (const auto& conj : conjuncts) {
            auto vars = FindUnboundVars(conj);
            if (vars.size() != 1) {
                continue;
            }
            const std::string& col = *vars.begin();
            if (outputs.contains(col)) {
                byColumn[i][col].push_back(conj);
            }
        }
    }

    std::vector<TExprPtr> columnPredicates;
    for (const auto& [name, type] : schema->Fields) {
        bool constrainedEverywhere = true;
        for (size_t i = 0; i < n; ++i) {
            if (!byColumn[i].contains(name)) {
                constrainedEverywhere = false;
                break;
            }
        }
        if (!constrainedEverywhere) {
            continue;
        }
        std::vector<TExprPtr> perRef;
        std::unordered_set<std::string> seen; // distinct constraints only
        for (size_t i = 0; i < n; ++i) {
            std::vector<TExprPtr> cloned;
            for (const auto& conj : byColumn[i].at(name)) {
                cloned.push_back(CloneExpr(conj));
            }
            auto conjoined = Reduce(std::move(cloned), "&&");
            if (seen.insert(NQumir::NAst::NCore::PrintAst(conjoined)).second) {
                perRef.push_back(std::move(conjoined));
            }
        }
        columnPredicates.push_back(Reduce(std::move(perRef), "||"));
    }
    return Reduce(std::move(columnPredicates), "&&");
}

} // namespace

void PushConsumerPredicatesIntoDefinitions(
    const TOperatorPtr& main,
    const NKernel::TAnnotationContext& context)
{
    TRefPredicates refs;
    CollectRefPredicates(main, refs);
    auto defs = CollectCteDefinitions(main);
    for (const auto& def : defs) {
        CollectRefPredicates(def->Plan, refs);
    }
    for (const auto& def : defs) {
        auto it = refs.find(def.get());
        if (it == refs.end()) {
            continue;
        }
        auto pushable = BuildPushablePredicate(*def, it->second);
        if (!pushable) {
            continue;
        }
        def->Plan = PushDownPredicates(
            std::make_shared<TFilterOperator>(def->Plan, std::move(pushable)));
        AnnotateTypes(def->Plan, context);
        EstimateStats(def->Plan);
    }
}

TCteReuseDecisions ChooseCteReuse(const TCteUsageMap& usage) {
    TCteReuseDecisions decisions;
    for (const auto& [def, info] : usage) {
        if (info.StaticRefCount == 0) {
            throw std::runtime_error("cte reuse: definition recorded with no references");
        }
        decisions.emplace(def, TCteReuseDecision{
            .Mode = ChooseMode(*def, info),
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

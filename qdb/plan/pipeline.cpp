#include <qdb/plan/pipeline.h>

#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/flatten_joins.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/cte_reuse.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/plan_print.h>

#include <cstdlib>
#include <iostream>

namespace NQdb {

namespace {

void OptimizeOne(TOperatorPtr& plan, TPlanPassOptions options) {
    // QDB_DUMP_PASSES=1 dumps the plan after each pass to stderr (debugging).
    const bool dump = std::getenv("QDB_DUMP_PASSES") != nullptr;
    auto stage = [&](const char* name) {
        if (dump) {
            std::cerr << "\n===== after " << name << " =====\n";
            PrintPlanTree(std::cerr, plan);
        }
    };
    if (dump) { std::cerr << "\n===== initial =====\n"; PrintPlanTree(std::cerr, plan); }

    AssignSourceAliases(plan); stage("AssignSourceAliases");
    QualifyColumns(plan); stage("QualifyColumns");
    AnnotateTypes(plan);
    if (options.EnableCbo) {
        plan = FlattenInnerJoins(plan); stage("FlattenInnerJoins");
        AnnotateTypes(plan); // re-annotate: inner-join regions were reassociated
    }
    plan = PushDownPredicates(plan); stage("PushDownPredicates");
    AnnotateTypes(plan); // re-annotate: pushdown moved filters onto leaves
    EstimateStats(plan); // leaf cardinalities for cost-based ReorderJoins
    plan = ReorderJoins(
        plan,
        options.EnableCbo,
        options.Diagnostics ? &options.Diagnostics->JoinReorder : nullptr);
    stage("ReorderJoins");
    AnnotateTypes(plan); // re-annotate: reordering rebuilt the join tree
    plan = ExtractEquiJoins(plan); stage("ExtractEquiJoins");
    AnnotateTypes(plan); // re-annotate: equi-join extraction adds/removes nodes
    plan = PushDownSemiJoins(plan); stage("PushDownSemiJoins");
    AnnotateTypes(plan); // re-annotate: semi pushdown restructured joins
    plan = ApplyTopSort(plan);
    AnnotateTypes(plan); // re-annotate: top-sort replaces limit(sort(...))
    CoerceSetOpBranches(plan); stage("CoerceSetOpBranches");
    AnnotateTypes(plan); // re-annotate: coercion added cast projections on union branches
    ApplyColumnPruning(plan); stage("ApplyColumnPruning");

    EstimateStats(plan);
}

} // namespace

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options) {
    for (const auto& def : CollectCteDefinitions(plan)) {
        OptimizeOne(def->Plan, options);
        def->OutputType = def->Plan->OutputColumns();
    }
    OptimizeOne(plan, options);
    auto usage = PropagateCteDemands(plan);
    // PropagateCteDemands narrowed definition schemas; refresh their cost
    // (dependency-first, so nested definitions are estimated before their users)
    // for the cost-based reuse decision.
    for (const auto& def : CollectCteDefinitions(plan)) {
        EstimateStats(def->Plan);
    }
    auto decisions = ChooseCteReuse(usage);
    plan = ApplyCteReuse(std::move(plan), decisions);
    if (std::getenv("QDB_DUMP_PASSES") != nullptr) {
        std::cerr << "\n===== after ApplyCteReuse =====\n";
        PrintPlanTreeWithCtes(std::cerr, plan);
    }
}

} // namespace NQdb

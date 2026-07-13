#include <qdb/plan/pipeline.h>

#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/typing.h>

namespace NQdb {

void ApplyPlanPasses(TOperatorPtr& plan) {
    AssignSourceAliases(plan);
    QualifyColumns(plan);
    AnnotateTypes(plan);
    plan = PushDownPredicates(plan);
    AnnotateTypes(plan); // re-annotate: pushdown moved filters onto leaves
    plan = ReorderJoins(plan);
    AnnotateTypes(plan); // re-annotate: reordering rebuilt the join tree
    plan = ExtractEquiJoins(plan);
    AnnotateTypes(plan); // re-annotate: equi-join extraction adds/removes nodes
    plan = PushDownSemiJoins(plan);
    AnnotateTypes(plan); // re-annotate: semi pushdown restructured joins
    plan = ApplyTopSort(plan);
    AnnotateTypes(plan); // re-annotate: top-sort replaces limit(sort(...))
    ApplyColumnPruning(plan);

    EstimateStats(plan);
}

} // namespace NQdb

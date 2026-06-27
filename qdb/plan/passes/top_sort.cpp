#include <qdb/plan/passes/top_sort.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>

namespace NQdb {

TOperatorPtr ApplyTopSort(const TOperatorPtr& root) {
    if (!root) {
        return root;
    }

    if (auto limit = TMaybeOp<TLimitOperator>(root)) {
        limit.Cast()->MutableInput() = ApplyTopSort(limit.Cast()->Input());
        if (limit.Cast()->Offset() == 0) {
            if (auto sort = TMaybeOp<TSortOperator>(limit.Cast()->Input())) {
                return std::make_shared<TTopSortOperator>(
                    sort.Cast()->Input(), sort.Cast()->Keys(), limit.Cast()->Limit());
            }
        }
        return root;
    }

    if (auto sort = TMaybeOp<TSortOperator>(root)) {
        sort.Cast()->MutableInput() = ApplyTopSort(sort.Cast()->Input());
        return root;
    }
    if (auto topSort = TMaybeOp<TTopSortOperator>(root)) {
        topSort.Cast()->MutableInput() = ApplyTopSort(topSort.Cast()->Input());
        return root;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(root)) {
        filter.Cast()->MutableInput() = ApplyTopSort(filter.Cast()->Input());
        return root;
    }
    if (auto project = TMaybeOp<TProjectOperator>(root)) {
        project.Cast()->MutableInput() = ApplyTopSort(project.Cast()->Input());
        return root;
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(root)) {
        aggregate.Cast()->MutableInput() = ApplyTopSort(aggregate.Cast()->Input());
        return root;
    }
    if (auto join = TMaybeOp<TJoinOperator>(root)) {
        join.Cast()->MutableLeft() = ApplyTopSort(join.Cast()->Left());
        join.Cast()->MutableRight() = ApplyTopSort(join.Cast()->Right());
        return root;
    }

    return root;
}

} // namespace NQdb

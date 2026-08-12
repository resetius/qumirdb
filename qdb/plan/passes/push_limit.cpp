#include <qdb/plan/passes/push_limit.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>

namespace NQdb {

TOperatorPtr PushDownLimits(const TOperatorPtr& root) {
    if (!root) {
        return root;
    }

    if (auto limit = TMaybeOp<TLimitOperator>(root)) {
        auto input = PushDownLimits(limit.Cast()->Input());
        if (auto project = TMaybeOp<TProjectOperator>(input)) {
            auto pushed = std::make_shared<TLimitOperator>(
                project.Cast()->Input(),
                limit.Cast()->Limit(),
                limit.Cast()->Offset());
            project.Cast()->MutableInput() = PushDownLimits(pushed);
            return project.Cast();
        }
        limit.Cast()->MutableInput() = input;
        return root;
    }

    if (auto project = TMaybeOp<TProjectOperator>(root)) {
        project.Cast()->MutableInput() = PushDownLimits(project.Cast()->Input());
        return root;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(root)) {
        filter.Cast()->MutableInput() = PushDownLimits(filter.Cast()->Input());
        return root;
    }
    if (auto sort = TMaybeOp<TSortOperator>(root)) {
        sort.Cast()->MutableInput() = PushDownLimits(sort.Cast()->Input());
        return root;
    }
    if (auto topSort = TMaybeOp<TTopSortOperator>(root)) {
        topSort.Cast()->MutableInput() = PushDownLimits(topSort.Cast()->Input());
        return root;
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(root)) {
        aggregate.Cast()->MutableInput() = PushDownLimits(aggregate.Cast()->Input());
        return root;
    }
    if (auto join = TMaybeOp<TJoinOperator>(root)) {
        join.Cast()->MutableLeft() = PushDownLimits(join.Cast()->Left());
        join.Cast()->MutableRight() = PushDownLimits(join.Cast()->Right());
        return root;
    }

    return root;
}

} // namespace NQdb

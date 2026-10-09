#include <qdb/plan/passes/top_sort.h>

#include <qdb/plan/ops/limit.h>
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

    for (auto& input : root->MutableInputs()) {
        input = ApplyTopSort(input);
    }

    return root;
}

} // namespace NQdb

#include <qdb/plan/passes/push_limit.h>

#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>

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

    for (auto& input : root->MutableInputs()) {
        input = PushDownLimits(input);
    }

    return root;
}

} // namespace NQdb

#include <qdb/plan/passes/row_group_predicate.h>

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/predicate_requirements.h>

namespace NQdb {

using NQumir::NAst::TMaybeNode;

namespace {

void Clear(const TOperatorPtr& node) {
    if (!node) {
        return;
    }
    if (auto source = TMaybeOp<TSourceOperator>(node)) {
        source.Cast()->SetRowGroupPredicate(nullptr);
    }
    for (const auto& child : node->Children()) {
        if (auto op = TMaybeNode<IOperator>(child)) {
            Clear(op.Cast());
        }
    }
}

void Attach(const TOperatorPtr& node) {
    if (!node) {
        return;
    }
    if (TMaybeOp<TFilterOperator>(node)) {
        std::vector<NQumir::NAst::TExprPtr> predicates;
        TOperatorPtr input = node;
        while (auto nested = TMaybeOp<TFilterOperator>(input)) {
            predicates.push_back(nested.Cast()->Predicate());
            input = nested.Cast()->Input();
        }
        if (auto source = TMaybeOp<TSourceOperator>(input)) {
            auto schema = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
                source.Cast()->OutputColumns());
            if (schema) {
                auto pathPredicate = ConjoinPredicates(predicates);
                source.Cast()->SetRowGroupPredicate(BuildPredicateSuperset(
                    *schema.Cast(), {std::move(pathPredicate)}));
                return;
            }
        }
    }
    for (const auto& child : node->Children()) {
        if (auto op = TMaybeNode<IOperator>(child)) {
            Attach(op.Cast());
        }
    }
}

} // namespace

void AttachRowGroupPredicates(const TOperatorPtr& root) {
    Clear(root);
    Attach(root);
}

} // namespace NQdb

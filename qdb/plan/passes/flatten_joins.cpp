#include <qdb/plan/passes/flatten_joins.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

TExprPtr Eq(const std::string& l, const std::string& r) {
    return std::make_shared<TBinaryExpr>(NQumir::TLocation{}, TOperator("=="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, l),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, r));
}

TExprPtr Conjoin(const std::vector<TExprPtr>& parts) {
    TExprPtr result;
    for (const auto& p : parts) {
        result = result
            ? std::make_shared<TBinaryExpr>(p->Location, TOperator("&&"), result, p)
            : p;
    }
    return result;
}

bool IsInner(const TOperatorPtr& node) {
    auto join = TMaybeOp<TJoinOperator>(node);
    return join && join.Cast()->JoinType() == EJoinType::Inner;
}

void Collect(const TOperatorPtr& node, std::vector<TOperatorPtr>& leaves, std::vector<TExprPtr>& conds) {
    if (!IsInner(node)) {
        leaves.push_back(node);
        return;
    }
    auto join = TMaybeOp<TJoinOperator>(node).Cast();
    if (join->Filter()) {
        conds.push_back(join->Filter());
    }
    for (const auto& key : join->Keys()) {
        conds.push_back(Eq(key.Left, key.Right));
    }
    Collect(join->Left(), leaves, conds);
    Collect(join->Right(), leaves, conds);
}

} // namespace

TOperatorPtr FlattenInnerJoins(TOperatorPtr root) {
    if (!root) {
        return root;
    }
    if (IsInner(root)) {
        std::vector<TOperatorPtr> leaves;
        std::vector<TExprPtr> conds;
        Collect(root, leaves, conds);
        TOperatorPtr chain;
        for (const auto& leaf : leaves) {
            auto flat = FlattenInnerJoins(leaf);
            chain = chain
                ? std::make_shared<TJoinOperator>(
                      chain, flat, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr)
                : flat;
        }
        return conds.empty()
            ? chain
            : std::make_shared<TFilterOperator>(chain, Conjoin(conds));
    }
    if (auto n = TMaybeOp<TFilterOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TProjectOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TAggregateOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TSortOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TTopSortOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TLimitOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TWindowOperator>(root)) {
        n.Cast()->MutableInput() = FlattenInnerJoins(n.Cast()->Input());
    } else if (auto n = TMaybeOp<TJoinOperator>(root)) {
        n.Cast()->MutableLeft() = FlattenInnerJoins(n.Cast()->Left());
        n.Cast()->MutableRight() = FlattenInnerJoins(n.Cast()->Right());
    } else if (auto n = TMaybeOp<TUnionAllOperator>(root)) {
        for (auto& branch : n.Cast()->MutableInputs()) {
            branch = FlattenInnerJoins(branch);
        }
    }
    return root;
}

} // namespace NQdb

#include <qdb/plan/passes/equijoin.h>

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/utils/union_find.h>

#include "flatten_conjucts.h"

#include <iostream>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

struct TConjuct {
    TExprPtr Expr = nullptr;
    bool equiv = false;
    std::string left;
    std::string right;
};

struct TEquiJoinExtractor {
    TEquiJoinExtractor(TOperatorPtr root)
        : Root(root)
    { }

    void Run() {
        Process(Root, {});
    }

    void Process(TOperatorPtr node, std::vector<TConjuct> conjucts) {
        if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
            auto filter = maybeFilter.Cast();
            ExtractConjucts(conjucts, filter->Predicate());
            Process(filter->Input(), conjucts);
        } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
            auto join = maybeJoin.Cast();

            if (!(join->JoinType() == EJoinType::Inner && join->Keys().size() == 0)) {
                return;
            }

            // cross join
            TUnionFind<std::string> unionFind;
            for (const auto& conj : conjucts) {
                if (conj.equiv) {
                    unionFind.Union(conj.left, conj.right);
                }
            }

            auto leftCols = JoinColumns(0, join);
            auto rightCols = JoinColumns(1, join);

            std::unordered_set<std::string> used;
            std::vector<TJoinKey> joinKeys = ExtractJoinKeys(leftCols, rightCols, unionFind);
            for (auto& key : joinKeys) {
                used.insert(key.Left);
                used.insert(key.Right);
            }
            join->MutableKeys().swap(joinKeys);

            std::unordered_set<std::string> unused;
            std::vector<TConjuct> newConjucts;
            for (const auto& conj : conjucts) {
                if (conj.equiv) {
                    if (used.count(conj.left) == 0) {
                        unused.insert(conj.left);
                    }
                    if (used.count(conj.right) == 0) {
                        unused.insert(conj.right);
                    }
                } else {
                    newConjucts.emplace_back(conj);
                }
            }

            // rebuild predicate
            joinKeys = ExtractJoinKeys(unused, unused, unionFind);
            for (auto& key : joinKeys) {
                newConjucts.emplace_back(TConjuct{nullptr, true, key.Left, key.Right});
            }

            Process(join->Left(), newConjucts);
            Process(join->Right(), newConjucts);
        }
    }

    std::vector<TJoinKey> ExtractJoinKeys(const std::unordered_set<std::string>& leftCols, std::unordered_set<std::string> rightCols, TUnionFind<std::string>& unionFind)
    {
        std::vector<TJoinKey> joinKeys;
        for (const auto& left : leftCols) {
            std::optional<std::string> found;
            for (const auto& right : rightCols) {
                if (unionFind.Connected(left, right)) {
                    found = right;
                    break;
                }
            }
            if (found) {
                joinKeys.emplace_back(left, *found);
                rightCols.erase(*found);
            }
        }
        return joinKeys;
    }

    void ExtractConjucts(std::vector<TConjuct>& ret, const TExprPtr& expr) {
        std::vector<TExprPtr> conjucts;
        FlattenConjuncts(expr, conjucts);
        for (auto conj : conjucts) {
            auto& cur = ret.emplace_back(TConjuct{conj});
            auto maybeBinary = TMaybeNode<TBinaryExpr>(conj);
            if (!maybeBinary) {
                continue;
            }
            auto binary = maybeBinary.Cast();
            if (binary->Operator != "==") {
                continue;
            }
            auto identLeft = TMaybeNode<TIdentExpr>(binary->Left);
            auto identRight = TMaybeNode<TIdentExpr>(binary->Right);
            if (!identLeft) {
                continue;
            }
            if (!identRight) {
                continue;
            }
            cur.equiv = true;
            cur.left = identLeft.Cast()->Name;
            cur.right = identRight.Cast()->Name;
        }
    }

    std::unordered_set<std::string> JoinColumns(int side, const std::shared_ptr<TJoinOperator>& join) const {
        std::unordered_set<std::string> ret;
        auto sideInput = side == 0
            ? join->Left()
            : join->Right();
        auto* schema = static_cast<TStructType*>(sideInput->OutputColumns().get());
        if (!schema) {
            return ret;
        }

        for (const auto& [name, _] : schema->Fields) {
            ret.insert(name);
        }
        return ret;
    }

    TOperatorPtr Root;
};

} // namespace

TOperatorPtr ExtractEquiJoins(TOperatorPtr root) {
    // TODO: lift equi-predicates into join keys (see PLAN_EQUIJOIN.md).
    TEquiJoinExtractor{root}.Run();
    return root;
}

} // namespace NQdb

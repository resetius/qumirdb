#include "equijoin.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/passes/unbound_vars.h>
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

TExprPtr ConjuctExpr(const TConjuct& conj) {
    if (conj.Expr) {
        return conj.Expr;
    } else {
        return
            std::make_shared<TBinaryExpr>(NQumir::TLocation{}, TOperator("=="),
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, conj.left),
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, conj.right));
    }
}

TExprPtr Conjoin(const std::vector<TConjuct>& conjucts) {
    TExprPtr result;
    for (const auto& conj : conjucts) {
        auto expr = ConjuctExpr(conj);
        result = result
            ? std::make_shared<TBinaryExpr>(expr->Location, TOperator("&&"), result, expr)
            : expr;
    }
    return result;
}

TOperatorPtr Materialize(TOperatorPtr result, const std::vector<TConjuct>& conjucts) {
    return conjucts.empty()
        ? result
        : std::make_shared<TFilterOperator>(std::move(result), Conjoin(conjucts));
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

std::unordered_set<std::string> JoinColumns(int side, const std::shared_ptr<TJoinOperator>& join) {
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

struct TContext {
    std::vector<TConjuct> Conjucts;
};

TOperatorPtr Process(TOperatorPtr node, TContext ctx);

TOperatorPtr ProcessAggregate(std::shared_ptr<TAggregateOperator> aggregate, TContext ctx) {
    aggregate->MutableInput() = Process(aggregate->Input(), {});
    return Materialize(aggregate, ctx.Conjucts);
}

TOperatorPtr ProcessProject(std::shared_ptr<TProjectOperator> project, TContext ctx) {
    project->MutableInput() = Process(project->Input(), {});
    return Materialize(project, ctx.Conjucts);
}

TOperatorPtr ProcessCrossJoin(std::shared_ptr<TJoinOperator> join, TContext ctx)
{
    TUnionFind<std::string> unionFind;
    for (const auto& conj : ctx.Conjucts) {
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
    for (const auto& conj : ctx.Conjucts) {
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

    // push each conjunct only to the side whose schema covers all its
    // columns; conjuncts spanning both sides stay at this join.
    auto columnsOf = [](const TConjuct& conj) {
        if (conj.equiv) {
            return std::unordered_set<std::string>{conj.left, conj.right};
        }
        return FindUnboundVars(conj.Expr);
    };

    auto covers = [](
        const std::unordered_set<std::string>& cols,
        const std::unordered_set<std::string>& side)
    {
        for (const auto& col : cols) {
            if (side.count(col) == 0) {
                return false;
            }
        }
        return true;
    };

    std::vector<TConjuct> leftConjucts;
    std::vector<TConjuct> rightConjucts;
    std::vector<TConjuct> residualConjucts;
    for (const auto& conj : newConjucts) {
        auto cols = columnsOf(conj);
        if (covers(cols, leftCols)) {
            leftConjucts.emplace_back(conj);
        } else if (covers(cols, rightCols)) {
            rightConjucts.emplace_back(conj);
        } else {
            // spans both sides: cannot push down, keep as join residual
            residualConjucts.emplace_back(conj);
        }
    }

    if (!residualConjucts.empty()) {
        auto residual = Conjoin(residualConjucts);
        auto& filter = join->MutableFilter();
        filter = filter
            ? std::make_shared<TBinaryExpr>(residual->Location, TOperator("&&"), filter, residual)
            : residual;
    }

    join->MutableLeft() = Process(join->Left(), std::move(TContext{leftConjucts}));
    join->MutableRight() = Process(join->Right(), std::move(TContext{rightConjucts}));
    return join;
}

TOperatorPtr Process(TOperatorPtr node, TContext ctx) {
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
        auto filter = maybeFilter.Cast();
        ExtractConjucts(ctx.Conjucts, filter->Predicate());
        // drop the filter: its conjuncts move down into keys / lower nodes
        return Process(filter->Input(), std::move(ctx));
    } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
        auto join = maybeJoin.Cast();

        if (join->JoinType() == EJoinType::Inner && join->Keys().size() == 0) {
            return ProcessCrossJoin(join, std::move(ctx));
        }
    } else if (auto maybeProject = TMaybeOp<TProjectOperator>(node)) {
        auto project = maybeProject.Cast();
        return ProcessProject(project, std::move(ctx));
    } else if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(node)) {
        auto aggregate = maybeAggregate.Cast();
        return ProcessAggregate(aggregate, std::move(ctx));
    }

    return Materialize(node, ctx.Conjucts);
}

} // namespace

TOperatorPtr ExtractEquiJoins(TOperatorPtr root) {
    return Process(root, {});
}

} // namespace NQdb

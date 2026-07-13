#include "equijoin.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/utils/union_find.h>

#include "factor_conjuncts.h"
#include "flatten_conjuncts.h"
#include "flatten_disjuncts.h"

#include <unordered_map>
#include <unordered_set>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

// PushOnly pushes single-side predicates to leaves but leaves joins empty-key;
// ExtractKeys additionally lifts equalities into join keys.
enum class EMode { PushOnly, ExtractKeys };

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
    FactorConjuncts(expr, conjucts);
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

std::unordered_set<std::string> ColumnsOf(const TConjuct& conj) {
    if (conj.equiv) {
        return {conj.left, conj.right};
    }
    return FindUnboundVars(conj.Expr);
}

bool Covers(
    const std::unordered_set<std::string>& cols,
    const std::unordered_set<std::string>& side)
{
    for (const auto& col : cols) {
        if (side.count(col) == 0) {
            return false;
        }
    }
    return true;
}

// True when every column shares the same qualifier (alias) prefix, i.e. the
// conjunct constrains a single relation. Only these are pushed to leaves in
// PushOnly mode; multi-relation conjuncts (join edges) stay in the top filter.
bool SingleRelation(const TConjuct& conj) {
    std::string prefix;
    bool first = true;
    for (const auto& col : ColumnsOf(conj)) {
        auto dot = col.find('.');
        std::string rel = dot == std::string::npos ? col : col.substr(0, dot);
        if (first) {
            prefix = rel;
            first = false;
        } else if (rel != prefix) {
            return false;
        }
    }
    return !first;
}

TExprPtr Combine(const std::vector<TExprPtr>& parts, TOperator op) {
    TExprPtr result;
    for (const auto& part : parts) {
        result = result
            ? std::make_shared<TBinaryExpr>(part->Location, op, result, part)
            : part;
    }
    return result;
}

// Weakens a disjunction into a single-side necessary condition: per disjunct,
// keep only the atoms over `side`; if a disjunct constrains no `side` column,
// nothing can be derived. Returns null when there is nothing to push.
TExprPtr DeriveSideFilter(const TExprPtr& expr, const std::unordered_set<std::string>& side) {
    std::vector<TExprPtr> disjuncts;
    FlattenDisjuncts(expr, disjuncts);
    if (disjuncts.size() < 2) {
        return nullptr;
    }

    std::vector<TExprPtr> parts;
    for (const auto& disjunct : disjuncts) {
        std::vector<TExprPtr> atoms;
        FlattenConjuncts(disjunct, atoms);

        std::vector<TExprPtr> kept;
        for (const auto& atom : atoms) {
            if (Covers(FindUnboundVars(atom), side)) {
                kept.push_back(atom);
            }
        }
        if (kept.empty()) {
            return nullptr;
        }
        parts.push_back(Combine(kept, TOperator("&&")));
    }
    return Combine(parts, TOperator("||"));
}

struct TContext {
    std::vector<TConjuct> Conjucts;
    EMode Mode = EMode::ExtractKeys;
};

TOperatorPtr Process(TOperatorPtr node, TContext ctx);

TOperatorPtr ProcessAggregate(std::shared_ptr<TAggregateOperator> aggregate, TContext ctx) {
    aggregate->MutableInput() = Process(aggregate->Input(), {{}, ctx.Mode});
    return Materialize(aggregate, ctx.Conjucts);
}

TOperatorPtr ProcessLimit(std::shared_ptr<TLimitOperator> limit, TContext ctx) {
    limit->MutableInput() = Process(limit->Input(), {{}, ctx.Mode});
    return Materialize(limit, ctx.Conjucts);
}

TOperatorPtr ProcessProject(std::shared_ptr<TProjectOperator> project, TContext ctx) {
    project->MutableInput() = Process(project->Input(), {{}, ctx.Mode});
    return Materialize(project, ctx.Conjucts);
}

TOperatorPtr ProcessSort(std::shared_ptr<TSortOperator> sort, TContext ctx) {
    sort->MutableInput() = Process(sort->Input(), {{}, ctx.Mode});
    return Materialize(sort, ctx.Conjucts);
}

TOperatorPtr ProcessTopSort(std::shared_ptr<TTopSortOperator> topSort, TContext ctx) {
    topSort->MutableInput() = Process(topSort->Input(), {{}, ctx.Mode});
    return Materialize(topSort, ctx.Conjucts);
}

// Outer joins are excluded: pushing onto the null-extended side changes results.
bool IsRedistributable(EJoinType type) {
    return type == EJoinType::Inner
        || type == EJoinType::LeftSemi || type == EJoinType::LeftAnti
        || type == EJoinType::RightSemi || type == EJoinType::RightAnti;
}

TOperatorPtr ProcessJoin(std::shared_ptr<TJoinOperator> join, TContext ctx)
{
    if (ctx.Mode == EMode::PushOnly) {
        auto leftCols = JoinColumns(0, join);
        auto rightCols = JoinColumns(1, join);
        std::vector<TConjuct> leftConjucts;
        std::vector<TConjuct> rightConjucts;
        std::vector<TConjuct> keep;
        for (const auto& conj : ctx.Conjucts) {
            auto cols = ColumnsOf(conj);
            if (Covers(cols, leftCols)) {
                leftConjucts.emplace_back(conj);
            } else if (Covers(cols, rightCols)) {
                rightConjucts.emplace_back(conj);
            } else {
                keep.emplace_back(conj);
            }
        }
        join->MutableLeft() = Process(join->Left(), {std::move(leftConjucts), ctx.Mode});
        join->MutableRight() = Process(join->Right(), {std::move(rightConjucts), ctx.Mode});
        return Materialize(join, keep);
    }

    // Fold the join's own residual into the pool so explicit JOIN ON and
    // decorrelated semi/anti correlations feed key extraction too.
    std::vector<TConjuct>& pool = ctx.Conjucts;
    if (join->Filter()) {
        ExtractConjucts(pool, join->Filter());
        join->MutableFilter() = nullptr;
    }

    TUnionFind<std::string> unionFind;
    for (const auto& conj : pool) {
        if (conj.equiv) {
            unionFind.Union(conj.left, conj.right);
        }
    }

    auto leftCols = JoinColumns(0, join);
    auto rightCols = JoinColumns(1, join);

    auto addEquivConjuct = [](std::vector<TConjuct>& out, const std::string& left, const std::string& right) {
        if (left != right) {
            out.emplace_back(TConjuct{nullptr, true, left, right});
        }
    };

    std::vector<TConjuct> newConjucts;
    std::vector<std::string> equivCols;
    std::unordered_set<std::string> seenEquivCols;
    for (const auto& conj : pool) {
        if (!conj.equiv) {
            newConjucts.emplace_back(conj);
            continue;
        }
        if (seenEquivCols.insert(conj.left).second) {
            equivCols.push_back(conj.left);
        }
        if (seenEquivCols.insert(conj.right).second) {
            equivCols.push_back(conj.right);
        }
    }

    auto& keys = join->MutableKeys();
    std::unordered_set<std::string> assignedEquivCols;
    for (const auto& root : equivCols) {
        if (assignedEquivCols.count(root)) {
            continue;
        }

        std::vector<std::string> cls;
        for (const auto& col : equivCols) {
            if (unionFind.Connected(root, col)) {
                cls.push_back(col);
                assignedEquivCols.insert(col);
            }
        }

        std::vector<std::string> left;
        std::vector<std::string> right;
        for (const auto& col : cls) {
            if (leftCols.count(col)) {
                left.push_back(col);
            } else if (rightCols.count(col)) {
                right.push_back(col);
            }
        }

        if (!left.empty() && !right.empty()) {
            keys.emplace_back(left.front(), right.front());

            for (size_t i = 1; i < left.size(); ++i) {
                addEquivConjuct(newConjucts, left.front(), left[i]);
            }
            for (size_t i = 1; i < right.size(); ++i) {
                addEquivConjuct(newConjucts, right.front(), right[i]);
            }
        } else if (left.size() > 1) {
            for (size_t i = 1; i < left.size(); ++i) {
                addEquivConjuct(newConjucts, left.front(), left[i]);
            }
        } else if (right.size() > 1) {
            for (size_t i = 1; i < right.size(); ++i) {
                addEquivConjuct(newConjucts, right.front(), right[i]);
            }
        }
    }

    std::vector<TConjuct> leftConjucts;
    std::vector<TConjuct> rightConjucts;
    std::vector<TConjuct> residualConjucts;
    for (const auto& conj : newConjucts) {
        auto cols = ColumnsOf(conj);
        if (Covers(cols, leftCols)) {
            leftConjucts.emplace_back(conj);
        } else if (Covers(cols, rightCols)) {
            rightConjucts.emplace_back(conj);
        } else {
            residualConjucts.emplace_back(conj);
            if (conj.Expr) {
                if (auto derived = DeriveSideFilter(conj.Expr, leftCols)) {
                    leftConjucts.emplace_back(TConjuct{derived});
                }
                if (auto derived = DeriveSideFilter(conj.Expr, rightCols)) {
                    rightConjucts.emplace_back(TConjuct{derived});
                }
            }
        }
    }

    if (!residualConjucts.empty()) {
        join->MutableFilter() = Conjoin(residualConjucts);
    }

    join->MutableLeft() = Process(join->Left(), {std::move(leftConjucts), ctx.Mode});
    join->MutableRight() = Process(join->Right(), {std::move(rightConjucts), ctx.Mode});
    return join;
}

// Outer join. WHERE predicates from above may only be pushed onto the preserved
// side (Left keeps left, Right keeps right, Full neither). The ON residual is
// different: a non-key ON conjunct constraining only the null-extended side can
// be filtered on that side before the join — equivalent for an outer join, and
// what the executor needs (it has no join-residual support for outer joins).
TOperatorPtr ProcessOuterJoin(std::shared_ptr<TJoinOperator> join, TContext ctx) {
    EJoinType type = join->JoinType();
    auto leftCols = JoinColumns(0, join);
    auto rightCols = JoinColumns(1, join);

    std::vector<TConjuct> leftConjucts;
    std::vector<TConjuct> rightConjucts;

    // Pull equi keys out of the ON residual (keys + residual together stay equal
    // to the original ON); push the null-extended side's own conjuncts down to it.
    if (ctx.Mode == EMode::ExtractKeys && join->Filter()) {
        std::vector<TConjuct> on;
        ExtractConjucts(on, join->Filter());

        TUnionFind<std::string> unionFind;
        for (const auto& conj : on) {
            if (conj.equiv) {
                unionFind.Union(conj.left, conj.right);
            }
        }

        std::unordered_set<std::string> used;
        auto& keys = join->MutableKeys();
        for (auto& key : ExtractJoinKeys(leftCols, rightCols, unionFind)) {
            used.insert(key.Left);
            used.insert(key.Right);
            keys.push_back(std::move(key));
        }

        std::vector<TConjuct> residual;
        for (const auto& conj : on) {
            if (conj.equiv && used.count(conj.left) && used.count(conj.right)) {
                continue;
            }
            auto cols = ColumnsOf(conj);
            if (type == EJoinType::Left && Covers(cols, rightCols)) {
                rightConjucts.emplace_back(conj);
            } else if (type == EJoinType::Right && Covers(cols, leftCols)) {
                leftConjucts.emplace_back(conj);
            } else {
                residual.emplace_back(conj);
            }
        }
        join->MutableFilter() = residual.empty() ? nullptr : Conjoin(residual);
    }

    std::vector<TConjuct> keep;
    for (const auto& conj : ctx.Conjucts) {
        auto cols = ColumnsOf(conj);
        if (type == EJoinType::Left && Covers(cols, leftCols)) {
            leftConjucts.emplace_back(conj);
        } else if (type == EJoinType::Right && Covers(cols, rightCols)) {
            rightConjucts.emplace_back(conj);
        } else {
            keep.emplace_back(conj);
        }
    }

    join->MutableLeft() = Process(join->Left(), {std::move(leftConjucts), ctx.Mode});
    join->MutableRight() = Process(join->Right(), {std::move(rightConjucts), ctx.Mode});
    return Materialize(join, keep);
}

TOperatorPtr Process(TOperatorPtr node, TContext ctx) {
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
        auto filter = maybeFilter.Cast();
        if (ctx.Mode == EMode::PushOnly) {
            // Push only single-relation predicates down; keep the rest (join
            // edges) materialized here so the join chain stays intact.
            std::vector<TConjuct> all;
            ExtractConjucts(all, filter->Predicate());
            std::vector<TConjuct> kept;
            for (auto& conj : all) {
                if (SingleRelation(conj)) {
                    ctx.Conjucts.push_back(std::move(conj));
                } else {
                    kept.push_back(std::move(conj));
                }
            }
            auto inner = Process(filter->Input(), std::move(ctx));
            return Materialize(inner, kept);
        }
        ExtractConjucts(ctx.Conjucts, filter->Predicate());
        // drop the filter: its conjuncts move down into keys / lower nodes
        return Process(filter->Input(), std::move(ctx));
    } else if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
        auto join = maybeJoin.Cast();
        return IsRedistributable(join->JoinType())
            ? ProcessJoin(join, std::move(ctx))
            : ProcessOuterJoin(join, std::move(ctx));
    } else if (auto maybeProject = TMaybeOp<TProjectOperator>(node)) {
        auto project = maybeProject.Cast();
        return ProcessProject(project, std::move(ctx));
    } else if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(node)) {
        auto aggregate = maybeAggregate.Cast();
        return ProcessAggregate(aggregate, std::move(ctx));
    } else if (auto maybeLimit = TMaybeOp<TLimitOperator>(node)) {
        auto limit = maybeLimit.Cast();
        return ProcessLimit(limit, std::move(ctx));
    } else if (auto maybeSort = TMaybeOp<TSortOperator>(node)) {
        auto sort = maybeSort.Cast();
        return ProcessSort(sort, std::move(ctx));
    } else if (auto maybeTopSort = TMaybeOp<TTopSortOperator>(node)) {
        auto topSort = maybeTopSort.Cast();
        return ProcessTopSort(topSort, std::move(ctx));
    }

    return Materialize(node, ctx.Conjucts);
}

} // namespace

TOperatorPtr PushDownPredicates(TOperatorPtr root) {
    return Process(root, {{}, EMode::PushOnly});
}

TOperatorPtr ExtractEquiJoins(TOperatorPtr root) {
    return Process(root, {{}, EMode::ExtractKeys});
}

} // namespace NQdb

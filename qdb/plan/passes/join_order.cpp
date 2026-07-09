#include "join_order.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>

#include "factor_conjuncts.h"
#include "unbound_vars.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

struct TEdge {
    std::string Left;
    std::string Right;
};

void CollectEquiEdges(const TExprPtr& predicate, std::vector<TEdge>& out) {
    std::vector<TExprPtr> conjucts;
    FactorConjuncts(predicate, conjucts);
    for (const auto& conj : conjucts) {
        auto binary = TMaybeNode<TBinaryExpr>(conj);
        if (!binary || binary.Cast()->Operator != "==") {
            continue;
        }
        auto left = TMaybeNode<TIdentExpr>(binary.Cast()->Left);
        auto right = TMaybeNode<TIdentExpr>(binary.Cast()->Right);
        if (left && right) {
            out.push_back({left.Cast()->Name, right.Cast()->Name});
        }
    }
}

// Does this join input carry a filter (a selective dimension), rather than being
// a raw fact scan? Used by the reorder seed heuristic (put filtered relations
// first) and the semi-pushdown guard (keep selective joins in the build). Stops
// at subquery boundaries (aggregate/project) so an inner correlated filter does
// not count.
bool ContainsFilter(const TOperatorPtr& node) {
    if (!node) {
        return false;
    }
    if (TMaybeOp<TFilterOperator>(node)) {
        return true;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        return ContainsFilter(join.Cast()->Left())
            || ContainsFilter(join.Cast()->Right());
    }
    return false;
}

// A reorderable join is an inner join we are free to reassociate: no extracted
// keys yet and no residual of its own (the comma-join shape the builder emits).
bool IsReorderableJoin(const TOperatorPtr& node) {
    auto join = TMaybeOp<TJoinOperator>(node);
    return join
        && join.Cast()->JoinType() == EJoinType::Inner
        && join.Cast()->Keys().empty()
        && join.Cast()->Filter() == nullptr;
}

void CollectChainLeaves(const TOperatorPtr& node, std::vector<TOperatorPtr>& out) {
    if (IsReorderableJoin(node)) {
        auto join = TMaybeOp<TJoinOperator>(node).Cast();
        CollectChainLeaves(join->Left(), out);
        CollectChainLeaves(join->Right(), out);
    } else {
        out.push_back(node);
    }
}

// Greedy connected order: seed a component with the first unplaced leaf, then
// repeatedly attach any leaf sharing an edge with an already-placed one. When
// nothing connects, the next leaf seeds a new component (a deliberate cross).
std::vector<size_t> ConnectedOrder(
    size_t count,
    const std::vector<std::unordered_set<size_t>>& adjacency)
{
    std::vector<size_t> order;
    order.reserve(count);
    std::vector<bool> placed(count, false);

    while (order.size() < count) {
        size_t seed = 0;
        while (seed < count && placed[seed]) {
            ++seed;
        }
        order.push_back(seed);
        placed[seed] = true;

        bool grew = true;
        while (grew) {
            grew = false;
            for (size_t i = 0; i < count; ++i) {
                if (placed[i]) {
                    continue;
                }
                bool connected = false;
                for (size_t p : order) {
                    if (adjacency[i].count(p)) {
                        connected = true;
                        break;
                    }
                }
                if (connected) {
                    order.push_back(i);
                    placed[i] = true;
                    grew = true;
                }
            }
        }
    }
    return order;
}

TOperatorPtr BuildChain(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TEdge>& edges)
{
    // Map every output column to the leaf that produces it.
    std::unordered_map<std::string, size_t> owner;
    for (size_t i = 0; i < leaves.size(); ++i) {
        auto* schema = static_cast<TStructType*>(leaves[i]->OutputColumns().get());
        if (!schema) {
            continue;
        }
        for (const auto& [name, _] : schema->Fields) {
            owner.emplace(name, i);
        }
    }

    std::vector<std::unordered_set<size_t>> adjacency(leaves.size());
    for (const auto& edge : edges) {
        auto left = owner.find(edge.Left);
        auto right = owner.find(edge.Right);
        if (left == owner.end() || right == owner.end() || left->second == right->second) {
            continue;
        }
        adjacency[left->second].insert(right->second);
        adjacency[right->second].insert(left->second);
    }

    auto order = ConnectedOrder(leaves.size(), adjacency);

    TOperatorPtr result = leaves[order.front()];
    for (size_t i = 1; i < order.size(); ++i) {
        result = std::make_shared<TJoinOperator>(
            std::move(result), leaves[order[i]],
            std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
    }
    return result;
}

// `edges` are equality edges threaded down from enclosing filters, so an
// inner-join chain can be reordered even when a filter is separated from it by
// other operators (e.g. a decorrelation LEFT JOIN). Edges are reset across
// project/aggregate, which redefine columns; BuildChain ignores edges whose
// endpoints aren't owned by the chain's leaves (qualified names disambiguate).
TOperatorPtr Reorder(TOperatorPtr node, std::vector<TEdge> edges) {
    if (!node) {
        return node;
    }
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
        auto filter = maybeFilter.Cast();
        CollectEquiEdges(filter->Predicate(), edges);
        filter->MutableInput() = Reorder(filter->Input(), std::move(edges));
        return filter;
    }
    if (IsReorderableJoin(node)) {
        std::vector<TOperatorPtr> leaves;
        CollectChainLeaves(node, leaves);
        for (auto& leaf : leaves) {
            leaf = Reorder(std::move(leaf), {});
        }
        return BuildChain(leaves, edges);
    }
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
        auto join = maybeJoin.Cast();
        join->MutableLeft() = Reorder(join->Left(), edges);
        join->MutableRight() = Reorder(join->Right(), edges);
        return join;
    }
    if (auto maybeProject = TMaybeOp<TProjectOperator>(node)) {
        auto project = maybeProject.Cast();
        project->MutableInput() = Reorder(project->Input(), {});
        return project;
    }
    if (auto maybeAggregate = TMaybeOp<TAggregateOperator>(node)) {
        auto aggregate = maybeAggregate.Cast();
        aggregate->MutableInput() = Reorder(aggregate->Input(), {});
        return aggregate;
    }
    if (auto maybeLimit = TMaybeOp<TLimitOperator>(node)) {
        auto limit = maybeLimit.Cast();
        limit->MutableInput() = Reorder(limit->Input(), {});
        return limit;
    }
    if (auto maybeSort = TMaybeOp<TSortOperator>(node)) {
        auto sort = maybeSort.Cast();
        sort->MutableInput() = Reorder(sort->Input(), {});
        return sort;
    }
    if (auto maybeTopSort = TMaybeOp<TTopSortOperator>(node)) {
        auto topSort = maybeTopSort.Cast();
        topSort->MutableInput() = Reorder(topSort->Input(), {});
        return topSort;
    }
    return node;
}

} // namespace

namespace {

std::unordered_set<std::string> ColumnNames(const TOperatorPtr& node) {
    std::unordered_set<std::string> names;
    if (auto st = TMaybeType<TStructType>(node->OutputColumns())) {
        for (const auto& [name, _] : st.Cast()->Fields) {
            names.insert(name);
        }
    }
    return names;
}

bool AllIn(const std::unordered_set<std::string>& cols,
    const std::unordered_set<std::string>& set)
{
    for (const auto& c : cols) {
        if (!set.count(c)) {
            return false;
        }
    }
    return true;
}

// Sinks a LeftSemi/LeftAnti join through the inner joins beneath its build side,
// as deep as its build-side dependencies allow. Reads output schemas of the
// current (typed) tree before mutating, so a caller must re-annotate afterwards.
TOperatorPtr SinkSemi(std::shared_ptr<TJoinOperator> semi) {
    auto childJoin = TMaybeOp<TJoinOperator>(semi->Left());
    if (!childJoin || childJoin.Cast()->JoinType() != EJoinType::Inner) {
        return semi;
    }
    auto inner = childJoin.Cast();

    // Everything the semi needs from its build side: its left key columns plus
    // any build-side columns its residual predicate reads. (Probe-side columns
    // travel with the probe input C and are unaffected by the push.)
    std::unordered_set<std::string> needBuild;
    for (const auto& key : semi->Keys()) {
        needBuild.insert(key.Left);
    }
    if (semi->Filter()) {
        const auto childCols = ColumnNames(semi->Left());
        for (const auto& v : FindUnboundVars(semi->Filter())) {
            if (childCols.count(v)) {
                needBuild.insert(v);
            }
        }
    }

    const auto leftCols = ColumnNames(inner->Left());
    const auto rightCols = ColumnNames(inner->Right());
    const bool toLeft = AllIn(needBuild, leftCols);
    const bool toRight = !toLeft && AllIn(needBuild, rightCols);
    if (!toLeft && !toRight) {
        return semi; // build dependencies span both sides — cannot push
    }

    // Don't push past a selective (filtered) side: keeping that join in the
    // build shrinks the blocking semi/anti far more than the push would.
    const auto& other = toLeft ? inner->Right() : inner->Left();
    if (ContainsFilter(other)) {
        return semi;
    }

    auto& target = toLeft ? inner->MutableLeft() : inner->MutableRight();
    auto pushed = std::make_shared<TJoinOperator>(
        target, semi->Right(), semi->Keys(), semi->JoinType(), semi->Filter());
    target = SinkSemi(std::move(pushed));
    return inner;
}

TOperatorPtr PushDown(TOperatorPtr node) {
    if (!node) {
        return node;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        auto j = join.Cast();
        const auto type = j->JoinType();
        // Sink first (top-down), while this subtree's types are still current;
        // SinkSemi preserves output column names, so recursing into the
        // restructured result stays valid before the pipeline re-annotates.
        if (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) {
            auto sunk = SinkSemi(j);
            if (sunk.get() != j.get()) {
                return PushDown(std::move(sunk));
            }
        }
        j->MutableLeft() = PushDown(j->Left());
        j->MutableRight() = PushDown(j->Right());
        return j;
    }
    if (auto f = TMaybeOp<TFilterOperator>(node)) {
        f.Cast()->MutableInput() = PushDown(f.Cast()->Input());
    } else if (auto p = TMaybeOp<TProjectOperator>(node)) {
        p.Cast()->MutableInput() = PushDown(p.Cast()->Input());
    } else if (auto a = TMaybeOp<TAggregateOperator>(node)) {
        a.Cast()->MutableInput() = PushDown(a.Cast()->Input());
    } else if (auto l = TMaybeOp<TLimitOperator>(node)) {
        l.Cast()->MutableInput() = PushDown(l.Cast()->Input());
    } else if (auto s = TMaybeOp<TSortOperator>(node)) {
        s.Cast()->MutableInput() = PushDown(s.Cast()->Input());
    } else if (auto t = TMaybeOp<TTopSortOperator>(node)) {
        t.Cast()->MutableInput() = PushDown(t.Cast()->Input());
    }
    return node;
}

} // namespace

TOperatorPtr ReorderJoins(TOperatorPtr root) {
    return Reorder(std::move(root), {});
}

TOperatorPtr PushDownSemiJoins(TOperatorPtr root) {
    return PushDown(std::move(root));
}

} // namespace NQdb

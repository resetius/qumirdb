#include "join_order.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>

#include "factor_conjuncts.h"

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

TOperatorPtr ReorderJoins(TOperatorPtr root) {
    return Reorder(std::move(root), {});
}

} // namespace NQdb

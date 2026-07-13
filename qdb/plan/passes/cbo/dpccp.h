#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstddef>
#include <string>
#include <vector>

// DPccp join ordering: https://www.vldb.org/conf/2006/p930-moerkotte.pdf

namespace NQdb {
namespace NCbo {

// Join-graph edge: equality between columns of two leaves.
struct TJoinEdge {
    std::string LeftCol;
    std::string RightCol;
};

constexpr size_t MaxRelations = 12;

// Optimal bushy inner-join tree minimizing the planner cost stored in Stats_;
// joins are key-less (ExtractEquiJoins keys them later). nullptr when DP does
// not apply (n > MaxRelations or disconnected graph). Leaves must carry Stats_.
TOperatorPtr DpccpJoinOrder(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TJoinEdge>& edges);

} // namespace NCbo
} // namespace NQdb

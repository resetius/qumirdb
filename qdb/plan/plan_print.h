#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/sort.h>

#include <qumir/parser/ast.h>

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NQdb {

std::string ExprLine(const NQumir::NAst::TExprPtr& expr);
std::string JoinPlanLabel(const TJoinOperator& join);
std::string AggregatePlanLabel(const TAggregateOperator& aggregate);
std::string SortPlanLabel(
    std::string_view kind,
    const std::vector<TSortKey>& keys,
    std::optional<int64_t> limit = std::nullopt);
std::string PlanLabel(const TOperatorPtr& op);
std::vector<TOperatorPtr> ChildOps(const TOperatorPtr& op);
void PrintPlanTree(
    std::ostream& out,
    const TOperatorPtr& op,
    const std::string& prefix = "",
    bool isLast = true,
    bool isRoot = true);

} // namespace NQdb

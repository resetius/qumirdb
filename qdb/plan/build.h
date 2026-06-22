#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/sql/ast.h>

#include <qumir/error.h>

#include <expected>
#include <functional>
#include <string_view>

namespace NQqb {

using TTableSourceFactory =
    std::function<std::expected<TOperatorPtr, NQumir::TError>(std::string_view table)>;

// Builds an unqualified, untyped logical plan from a parsed SQL query.
std::expected<TOperatorPtr, NQumir::TError>
BuildPlan(const NQdb::NSql::TSqlNodePtr& query, const TTableSourceFactory& sources);

} // namespace NQqb

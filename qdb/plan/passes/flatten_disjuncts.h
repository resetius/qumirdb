#pragma once

#include <qumir/parser/ast.h>

#include <vector>

namespace NQdb {

void FlattenDisjuncts(const NQumir::NAst::TExprPtr& expr, std::vector<NQumir::NAst::TExprPtr>& out);

} // namespace NQdb

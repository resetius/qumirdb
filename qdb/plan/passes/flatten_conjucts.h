#pragma once

#include <qumir/parser/ast.h>

#include <string>
#include <unordered_set>

namespace NQdb {

void FlattenConjuncts(const NQumir::NAst::TExprPtr& expr, std::vector<NQumir::NAst::TExprPtr>& out);

} // namespace NQdb

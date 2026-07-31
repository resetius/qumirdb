#pragma once

#include <qumir/parser/ast.h>

namespace NQdb {

NQumir::NAst::TExprPtr ConstFold(NQumir::NAst::TExprPtr expr);

} // namespace NQdb

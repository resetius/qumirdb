#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>
#include <vector>

namespace NQdb::NKernel {

struct TColumnValueAst {
    std::vector<NQumir::NAst::TExprPtr> Setup;
    NQumir::NAst::TExprPtr Value;
    NQumir::NAst::TExprPtr IsValid;
    NQumir::NAst::TTypePtr ValueType;
};

TColumnValueAst BuildColumnValueAst(
    const std::string& columnName,
    const std::string& rowName,
    const std::string& temporaryPrefix,
    const NQumir::NAst::TTypePtr& logicalType,
    const NQumir::NAst::TTypePtr& stringViewType,
    bool needValue = true);

} // namespace NQdb::NKernel

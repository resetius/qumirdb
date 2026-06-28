#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>
#include <vector>

namespace NQdb {
namespace NKernel {
namespace NOz {

class TFunBuilder {
public:
    explicit TFunBuilder(std::string name);

    TFunBuilder& Param(std::string name, NQumir::NAst::TTypePtr type);
    TFunBuilder& Return(NQumir::NAst::TTypePtr type);
    TFunBuilder& Stmt(NQumir::NAst::TExprPtr stmt);

    NQumir::NAst::TExprPtr Build() &&;

private:
    std::string Name_;
    std::vector<NQumir::NAst::TParam> Params_;
    NQumir::NAst::TTypePtr RetType_;
    std::vector<NQumir::NAst::TExprPtr> Stmts_;
};

NQumir::NAst::TExprPtr Ident(std::string name);
NQumir::NAst::TExprPtr Return(NQumir::NAst::TExprPtr value = nullptr);
NQumir::NAst::TExprPtr Block(std::vector<NQumir::NAst::TExprPtr> stmts);

} // namespace NOz
} // namespace NKernel
} // namespace NQdb


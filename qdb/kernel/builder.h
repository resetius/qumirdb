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

    TFunBuilder& Var(std::string name, NQumir::NAst::TTypePtr type);
    TFunBuilder& Assign(std::string name, NQumir::NAst::TExprPtr value);

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

NQumir::NAst::TExprPtr Var(std::string name, NQumir::NAst::TTypePtr type);
NQumir::NAst::TExprPtr Assign(std::string name, NQumir::NAst::TExprPtr value);

NQumir::NAst::TExprPtr Int(int64_t v);
NQumir::NAst::TExprPtr Float(double v);
NQumir::NAst::TExprPtr Bool(bool v);
NQumir::NAst::TExprPtr String(std::string v);

NQumir::NAst::TExprPtr Bin(NQumir::NAst::TOperator op, NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Add(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Sub(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Mul(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);
NQumir::NAst::TExprPtr Div(NQumir::NAst::TExprPtr l, NQumir::NAst::TExprPtr r);

NQumir::NAst::TExprPtr Call(std::string name, std::vector<NQumir::NAst::TExprPtr> args = {});
NQumir::NAst::TExprPtr If(NQumir::NAst::TExprPtr cond, NQumir::NAst::TExprPtr thenBody, NQumir::NAst::TExprPtr elseBody = nullptr);

NQumir::NAst::TExprPtr While(NQumir::NAst::TExprPtr cond, NQumir::NAst::TExprPtr body);

NQumir::NAst::TExprPtr Index(NQumir::NAst::TExprPtr collection, NQumir::NAst::TExprPtr index);
NQumir::NAst::TExprPtr Index(std::string collection, NQumir::NAst::TExprPtr index);

} // namespace NOz
} // namespace NKernel
} // namespace NQdb


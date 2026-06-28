#include <qdb/kernel/builder.h>

#include <utility>

namespace NQdb {
namespace NKernel {
namespace NOz {

using namespace NQumir::NAst;

namespace {

NQumir::TLocation Loc() {
    return {};
}

} // namespace

TFunBuilder::TFunBuilder(std::string name)
    : Name_(std::move(name))
{}

TFunBuilder& TFunBuilder::Param(std::string name, TTypePtr type) {
    Params_.push_back(std::make_shared<TVarStmt>(
        Loc(), std::move(name), std::move(type)));
    return *this;
}

TFunBuilder& TFunBuilder::Return(TTypePtr type) {
    RetType_ = std::move(type);
    return *this;
}

TFunBuilder& TFunBuilder::Stmt(TExprPtr stmt) {
    Stmts_.push_back(std::move(stmt));
    return *this;
}

TExprPtr TFunBuilder::Build() && {
    auto body = std::make_shared<TBlockExpr>(Loc(), std::move(Stmts_));
    return std::make_shared<TFunDecl>(
        Loc(), std::move(Name_), std::move(Params_), std::move(body),
        std::move(RetType_));
}

TExprPtr Ident(std::string name) {
    return std::make_shared<TIdentExpr>(Loc(), std::move(name));
}

TExprPtr Return(TExprPtr value) {
    return std::make_shared<TReturnExpr>(Loc(), std::move(value));
}

TExprPtr Block(std::vector<TExprPtr> stmts) {
    return std::make_shared<TBlockExpr>(Loc(), std::move(stmts));
}

} // namespace NOz
} // namespace NKernel
} // namespace NQdb


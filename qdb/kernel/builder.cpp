#include <qdb/kernel/builder.h>

#include <utility>

namespace NQdb {
namespace NKernel {
namespace NOz {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

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

TFunBuilder& TFunBuilder::Var(std::string name, TTypePtr type) {
    return Stmt(NOz::Var(std::move(name), std::move(type)));
}

TFunBuilder& TFunBuilder::Assign(std::string name, TExprPtr value) {
    return Stmt(NOz::Assign(std::move(name), std::move(value)));
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

TExprPtr Var(std::string name, TTypePtr type) {
    return std::make_shared<TVarStmt>(Loc(), std::move(name), std::move(type));
}

TExprPtr Assign(std::string name, TExprPtr value) {
    return std::make_shared<TAssignExpr>(Loc(), std::move(name), std::move(value));
}

TExprPtr Int(int64_t v) {
    return std::make_shared<TNumberExpr>(Loc(), v);
}

TExprPtr TypedInt(int64_t v, TTypePtr type) {
    auto e = std::make_shared<TNumberExpr>(Loc(), v);
    e->Type = std::move(type);
    return e;
}

TExprPtr Float(double v) {
    return std::make_shared<TNumberExpr>(Loc(), v);
}

TExprPtr Bool(bool v) {
    auto e = std::make_shared<TNumberExpr>(Loc(), static_cast<int64_t>(v));
    e->Type = std::make_shared<TBoolType>();
    return e;
}

TExprPtr String(std::string v) {
    return std::make_shared<TStringLiteralExpr>(Loc(), std::move(v));
}

TExprPtr Bin(TOperator op, TExprPtr l, TExprPtr r) {
    return std::make_shared<TBinaryExpr>(
        Loc(), std::move(op), std::move(l), std::move(r));
}

TExprPtr Add(TExprPtr l, TExprPtr r) { return Bin("+"_op, std::move(l), std::move(r)); }
TExprPtr Sub(TExprPtr l, TExprPtr r) { return Bin("-"_op, std::move(l), std::move(r)); }
TExprPtr Mul(TExprPtr l, TExprPtr r) { return Bin("*"_op, std::move(l), std::move(r)); }
TExprPtr Div(TExprPtr l, TExprPtr r) { return Bin("/"_op, std::move(l), std::move(r)); }

TExprPtr Call(std::string name, std::vector<TExprPtr> args) {
    return std::make_shared<TCallExpr>(
        Loc(),
        Ident(std::move(name)),
        std::move(args));
}

TExprPtr Cast(TExprPtr expr, TTypePtr type) {
    return std::make_shared<TCastExpr>(Loc(), std::move(expr), std::move(type));
}

TExprPtr NullPtr(TTypePtr type) {
    return Cast(Int(0), std::move(type));
}

TExprPtr Unary(TOperator op, TExprPtr value) {
    return std::make_shared<TUnaryExpr>(
        Loc(), std::move(op), std::move(value));
}

TExprPtr If(TExprPtr cond, TExprPtr thenBody, TExprPtr elseBody) {
    return std::make_shared<TIfExpr>(
        Loc(),
        std::move(cond),
        std::move(thenBody),
        std::move(elseBody));
}

TExprPtr While(TExprPtr cond, TExprPtr body) {
    auto loop = std::make_shared<TWhileStmtExpr>(Loc(), std::move(cond), std::move(body));
    return loop;
}

TExprPtr Index(TExprPtr collection, TExprPtr index) {
    return std::make_shared<TIndexExpr>(
        Loc(),
        std::move(collection),
        std::move(index));
}

TExprPtr Index(std::string collection, TExprPtr index) {
    return Index(Ident(std::move(collection)), std::move(index));
}

TExprPtr ArrayAssign(std::string collection, TExprPtr index, TExprPtr value) {
    std::vector<TExprPtr> indices;
    indices.push_back(std::move(index));
    return ArrayAssign(std::move(collection), std::move(indices), std::move(value));
}

TExprPtr ArrayAssign(std::string collection, std::vector<TExprPtr> indices, TExprPtr value) {
    return std::make_shared<TArrayAssignExpr>(
        Loc(), std::move(collection), std::move(indices), std::move(value));
}

TExprPtr Field(TExprPtr object, std::string field) {
    return std::make_shared<TFieldAccessExpr>(
        Loc(), std::move(object), std::move(field));
}

TExprPtr Field(std::string object, std::string field) {
    return Field(Ident(std::move(object)), std::move(field));
}

TExprPtr FieldAssign(TExprPtr object, std::string field, TExprPtr value) {
    return std::make_shared<TFieldAssignExpr>(
        Loc(), std::move(object), std::move(field), std::move(value));
}

} // namespace NOz
} // namespace NKernel
} // namespace NQdb

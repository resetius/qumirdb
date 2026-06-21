#pragma once

#include <qumir/parser/ast.h>

namespace NQdb {
namespace NSql {

struct TSqlNode {
    virtual ~TSqlNode() = default;
};

using TSqlNodePtr = std::shared_ptr<TSqlNode>;

template <class T>
using TSqlPtr = std::shared_ptr<T>;

enum ESetQuantifier {
    All,
    Distinct,
};

struct TIdentList;
struct TSqlQuery;

struct TSqlCte : TSqlNode {
    std::string Name;
    TSqlPtr<TIdentList> Columns; // optional column name list
    TSqlPtr<TSqlQuery> Query;
};

struct TSqlWithClause : TSqlNode {
    bool Recursive = false;
    std::vector<TSqlPtr<TSqlCte>> Ctes;
};

struct TSqlOrderItem : TSqlNode {
    NQumir::NAst::TExprPtr Expr;
    bool Desc = false;

    enum class ENullOrder {
        Default,
        First,
        Last
    };
    ENullOrder NullOrder = ENullOrder::Default;
};

struct TSqlOrder : TSqlNode {
    std::vector<TSqlPtr<TSqlOrderItem>> Items;
};

struct TSqlSelectItem : TSqlNode {
    NQumir::NAst::TExprPtr Expr;
    std::optional<std::string> Alias;

    // For "*" and "<qualified_name>.*". When Star is set, Expr/Alias are unused
    // and StarPrefix holds the optional qualifier (empty for a bare "*").
    bool Star = false;
    std::vector<std::string> StarPrefix;
};

struct TSqlTableRef : TSqlNode {
    std::optional<std::string> Alias;
};

struct TSqlTableName : TSqlTableRef {
    std::vector<std::string> Name; // schema.table
};

struct TSqlQuery;

struct TSqlSubqueryTable : TSqlTableRef {
    TSqlPtr<TSqlQuery> Query;
};

enum class ESqlJoinType {
    Inner,
    Left,
    Right,
    LeftSemi,
    RightSemi,
    Full,
    Cross,
};

struct TIdentList : TSqlNode {
    std::vector<std::string> Items;
};

struct TJoinCondition : TSqlNode {
    NQumir::NAst::TExprPtr On;
    TSqlPtr<TIdentList> UsingColumns;
};

struct TSqlJoin : TSqlTableRef {
    TSqlPtr<TSqlTableRef> Left;
    TSqlPtr<TSqlTableRef> Right;

    ESqlJoinType Type = ESqlJoinType::Inner;

    TSqlPtr<TJoinCondition> Condition;
};

struct TSqlQuery : TSqlNode {
    TSqlPtr<TSqlWithClause> WithClause;

    TSqlNodePtr Body; // TSqlSelect / TSqlSetOp later

    TSqlPtr<TSqlOrder> OrderBy;

    NQumir::NAst::TExprPtr Limit;
    NQumir::NAst::TExprPtr Offset;
};

struct TSqlSelectList : TSqlNode {
    std::vector<TSqlPtr<TSqlSelectItem>> Items;
};

struct TSqlGroupBy : TSqlNode {
    std::vector<NQumir::NAst::TExprPtr> Items;
};

struct TSqlFrom : TSqlNode {
    std::vector<TSqlPtr<TSqlTableRef>> Items;
};

struct TSqlSelect : TSqlNode {
    ESetQuantifier Quantifier = ESetQuantifier::All;

    TSqlPtr<TSqlSelectList> SelectList;

    TSqlPtr<TSqlFrom> From;

    NQumir::NAst::TExprPtr Where;

    TSqlPtr<TSqlGroupBy> GroupBy;

    NQumir::NAst::TExprPtr Having;
};

// A subquery used inside an expression. Bridges a SQL query into the Qumir
// expression tree; visitors handle it through IVisitor::VisitOtherwise.
struct TSubqueryExpr : NQumir::NAst::TExpr {
    static constexpr const char* NodeId = "SqlSubquery";

    enum class EKind {
        Scalar, // ( <select> )
        Exists, // EXISTS ( <select> )
        In,     // <operand> IN ( <select> )
    };

    EKind Kind = EKind::Scalar;
    TSqlPtr<TSqlQuery> Query;
    NQumir::NAst::TExprPtr Operand; // left-hand side, used by the IN form

    TSubqueryExpr(NQumir::TLocation loc, EKind kind, TSqlPtr<TSqlQuery> query)
        : NQumir::NAst::TExpr(std::move(loc))
        , Kind(kind)
        , Query(std::move(query))
    { }

    std::vector<NQumir::NAst::TExprPtr> Children() const override {
        if (Operand) {
            return { Operand };
        }
        return {};
    }

    std::vector<NQumir::NAst::TExprPtr*> MutableChildren() override {
        if (Operand) {
            return { &Operand };
        }
        return {};
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(NQumir::NAst::IVisitor& visitor) override {
        visitor.VisitOtherwise(*this);
    }
};

} // namespace NSql
} // namespace NQdb

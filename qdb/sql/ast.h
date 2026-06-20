#include <qumir/parser/ast.h>

namespace NQqb {
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

struct TSqlWithClause : TSqlNode {
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

struct TSqlSelectItem : TSqlNode {
    NQumir::NAst::TExprPtr Expr;
    std::optional<std::string> Alias;
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

struct TSqlJoin : TSqlTableRef {
    TSqlPtr<TSqlTableRef> Left;
    TSqlPtr<TSqlTableRef> Right;

    ESqlJoinType Type = ESqlJoinType::Inner;

    NQumir::NAst::TExprPtr On;

    std::vector<std::string> UsingColumns;
};

struct TSqlQuery : TSqlNode {
    TSqlPtr<TSqlWithClause> WithClause;

    TSqlNodePtr Body; // TSqlSelect / TSqlSetOp later

    std::vector<TSqlPtr<TSqlOrderItem>> OrderBy;

    NQumir::NAst::TExprPtr Limit;
    NQumir::NAst::TExprPtr Offset;
};

struct TSqlSelect : TSqlNode {
    ESetQuantifier Quantifier = ESetQuantifier::All;

    std::vector<TSqlPtr<TSqlSelectItem>> SelectList;

    TSqlPtr<TSqlTableRef> From;

    NQumir::NAst::TExprPtr Where;

    std::vector<NQumir::NAst::TExprPtr> GroupBy;

    NQumir::NAst::TExprPtr Having;
};

} // namespace NSql
} // namespace NQqb

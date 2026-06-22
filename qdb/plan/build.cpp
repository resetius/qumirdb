#include <qdb/plan/build.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cctype>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace NQqb {

namespace {

namespace NAst = NQumir::NAst;
namespace NSql = NQdb::NSql;
using NQumir::TError;
using NQumir::TLocation;

std::string ToLower(std::string s) {
    for (char& c : s) {
        c = std::tolower(static_cast<unsigned char>(c));
    }
    return s;
}

bool IsAggFunc(const std::string& name) {
    static const std::set<std::string> funcs = {"sum", "count", "avg", "min", "max"};
    return funcs.count(name) > 0;
}

struct TAggCall {
    std::string Func;
    NAst::TExprPtr Arg; // null for count(*)
    bool Distinct = false;
    bool Star = false;
};

std::optional<TAggCall> AsAggCall(const NAst::TExprPtr& expr) {
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr);
    if (!call) {
        return std::nullopt;
    }
    auto callee = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee);
    if (!callee) {
        return std::nullopt;
    }
    std::string func = ToLower(callee.Cast()->Name);
    if (!IsAggFunc(func)) {
        return std::nullopt;
    }

    TAggCall agg;
    agg.Func = func;
    const auto& args = call.Cast()->Args;
    if (!args.empty()) {
        const auto& arg = args[0];
        auto star = NAst::TMaybeNode<NAst::TIdentExpr>(arg);
        auto distinct = NAst::TMaybeNode<NAst::TCallExpr>(arg);
        if (star && star.Cast()->Name == "*") {
            agg.Star = true;
        } else if (distinct) {
            auto distinctCallee = NAst::TMaybeNode<NAst::TIdentExpr>(distinct.Cast()->Callee);
            if (distinctCallee && ToLower(distinctCallee.Cast()->Name) == "distinct") {
                agg.Distinct = true;
                const auto& distinctArgs = distinct.Cast()->Args;
                agg.Arg = distinctArgs.empty() ? nullptr : distinctArgs[0];
            } else {
                agg.Arg = arg;
            }
        } else {
            agg.Arg = arg;
        }
    }
    return agg;
}

NAst::TExprPtr Ident(TLocation loc, std::string name) {
    return std::make_shared<NAst::TIdentExpr>(std::move(loc), std::move(name));
}

bool HasSubquery(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        return true;
    }
    for (const auto& child : expr->Children()) {
        if (HasSubquery(child)) {
            return true;
        }
    }
    return false;
}

// Replaces aggregate calls in an expression with references to synthetic
// aggregate output columns, collecting the corresponding specs.
class TAggCollector {
public:
    std::expected<NAst::TExprPtr, TError> Rewrite(NAst::TExprPtr expr) {
        if (!expr) {
            return expr;
        }
        if (auto agg = AsAggCall(expr)) {
            return Emit(*agg, expr->Location);
        }
        for (auto* child : expr->MutableChildren()) {
            auto rewritten = Rewrite(*child);
            if (!rewritten) {
                return std::unexpected(rewritten.error());
            }
            *child = std::move(*rewritten);
        }
        return expr;
    }

    std::vector<TAggregateSpec> TakeSpecs() {
        return std::move(Specs_);
    }

    bool Empty() const {
        return Specs_.empty();
    }

private:
    std::expected<NAst::TExprPtr, TError> Emit(const TAggCall& agg, TLocation loc) {
        if (agg.Distinct) {
            return std::unexpected(TError(loc, "count(distinct) is not supported yet"));
        }

        // avg has no aggregate kernel; express it as sum / count.
        if (agg.Func == "avg") {
            std::string sumName = NextName("sum");
            Specs_.push_back({ .Name = sumName, .Func = "sum", .Arg = agg.Arg });
            std::string countName = NextName("count");
            Specs_.push_back({ .Name = countName, .Func = "count", .Arg = nullptr });

            auto count = std::make_shared<NAst::TCastExpr>(
                loc, Ident(loc, countName), std::make_shared<NAst::TFloatType>());
            return std::make_shared<NAst::TBinaryExpr>(
                loc, NAst::TOperator('/'), Ident(loc, sumName), std::move(count));
        }

        NAst::TExprPtr arg = (agg.Func == "count" && agg.Star)
            ? nullptr
            : agg.Arg;
        std::string name = NextName(agg.Func);
        Specs_.push_back({ .Name = name, .Func = agg.Func, .Arg = std::move(arg) });
        return Ident(loc, name);
    }

    std::string NextName(const std::string& func) {
        return func + "_" + std::to_string(Counter_++);
    }

    std::vector<TAggregateSpec> Specs_;
    int Counter_ = 0;
};

std::string ItemName(const NSql::TSqlSelectItem& item, size_t index) {
    if (item.Alias) {
        return *item.Alias;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item.Expr)) {
        return ident.Cast()->Name;
    }
    return "col" + std::to_string(index);
}

std::string TableName(const std::vector<std::string>& parts) {
    std::string name;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            name += '.';
        }
        name += parts[i];
    }
    return name;
}

std::expected<std::vector<std::string>, TError> GroupKeys(const NSql::TSqlGroupBy& groupBy) {
    std::vector<std::string> keys;
    for (const auto& item : groupBy.Items) {
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item);
        if (!ident) {
            return std::unexpected(TError("GROUP BY on an expression is not supported yet"));
        }
        keys.push_back(ident.Cast()->Name);
    }
    return keys;
}

EJoinType MapJoinType(NSql::ESqlJoinType type) {
    switch (type) {
        case NSql::ESqlJoinType::Inner: return EJoinType::Inner;
        case NSql::ESqlJoinType::Left: return EJoinType::Left;
        case NSql::ESqlJoinType::Right: return EJoinType::Right;
        case NSql::ESqlJoinType::Full: return EJoinType::Full;
        case NSql::ESqlJoinType::LeftSemi: return EJoinType::LeftSemi;
        case NSql::ESqlJoinType::RightSemi: return EJoinType::RightSemi;
        case NSql::ESqlJoinType::Cross: return EJoinType::Inner;
    }
    return EJoinType::Inner;
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources);

std::expected<TOperatorPtr, TError> BuildTableRef(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources)
{
    if (auto table = std::dynamic_pointer_cast<NSql::TSqlTableName>(ref)) {
        auto source = sources(TableName(table->Name));
        if (!source) {
            return std::unexpected(source.error());
        }
        if (table->Alias) {
            if (auto* op = dynamic_cast<TSourceOperator*>(source->get())) {
                op->SetAlias(*table->Alias);
            }
        }
        return *source;
    }

    if (auto sub = std::dynamic_pointer_cast<NSql::TSqlSubqueryTable>(ref)) {
        if (sub->ColumnAliases) {
            return std::unexpected(TError("column aliases on derived tables are not supported yet"));
        }
        return BuildQuery(*sub->Query, sources);
    }

    if (auto join = std::dynamic_pointer_cast<NSql::TSqlJoin>(ref)) {
        auto left = BuildTableRef(join->Left, sources);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildTableRef(join->Right, sources);
        if (!right) {
            return std::unexpected(right.error());
        }

        // The builder does not extract equi-keys from ON; the whole predicate
        // becomes a residual that the optimizer later turns into join keys.
        std::vector<TJoinKey> keys;
        NAst::TExprPtr residual;
        if (join->Condition) {
            if (join->Condition->On) {
                if (HasSubquery(join->Condition->On)) {
                    return std::unexpected(TError("subqueries are not supported yet"));
                }
                residual = join->Condition->On;
            } else if (join->Condition->UsingColumns) {
                for (const auto& column : join->Condition->UsingColumns->Items) {
                    keys.push_back({ .Left = column, .Right = column });
                }
            }
        }
        return std::make_shared<TJoinOperator>(
            *left, *right, std::move(keys), MapJoinType(join->Type), std::move(residual));
    }

    return std::unexpected(TError("unsupported table reference"));
}

std::expected<TOperatorPtr, TError> BuildFrom(
    const NSql::TSqlFrom& from,
    const TTableSourceFactory& sources)
{
    if (from.Items.empty()) {
        return std::unexpected(TError("empty FROM"));
    }
    auto node = BuildTableRef(from.Items[0], sources);
    if (!node) {
        return std::unexpected(node.error());
    }
    TOperatorPtr result = *node;

    // Comma-separated tables become a left-deep cross-join tree; the WHERE
    // predicate stays a single top-level filter (equi-join extraction is an
    // optimizer pass).
    for (size_t i = 1; i < from.Items.size(); ++i) {
        auto right = BuildTableRef(from.Items[i], sources);
        if (!right) {
            return std::unexpected(right.error());
        }
        result = std::make_shared<TJoinOperator>(
            std::move(result), *right, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
    }
    return result;
}

std::expected<TOperatorPtr, TError> BuildSelect(
    const NSql::TSqlSelect& select,
    const TTableSourceFactory& sources)
{
    if (!select.From) {
        return std::unexpected(TError("SELECT without FROM is not supported yet"));
    }
    auto base = BuildFrom(*select.From, sources);
    if (!base) {
        return std::unexpected(base.error());
    }
    TOperatorPtr node = *base;

    if (select.Where) {
        if (HasSubquery(select.Where)) {
            return std::unexpected(TError("subqueries are not supported yet"));
        }
        node = std::make_shared<TFilterOperator>(std::move(node), select.Where);
    }

    if (!select.SelectList) {
        return std::unexpected(TError("empty select list"));
    }

    TAggCollector collector;
    std::vector<TProjectionSpec> projections;
    for (size_t i = 0; i < select.SelectList->Items.size(); ++i) {
        const auto& item = select.SelectList->Items[i];
        if (item->Star) {
            return std::unexpected(TError("'*' projection is not supported yet"));
        }
        if (HasSubquery(item->Expr)) {
            return std::unexpected(TError("subqueries are not supported yet"));
        }
        std::string name = ItemName(*item, i);
        auto expr = collector.Rewrite(item->Expr);
        if (!expr) {
            return std::unexpected(expr.error());
        }
        projections.push_back({ .Name = std::move(name), .Expression = std::move(*expr) });
    }

    NAst::TExprPtr having;
    if (select.Having) {
        if (HasSubquery(select.Having)) {
            return std::unexpected(TError("subqueries are not supported yet"));
        }
        auto rewritten = collector.Rewrite(select.Having);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        having = std::move(*rewritten);
    }

    bool aggregated = select.GroupBy != nullptr || !collector.Empty();
    if (!aggregated) {
        if (having) {
            return std::unexpected(TError("HAVING requires aggregation"));
        }
        return std::make_shared<TProjectOperator>(std::move(node), std::move(projections));
    }

    std::vector<std::string> keys;
    if (select.GroupBy) {
        auto groupKeys = GroupKeys(*select.GroupBy);
        if (!groupKeys) {
            return std::unexpected(groupKeys.error());
        }
        keys = std::move(*groupKeys);
    }

    node = std::make_shared<TAggregateOperator>(
        std::move(node), std::move(keys), collector.TakeSpecs());
    if (having) {
        node = std::make_shared<TFilterOperator>(std::move(node), std::move(having));
    }
    return std::make_shared<TProjectOperator>(std::move(node), std::move(projections));
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources)
{
    if (query.WithClause) {
        return std::unexpected(TError("WITH is not supported yet"));
    }
    auto select = std::dynamic_pointer_cast<NSql::TSqlSelect>(query.Body);
    if (!select) {
        return std::unexpected(TError("expected a SELECT statement"));
    }
    // TODO: ORDER BY / LIMIT / OFFSET require sort/limit operators.
    return BuildSelect(*select, sources);
}

} // namespace

std::expected<TOperatorPtr, TError> BuildPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources)
{
    auto root = std::dynamic_pointer_cast<NSql::TSqlQuery>(query);
    if (!root) {
        return std::unexpected(TError("expected a query"));
    }
    return BuildQuery(*root, sources);
}

} // namespace NQqb

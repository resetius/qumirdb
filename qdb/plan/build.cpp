#include <qdb/plan/build.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/flatten_conjucts.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace NQdb {

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

    // Set when a count(distinct <col>) was collected: BuildSelect realizes it via
    // a double aggregation (inner dedup on the group keys ∪ <col>, then count).
    const std::optional<std::string>& DistinctColumn() const {
        return DistinctColumn_;
    }
    bool HasNonDistinct() const {
        return HasNonDistinct_;
    }

private:
    std::expected<NAst::TExprPtr, TError> Emit(const TAggCall& agg, TLocation loc) {
        if (agg.Distinct) {
            if (agg.Func != "count") {
                return std::unexpected(TError(loc, "only count(distinct ...) is supported"));
            }
            auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(agg.Arg);
            if (!ident) {
                return std::unexpected(TError(loc, "count(distinct ...) requires a column argument"));
            }
            const std::string& column = ident.Cast()->Name;
            if (DistinctColumn_ && *DistinctColumn_ != column) {
                return std::unexpected(TError(loc, "multiple distinct columns are not supported"));
            }
            DistinctColumn_ = column;
            std::string name = NextName("count");
            Specs_.push_back({ .Name = name, .Func = "count", .Arg = agg.Arg });
            return Ident(loc, name);
        }

        HasNonDistinct_ = true;

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
    std::optional<std::string> DistinctColumn_;
    bool HasNonDistinct_ = false;
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

// Renames a built sub-plan's output columns to the given list (CTE column lists
// and derived-table column aliases). The sub-plan's top must be a projection.
std::expected<TOperatorPtr, TError> ApplyColumnAliases(
    TOperatorPtr plan, const std::vector<std::string>& aliases)
{
    auto project = TMaybeOp<TProjectOperator>(plan);
    if (!project || project.Cast()->Projections().size() != aliases.size()) {
        return std::unexpected(TError("column alias count does not match output columns"));
    }
    auto& projections = project.Cast()->MutableProjections();
    for (size_t i = 0; i < projections.size(); ++i) {
        projections[i].Name = aliases[i];
    }
    return plan;
}

std::expected<TOperatorPtr, TError> BuildTableRef(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources)
{
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        auto node = table.Cast();
        auto source = sources(TableName(node->Name));
        if (!source) {
            return std::unexpected(source.error());
        }
        if (node->Alias) {
            if (auto sourceOp = TMaybeOp<TSourceOperator>(*source)) {
                sourceOp.Cast()->SetAlias(*node->Alias);
            }
        }
        return *source;
    }

    if (auto sub = NSql::TMaybeNode<NSql::TSqlSubqueryTable>(ref)) {
        auto node = sub.Cast();
        auto plan = BuildQuery(*node->Query, sources);
        if (!plan) {
            return plan;
        }
        if (node->ColumnAliases) {
            return ApplyColumnAliases(std::move(*plan), node->ColumnAliases->Items);
        }
        return plan;
    }

    if (auto maybeJoin = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        auto join = maybeJoin.Cast();
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

NAst::TExprPtr Conjoin(const std::vector<NAst::TExprPtr>& parts) {
    NAst::TExprPtr result;
    for (const auto& part : parts) {
        result = result
            ? std::make_shared<NAst::TBinaryExpr>(part->Location, NAst::TOperator("&&"), result, part)
            : part;
    }
    return result;
}

struct TDecorrelation {
    EJoinType Type;
    bool IsIn;
    std::shared_ptr<NSql::TSubqueryExpr> Subquery;
};

// Recognizes a WHERE conjunct that decorrelates into a semi/anti join:
// EXISTS / IN -> semi, NOT EXISTS / NOT IN -> anti.
std::optional<TDecorrelation> AsDecorrelation(const NAst::TExprPtr& conjunct) {
    NAst::TExprPtr inner = conjunct;
    bool negated = false;
    if (auto unary = NAst::TMaybeNode<NAst::TUnaryExpr>(inner); unary && unary.Cast()->Operator == "!") {
        negated = true;
        inner = unary.Cast()->Operand;
    }
    auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(inner);
    if (!sub) {
        return std::nullopt;
    }
    using EKind = NSql::TSubqueryExpr::EKind;
    EKind kind = sub.Cast()->Kind;
    if (kind != EKind::Exists && kind != EKind::In) {
        return std::nullopt;
    }
    return TDecorrelation{
        .Type = negated ? EJoinType::LeftAnti : EJoinType::LeftSemi,
        .IsIn = kind == EKind::In,
        .Subquery = sub.Cast(),
    };
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources);

// EXISTS: the subquery's FROM becomes the right side and its WHERE (correlation
// plus local predicates) becomes the join residual.
std::expected<TOperatorPtr, TError> DecorrelateExists(
    TOperatorPtr left,
    const NSql::TSubqueryExpr& subquery,
    EJoinType type,
    const TTableSourceFactory& sources)
{
    auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(subquery.Query->Body);
    if (!maybeSelect) {
        return std::unexpected(TError("expected a SELECT in EXISTS subquery"));
    }
    auto select = maybeSelect.Cast();
    if (subquery.Query->WithClause || select->GroupBy || select->Having) {
        return std::unexpected(TError("aggregated/CTE EXISTS subquery is not supported yet"));
    }
    if (!select->From) {
        return std::unexpected(TError("EXISTS subquery requires FROM"));
    }
    auto right = BuildFrom(*select->From, sources);
    if (!right) {
        return std::unexpected(right.error());
    }
    if (select->Where && HasSubquery(select->Where)) {
        return std::unexpected(TError("nested subqueries are not supported yet"));
    }
    return std::make_shared<TJoinOperator>(
        std::move(left), *right, std::vector<TJoinKey>{}, type, select->Where);
}

// IN: build the subquery as a full plan and join on `operand == <its only column>`.
std::expected<TOperatorPtr, TError> DecorrelateIn(
    TOperatorPtr left,
    const NSql::TSubqueryExpr& subquery,
    EJoinType type,
    const TTableSourceFactory& sources)
{
    if (!subquery.Operand || HasSubquery(subquery.Operand)) {
        return std::unexpected(TError("unsupported IN operand"));
    }
    auto right = BuildQuery(*subquery.Query, sources);
    if (!right) {
        return std::unexpected(right.error());
    }
    auto project = TMaybeOp<TProjectOperator>(*right);
    if (!project || project.Cast()->Projections().size() != 1) {
        return std::unexpected(TError("IN subquery must return exactly one column"));
    }
    auto column = Ident(subquery.Operand->Location, project.Cast()->Projections()[0].Name);
    auto residual = std::make_shared<NAst::TBinaryExpr>(
        subquery.Operand->Location, NAst::TOperator("=="), subquery.Operand, std::move(column));
    return std::make_shared<TJoinOperator>(
        std::move(left), *right, std::vector<TJoinKey>{}, type, std::move(residual));
}

// Replaces each uncorrelated scalar subquery in `expr` with a reference to a
// fresh column produced by cross-joining the (single-row) subquery plan onto
// `node`. The subquery must project exactly one column. Correlated scalar
// subqueries are not handled here.
std::expected<NAst::TExprPtr, TError> ExtractScalarSubqueries(
    NAst::TExprPtr expr,
    TOperatorPtr& node,
    const TTableSourceFactory& sources,
    int& counter)
{
    if (!expr) {
        return expr;
    }
    if (auto sub = NAst::TMaybeNode<NSql::TSubqueryExpr>(expr)) {
        if (sub.Cast()->Kind != NSql::TSubqueryExpr::EKind::Scalar) {
            return std::unexpected(TError("subquery is not supported in this position"));
        }
        auto plan = BuildQuery(*sub.Cast()->Query, sources);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        auto project = TMaybeOp<TProjectOperator>(*plan);
        if (!project || project.Cast()->Projections().size() != 1) {
            return std::unexpected(TError("scalar subquery must return exactly one column"));
        }
        std::string name = "__scalar_" + std::to_string(counter++) + "__";
        project.Cast()->MutableProjections()[0].Name = name;
        // The subquery yields one row, so a cross join just broadcasts its value.
        node = std::make_shared<TJoinOperator>(
            std::move(node), *plan, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
        return Ident(expr->Location, std::move(name));
    }
    for (NAst::TExprPtr* child : expr->MutableChildren()) {
        auto rewritten = ExtractScalarSubqueries(*child, node, sources, counter);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
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
    int scalarCounter = 0;

    if (select.Where) {
        std::vector<NAst::TExprPtr> conjuncts;
        FlattenConjuncts(select.Where, conjuncts);

        std::vector<NAst::TExprPtr> residual;
        for (const auto& conjunct : conjuncts) {
            if (auto decorrelation = AsDecorrelation(conjunct)) {
                auto joined = decorrelation->IsIn
                    ? DecorrelateIn(node, *decorrelation->Subquery, decorrelation->Type, sources)
                    : DecorrelateExists(node, *decorrelation->Subquery, decorrelation->Type, sources);
                if (!joined) {
                    return std::unexpected(joined.error());
                }
                node = std::move(*joined);
            } else {
                auto rewritten = ExtractScalarSubqueries(conjunct, node, sources, scalarCounter);
                if (!rewritten) {
                    return std::unexpected(rewritten.error());
                }
                residual.push_back(std::move(*rewritten));
            }
        }

        if (!residual.empty()) {
            node = std::make_shared<TFilterOperator>(std::move(node), Conjoin(residual));
        }
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

    // A scalar subquery in HAVING is left intact by the aggregate collector
    // (it is opaque) and decorrelated below, once the aggregate node exists.
    NAst::TExprPtr having;
    if (select.Having) {
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

    auto specs = collector.TakeSpecs();

    // count(distinct col): dedup with an inner aggregate on (group keys ∪ col),
    // then count over the deduped rows above — the double aggregation the hand
    // plans use.
    if (auto distinctColumn = collector.DistinctColumn()) {
        if (collector.HasNonDistinct()) {
            return std::unexpected(
                TError("mixing DISTINCT and non-DISTINCT aggregates is not supported"));
        }
        std::vector<std::string> dedupKeys = keys;
        if (std::find(dedupKeys.begin(), dedupKeys.end(), *distinctColumn) == dedupKeys.end()) {
            dedupKeys.push_back(*distinctColumn);
        }
        node = std::make_shared<TAggregateOperator>(
            std::move(node), std::move(dedupKeys), std::vector<TAggregateSpec>{});
    }

    // A global aggregate (no GROUP BY) still needs a grouping-key descriptor, so
    // synthesize a constant key column and group by it, as the hand-written plans do.
    bool global = keys.empty();

    // The aggregate executor requires column-reference arguments, so materialize
    // computed arguments (and pass the group keys through) in a project below.
    bool needsArgProject = global;
    for (const auto& spec : specs) {
        if (spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
            needsArgProject = true;
            break;
        }
    }
    if (needsArgProject) {
        std::vector<TProjectionSpec> argProjections;
        std::set<std::string> projected;
        auto passthrough = [&](const std::string& name) {
            if (projected.insert(name).second) {
                argProjections.push_back({ .Name = name, .Expression = Ident({}, name) });
            }
        };
        for (const auto& key : keys) {
            passthrough(key);
        }
        int counter = 0;
        for (auto& spec : specs) {
            if (!spec.Arg) {
                continue;
            }
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
                passthrough(ident.Cast()->Name);
            } else {
                std::string name = "arg_" + std::to_string(counter++);
                argProjections.push_back({ .Name = name, .Expression = spec.Arg });
                spec.Arg = Ident(spec.Arg->Location, name);
            }
        }
        if (global) {
            argProjections.push_back({
                .Name = "__group__",
                .Expression = std::make_shared<NAst::TNumberExpr>(TLocation{}, static_cast<int64_t>(1)),
            });
            keys.push_back("__group__");
        }
        node = std::make_shared<TProjectOperator>(std::move(node), std::move(argProjections));
    }

    node = std::make_shared<TAggregateOperator>(
        std::move(node), std::move(keys), std::move(specs));
    if (having) {
        auto rewritten = ExtractScalarSubqueries(having, node, sources, scalarCounter);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        node = std::make_shared<TFilterOperator>(std::move(node), std::move(*rewritten));
    }
    return std::make_shared<TProjectOperator>(std::move(node), std::move(projections));
}

// Deep-clones the expressions embedded in an operator subtree, so a sub-plan
// inlined more than once (a CTE) does not share mutable expression nodes between
// copies — QualifyColumns rewrites idents in place.
void CloneOperatorExprs(const TOperatorPtr& op) {
    if (!op) {
        return;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NAst::TMaybeNode<IOperator>(child)) {
            CloneOperatorExprs(childOp.Cast());
        }
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        filter.Cast()->MutablePredicate() = CloneExpr(filter.Cast()->Predicate());
    } else if (auto project = TMaybeOp<TProjectOperator>(op)) {
        for (auto& spec : project.Cast()->MutableProjections()) {
            spec.Expression = CloneExpr(spec.Expression);
        }
    } else if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        for (auto& spec : aggregate.Cast()->MutableAggs()) {
            if (spec.Arg) {
                spec.Arg = CloneExpr(spec.Arg);
            }
        }
    } else if (auto join = TMaybeOp<TJoinOperator>(op)) {
        if (join.Cast()->Filter()) {
            join.Cast()->MutableFilter() = CloneExpr(join.Cast()->Filter());
        }
    }
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources)
{
    auto select = NSql::TMaybeNode<NSql::TSqlSelect>(query.Body);
    if (!select) {
        return std::unexpected(TError("expected a SELECT statement"));
    }
    // TODO: ORDER BY / LIMIT / OFFSET require sort/limit operators.

    if (!query.WithClause) {
        return BuildSelect(*select.Cast(), sources);
    }
    if (query.WithClause->Recursive) {
        return std::unexpected(TError("WITH RECURSIVE is not supported yet"));
    }

    std::map<std::string, NSql::TSqlPtr<NSql::TSqlCte>> ctes;
    for (const auto& cte : query.WithClause->Ctes) {
        ctes[ToLower(cte->Name)] = cte;
    }

    // CTE-aware factory: a CTE name builds a fresh inlined copy of its query
    // (applying the optional column-alias list); other names fall back to the
    // base sources. Stack-local, so the recursive self-reference stays valid for
    // the duration of the BuildSelect call below.
    TTableSourceFactory factory =
        [&](std::string_view name) -> std::expected<TOperatorPtr, TError> {
        auto it = ctes.find(ToLower(std::string(name)));
        if (it == ctes.end()) {
            return sources(name);
        }
        auto plan = BuildQuery(*it->second->Query, factory);
        if (!plan) {
            return plan;
        }
        // Each inlined copy must own independent expression nodes.
        CloneOperatorExprs(*plan);
        if (it->second->Columns) {
            return ApplyColumnAliases(std::move(*plan), it->second->Columns->Items);
        }
        return plan;
    };

    return BuildSelect(*select.Cast(), factory);
}

} // namespace

std::expected<TOperatorPtr, TError> BuildPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources)
{
    auto root = NSql::TMaybeNode<NSql::TSqlQuery>(query);
    if (!root) {
        return std::unexpected(TError("expected a query"));
    }
    return BuildQuery(*root.Cast(), sources);
}

} // namespace NQdb

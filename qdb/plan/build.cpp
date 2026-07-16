#include <qdb/plan/build.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/flatten_conjuncts.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <utility>
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

// A plain column keeps its name; an expression is materialized into a synthetic
// `gb_<n>` column below the aggregate, and references to it are rewritten to that.
struct TGroupKey {
    std::string Name;
    NAst::TExprPtr Expression; // Ident(Name) for a plain column
    bool Computed = false;
};

// All distinct group keys (G) plus the grouping sets as index lists into them.
struct TGroupingParse {
    std::vector<TGroupKey> Keys;
    std::vector<std::vector<size_t>> Sets;
};

std::vector<NAst::TExprPtr> UnpackExprs(const NAst::TExprPtr& exprs) {
    std::vector<NAst::TExprPtr> out;
    if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(exprs)) {
        out = block.Cast()->Stmts;
    } else if (exprs) {
        out.push_back(exprs);
    }
    return out;
}

// One grouping element expands to a list of column-sets (a set = list of exprs).
std::expected<std::vector<std::vector<NAst::TExprPtr>>, TError>
ExpandGroupingElement(const NSql::TSqlNodePtr& element) {
    if (auto e = NSql::TMaybeNode<NSql::TSqlGroupingExprOrList>(element)) {
        return std::vector<std::vector<NAst::TExprPtr>>{ UnpackExprs(e.Cast()->Exprs) };
    }
    if (auto r = NSql::TMaybeNode<NSql::TSqlRollUp>(element)) {
        auto cols = UnpackExprs(r.Cast()->Exprs);
        std::vector<std::vector<NAst::TExprPtr>> sets;
        for (size_t k = cols.size() + 1; k-- > 0;) {
            sets.emplace_back(cols.begin(), cols.begin() + k);
        }
        return sets;
    }
    if (auto c = NSql::TMaybeNode<NSql::TSqlCube>(element)) {
        auto cols = UnpackExprs(c.Cast()->Exprs);
        if (cols.size() > 12) {
            return std::unexpected(TError("CUBE with more than 12 columns is not supported"));
        }
        std::vector<std::vector<NAst::TExprPtr>> sets;
        for (size_t mask = (size_t{1} << cols.size()); mask-- > 0;) {
            std::vector<NAst::TExprPtr> s;
            for (size_t i = 0; i < cols.size(); ++i) {
                if (mask & (size_t{1} << i)) s.push_back(cols[i]);
            }
            sets.push_back(std::move(s));
        }
        return sets;
    }
    if (auto gs = NSql::TMaybeNode<NSql::TSqlGroupingSet>(element)) {
        std::vector<std::vector<NAst::TExprPtr>> sets;
        for (const auto& item : gs.Cast()->Items) {
            auto sub = ExpandGroupingElement(item);
            if (!sub) return std::unexpected(sub.error());
            for (auto& s : *sub) sets.push_back(std::move(s));
        }
        return sets;
    }
    return std::unexpected(TError("unsupported grouping element"));
}

std::expected<TGroupingParse, TError> ParseGrouping(const NSql::TSqlGroupBy& groupBy) {
    // Multiple grouping elements combine as a cross product of their set-lists.
    std::vector<std::vector<NAst::TExprPtr>> combos = {{}};
    for (const auto& item : groupBy.Items) {
        auto elementSets = ExpandGroupingElement(item);
        if (!elementSets) {
            return std::unexpected(elementSets.error());
        }
        std::vector<std::vector<NAst::TExprPtr>> next;
        for (const auto& base : combos) {
            for (const auto& choice : *elementSets) {
                auto merged = base;
                merged.insert(merged.end(), choice.begin(), choice.end());
                next.push_back(std::move(merged));
            }
        }
        combos = std::move(next);
    }

    TGroupingParse result;
    std::unordered_map<std::string, size_t> keyIndex; // PrintAst -> index in Keys
    auto keyFor = [&](const NAst::TExprPtr& expr) -> std::expected<size_t, TError> {
        std::string k = NAst::NCore::PrintAst(expr);
        if (auto it = keyIndex.find(k); it != keyIndex.end()) {
            return it->second;
        }
        TGroupKey gk;
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
            gk = { .Name = ident.Cast()->Name, .Expression = Ident(expr->Location, ident.Cast()->Name) };
        } else if (HasSubquery(expr)) {
            return std::unexpected(TError(expr->Location, "GROUP BY on a subquery is not supported"));
        } else {
            gk = { .Name = "gb_" + std::to_string(result.Keys.size()), .Expression = expr, .Computed = true };
        }
        size_t idx = result.Keys.size();
        result.Keys.push_back(std::move(gk));
        keyIndex[k] = idx;
        return idx;
    };
    for (const auto& combo : combos) {
        std::vector<size_t> set;
        for (const auto& expr : combo) {
            auto idx = keyFor(expr);
            if (!idx) return std::unexpected(idx.error());
            if (std::find(set.begin(), set.end(), *idx) == set.end()) {
                set.push_back(*idx);
            }
        }
        result.Sets.push_back(std::move(set));
    }
    return result;
}

// Rewrites subtrees equal to a computed group key into a reference to its column:
// the base columns no longer exist above the aggregate. Matched top-down.
NAst::TExprPtr SubstituteGroupKeys(
    NAst::TExprPtr expr,
    const std::unordered_map<std::string, std::string>& byExpr)
{
    if (!expr) {
        return expr;
    }
    if (auto it = byExpr.find(NAst::NCore::PrintAst(expr)); it != byExpr.end()) {
        return Ident(expr->Location, it->second);
    }
    for (auto* child : expr->MutableChildren()) {
        *child = SubstituteGroupKeys(*child, byExpr);
    }
    return expr;
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

// Columns produced by a subquery's own FROM tables (base tables, recursing into
// joins). Used to tell a subquery's local columns from outer (correlated) ones.
void CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources,
    std::unordered_set<std::string>& out)
{
    if (auto table = NSql::TMaybeNode<NSql::TSqlTableName>(ref)) {
        if (auto src = sources(TableName(table.Cast()->Name))) {
            if (auto source = TMaybeOp<TSourceOperator>(*src)) {
                for (const auto& col : source.Cast()->GetSource().Schema().Columns) {
                    out.insert(std::string(col.Name));
                }
            }
        }
    } else if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        CollectLocalColumns(join.Cast()->Left, sources, out);
        CollectLocalColumns(join.Cast()->Right, sources, out);
    }
}

// Replaces each scalar subquery in `expr` with a reference to a fresh column.
// Uncorrelated subqueries broadcast via a cross join (a single row). Correlated
// ones are decorrelated: the subquery is grouped by its correlation columns and
// LEFT-joined onto `node` (Apply elimination).
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
        auto scalar = sub.Cast();
        if (scalar->Kind != NSql::TSubqueryExpr::EKind::Scalar) {
            return std::unexpected(TError("subquery is not supported in this position"));
        }

        auto maybeSelect = NSql::TMaybeNode<NSql::TSqlSelect>(scalar->Query->Body);
        if (maybeSelect && !scalar->Query->WithClause
            && maybeSelect.Cast()->From && maybeSelect.Cast()->Where)
        {
            auto sel = maybeSelect.Cast();
            std::unordered_set<std::string> local;
            for (const auto& item : sel->From->Items) {
                CollectLocalColumns(item, sources, local);
            }

            // Correlation = an equality with one local and one outer column.
            std::vector<std::pair<std::string, std::string>> correlation; // (outer, local)
            std::vector<NAst::TExprPtr> localPreds;
            std::vector<NAst::TExprPtr> conjuncts;
            FlattenConjuncts(sel->Where, conjuncts);
            for (const auto& conj : conjuncts) {
                bool isCorrelation = false;
                if (auto binary = NAst::TMaybeNode<NAst::TBinaryExpr>(conj);
                    binary && binary.Cast()->Operator == "==")
                {
                    auto left = NAst::TMaybeNode<NAst::TIdentExpr>(binary.Cast()->Left);
                    auto right = NAst::TMaybeNode<NAst::TIdentExpr>(binary.Cast()->Right);
                    if (left && right) {
                        const std::string& ln = left.Cast()->Name;
                        const std::string& rn = right.Cast()->Name;
                        bool lLocal = local.count(ln) > 0;
                        bool rLocal = local.count(rn) > 0;
                        if (lLocal && !rLocal) {
                            correlation.emplace_back(rn, ln);
                            isCorrelation = true;
                        } else if (rLocal && !lLocal) {
                            correlation.emplace_back(ln, rn);
                            isCorrelation = true;
                        }
                    }
                }
                if (!isCorrelation) {
                    localPreds.push_back(conj);
                }
            }

            if (!correlation.empty()) {
                int id = counter++;
                // Group by the correlation columns (exposed under fresh names) and
                // keep only the local predicates.
                if (!sel->GroupBy) {
                    sel->GroupBy = std::make_shared<NSql::TSqlGroupBy>();
                }
                std::vector<NSql::TSqlPtr<NSql::TSqlSelectItem>> prepend;
                std::vector<TJoinKey> joinKeys;
                for (size_t k = 0; k < correlation.size(); ++k) {
                    const auto& [outerCol, localCol] = correlation[k];
                    std::string corrName =
                        "__corr_" + std::to_string(id) + "_" + std::to_string(k) + "__";
                    auto item = std::make_shared<NSql::TSqlSelectItem>();
                    item->Expr = Ident({}, localCol);
                    item->Alias = corrName;
                    prepend.push_back(std::move(item));
                    sel->GroupBy->Items.push_back(
                        std::make_shared<NSql::TSqlGroupingExprOrList>(Ident({}, localCol)));
                    joinKeys.push_back({ .Left = outerCol, .Right = corrName });
                }
                sel->SelectList->Items.insert(
                    sel->SelectList->Items.begin(), prepend.begin(), prepend.end());
                sel->Where = localPreds.empty() ? nullptr : Conjoin(localPreds);

                auto plan = BuildQuery(*scalar->Query, sources);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
                auto project = TMaybeOp<TProjectOperator>(*plan);
                if (!project
                    || project.Cast()->Projections().size() != correlation.size() + 1)
                {
                    return std::unexpected(
                        TError("correlated scalar subquery must return one value"));
                }
                std::string scalarName = "__scalar_" + std::to_string(id) + "__";
                project.Cast()->MutableProjections().back().Name = scalarName;
                // LEFT join preserves outer rows; a missing match yields NULL, so
                // the surrounding comparison filters the row out (SQL semantics).
                node = std::make_shared<TJoinOperator>(
                    std::move(node), *plan, std::move(joinKeys), EJoinType::Left, nullptr);
                return Ident(expr->Location, std::move(scalarName));
            }
        }

        // Uncorrelated: the subquery yields one row, broadcast via a cross join.
        auto plan = BuildQuery(*scalar->Query, sources);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        auto project = TMaybeOp<TProjectOperator>(*plan);
        if (!project || project.Cast()->Projections().size() != 1) {
            return std::unexpected(TError("scalar subquery must return exactly one column"));
        }
        std::string name = "__scalar_" + std::to_string(counter++) + "__";
        project.Cast()->MutableProjections()[0].Name = name;
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

    std::vector<TGroupKey> groupKeys;
    std::vector<std::vector<size_t>> groupingSets;
    if (select.GroupBy) {
        auto parsed = ParseGrouping(*select.GroupBy);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        groupKeys = std::move(parsed->Keys);
        groupingSets = std::move(parsed->Sets);
    }
    // A single set over all keys is a plain aggregate; >1 set needs the kernel's
    // grouping-sets machinery (keys become nullable, masked per set).
    bool multiSet = groupingSets.size() > 1;

    std::unordered_map<std::string, std::string> computedKeys; // PrintAst -> name
    for (const auto& key : groupKeys) {
        if (key.Computed) {
            computedKeys.emplace(NAst::NCore::PrintAst(key.Expression), key.Name);
        }
    }
    if (!computedKeys.empty()) {
        for (auto& projection : projections) {
            projection.Expression = SubstituteGroupKeys(projection.Expression, computedKeys);
        }
        having = SubstituteGroupKeys(having, computedKeys);
    }

    std::vector<std::string> keys;
    keys.reserve(groupKeys.size());
    for (const auto& key : groupKeys) {
        keys.push_back(key.Name);
    }

    auto specs = collector.TakeSpecs();

    // count(distinct col): dedup with an inner aggregate on (group keys ∪ col),
    // then count over the deduped rows above — the double aggregation the hand
    // plans use.
    if (multiSet && collector.DistinctColumn()) {
        return std::unexpected(
            TError("GROUPING SETS with COUNT(DISTINCT) is not supported yet"));
    }

    if (auto distinctColumn = collector.DistinctColumn()) {
        if (collector.HasNonDistinct()) {
            return std::unexpected(
                TError("mixing DISTINCT and non-DISTINCT aggregates is not supported"));
        }
        if (!computedKeys.empty()) {
            return std::unexpected(
                TError("GROUP BY on an expression with COUNT(DISTINCT) is not supported yet"));
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
    // Grouping sets also force it so the input is exactly [keys..., args...], which
    // the masked-batch driver relies on.
    bool needsArgProject = global || !computedKeys.empty() || multiSet;
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
        for (const auto& key : groupKeys) {
            if (projected.insert(key.Name).second) {
                argProjections.push_back({ .Name = key.Name, .Expression = key.Expression });
            }
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

    auto aggregate = std::make_shared<TAggregateOperator>(
        std::move(node), std::move(keys), std::move(specs));
    if (multiSet) {
        aggregate->MutableGroupingSets() = std::move(groupingSets);
    }
    node = aggregate;
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

bool HasOutputColumn(const TOperatorPtr& op, const std::string& name) {
    auto* output = static_cast<NAst::TStructType*>(op->OutputColumns().get());
    if (!output) {
        return false;
    }
    for (const auto& [fieldName, _] : output->Fields) {
        if (fieldName == name) {
            return true;
        }
    }
    return false;
}

ESortNulls ConvertSortNulls(NSql::TSqlOrderItem::ENullOrder nulls) {
    switch (nulls) {
        case NSql::TSqlOrderItem::ENullOrder::Default:
            return ESortNulls::Default;
        case NSql::TSqlOrderItem::ENullOrder::First:
            return ESortNulls::First;
        case NSql::TSqlOrderItem::ENullOrder::Last:
            return ESortNulls::Last;
    }
    return ESortNulls::Default;
}

std::expected<TOperatorPtr, TError> ApplyOrderBy(
    const NSql::TSqlQuery& query,
    TOperatorPtr plan)
{
    if (!query.OrderBy || query.OrderBy->Items.empty()) {
        return plan;
    }

    // BuildSelect always tops the plan with a projection. An ORDER BY item is
    // resolved in order of preference:
    //   1. a plain output-column identifier -> sort on it directly;
    //   2. an expression already computed in the select list (matched
    //      structurally) -> sort on that output column, so aggregates and other
    //      computed columns are reused rather than re-evaluated;
    //   3. a pure-scalar expression over the projection's input -> materialized
    //      as a hidden column, sorted on, then stripped by a final projection so
    //      the output schema is unchanged.
    // An aggregate not present in the select list cannot be sorted on (it would
    // be re-applied as a scalar over already-grouped rows), so it is rejected.
    auto topProject = TMaybeOp<TProjectOperator>(plan);

    // Index the original (pre-lowering) select-list expressions by structure, so
    // ORDER BY can match one and reuse its output column. The plan's projection
    // expressions are unusable here: the aggregate collector has already
    // rewritten e.g. sum(x) into a reference to a synthetic column.
    std::unordered_map<std::string, std::string> outputByExpr; // PrintAst -> name
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(query.Body);
        select && select.Cast()->SelectList) {
        const auto& items = select.Cast()->SelectList->Items;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!items[i]->Star) {
                outputByExpr.emplace(NAst::NCore::PrintAst(items[i]->Expr), ItemName(*items[i], i));
            }
        }
    }

    std::vector<TSortKey> keys;
    keys.reserve(query.OrderBy->Items.size());
    std::vector<TProjectionSpec> hidden;
    int sortCounter = 0;

    for (const auto& item : query.OrderBy->Items) {
        std::string column;
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(item->Expr);
        if (ident && HasOutputColumn(plan, ident.Cast()->Name)) {
            column = ident.Cast()->Name;
        } else if (auto it = outputByExpr.find(NAst::NCore::PrintAst(item->Expr));
                   it != outputByExpr.end()) {
            column = it->second;
        } else {
            if (!topProject) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an expression is not supported here"));
            }
            // Reuse the aggregate collector purely to detect aggregate calls:
            // a non-empty result means the expression aggregates but is not in
            // the select list, which cannot be materialized in a scalar project.
            TAggCollector probe;
            auto scalar = probe.Rewrite(item->Expr);
            if (!scalar) {
                return std::unexpected(scalar.error());
            }
            if (!probe.Empty()) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY on an aggregate not in the select list is not supported"));
            }
            column = "__sort_" + std::to_string(sortCounter++);
            hidden.push_back({ .Name = column, .Expression = item->Expr });
        }

        keys.push_back({
            .Column = column,
            .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
            .Nulls = ConvertSortNulls(item->NullOrder),
        });
    }

    if (hidden.empty()) {
        return std::make_shared<TSortOperator>(std::move(plan), std::move(keys));
    }

    // Pass-through projection over the original outputs, captured before the
    // hidden columns are appended; it becomes the strip projection above the sort.
    std::vector<TProjectionSpec> outputProjections;
    outputProjections.reserve(topProject.Cast()->Projections().size());
    for (const auto& p : topProject.Cast()->Projections()) {
        outputProjections.push_back({ .Name = p.Name, .Expression = Ident({}, p.Name) });
    }

    auto& projections = topProject.Cast()->MutableProjections();
    for (auto& h : hidden) {
        projections.push_back(std::move(h));
    }

    TOperatorPtr sorted = std::make_shared<TSortOperator>(std::move(plan), std::move(keys));
    return std::make_shared<TProjectOperator>(std::move(sorted), std::move(outputProjections));
}

std::expected<int64_t, TError> ConstNonNegativeI64(
    const NAst::TExprPtr& expr,
    std::string_view clauseName)
{
    if (!expr) {
        return int64_t{0};
    }
    auto number = NAst::TMaybeNode<NAst::TNumberExpr>(expr);
    if (!number || number.Cast()->IsFloat()) {
        return std::unexpected(TError(expr->Location,
            std::string(clauseName) + " currently requires an integer literal"));
    }
    if (number.Cast()->IntValue < 0) {
        return std::unexpected(TError(expr->Location,
            std::string(clauseName) + " must be non-negative"));
    }
    return number.Cast()->IntValue;
}

std::expected<TOperatorPtr, TError> ApplyLimit(
    const NSql::TSqlQuery& query,
    TOperatorPtr plan)
{
    if (!query.Limit && !query.Offset) {
        return plan;
    }
    if (!query.Limit) {
        return std::unexpected(TError(query.Offset->Location,
            "OFFSET without LIMIT is not supported yet"));
    }

    auto limit = ConstNonNegativeI64(query.Limit, "LIMIT");
    if (!limit) {
        return std::unexpected(limit.error());
    }
    int64_t offsetValue = 0;
    if (query.Offset) {
        auto offset = ConstNonNegativeI64(query.Offset, "OFFSET");
        if (!offset) {
            return std::unexpected(offset.error());
        }
        offsetValue = *offset;
    }
    return std::make_shared<TLimitOperator>(std::move(plan), *limit, offsetValue);
}

std::expected<TOperatorPtr, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    const TTableSourceFactory& sources);

// Builds a query body (a SELECT, a set operation, or a parenthesized query).
std::expected<TOperatorPtr, TError> BuildQueryBody(
    const NSql::TSqlNodePtr& body,
    const TTableSourceFactory& sources)
{
    if (auto select = NSql::TMaybeNode<NSql::TSqlSelect>(body)) {
        return BuildSelect(*select.Cast(), sources);
    }
    if (auto setOp = NSql::TMaybeNode<NSql::TSqlSetOp>(body)) {
        return BuildSetOp(*setOp.Cast(), sources);
    }
    if (auto sub = NSql::TMaybeNode<NSql::TSqlQuery>(body)) {
        return BuildQuery(*sub.Cast(), sources);
    }
    return std::unexpected(TError("unsupported query body"));
}

std::expected<TOperatorPtr, TError> BuildSetOp(
    const NSql::TSqlSetOp& setOp,
    const TTableSourceFactory& sources)
{
    if (setOp.Op != NSql::TSqlSetOp::EOp::Union
        || setOp.Quantifier != NSql::ESetQuantifier::All)
    {
        return std::unexpected(TError(
            "only UNION ALL is supported yet (UNION/INTERSECT/EXCEPT DISTINCT not supported)"));
    }

    // Flatten a nested chain of UNION ALL into a single N-ary operator.
    std::vector<TOperatorPtr> branches;
    std::function<std::expected<void, TError>(const NSql::TSqlNodePtr&)> collect =
        [&](const NSql::TSqlNodePtr& node) -> std::expected<void, TError> {
        auto inner = NSql::TMaybeNode<NSql::TSqlSetOp>(node);
        if (inner
            && inner.Cast()->Op == NSql::TSqlSetOp::EOp::Union
            && inner.Cast()->Quantifier == NSql::ESetQuantifier::All)
        {
            if (auto l = collect(inner.Cast()->Left); !l) return l;
            return collect(inner.Cast()->Right);
        }
        auto branch = BuildQueryBody(node, sources);
        if (!branch) {
            return std::unexpected(branch.error());
        }
        branches.push_back(std::move(*branch));
        return {};
    };
    if (auto l = collect(setOp.Left); !l) return std::unexpected(l.error());
    if (auto r = collect(setOp.Right); !r) return std::unexpected(r.error());

    return std::make_shared<TUnionAllOperator>(std::move(branches));
}

std::expected<TOperatorPtr, TError> BuildQuery(
    const NSql::TSqlQuery& query,
    const TTableSourceFactory& sources)
{
    if (!query.WithClause) {
        auto plan = BuildQueryBody(query.Body, sources);
        if (!plan) {
            return plan;
        }
        auto ordered = ApplyOrderBy(query, std::move(*plan));
        if (!ordered) {
            return ordered;
        }
        return ApplyLimit(query, std::move(*ordered));
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

    auto plan = BuildQueryBody(query.Body, factory);
    if (!plan) {
        return plan;
    }
    auto ordered = ApplyOrderBy(query, std::move(*plan));
    if (!ordered) {
        return ordered;
    }
    return ApplyLimit(query, std::move(*ordered));
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

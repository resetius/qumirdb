#include <qdb/plan/build.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/flatten_conjuncts.h>
#include <qdb/plan/types/decimal.h>

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
    static const std::set<std::string> funcs = {
        "sum", "count", "avg", "min", "max", "stddev_samp"};
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

bool IsDecimalExprHint(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (auto cast = NAst::TMaybeNode<NAst::TCastExpr>(expr)) {
        return IsDecimalType(cast.Cast()->Type);
    }
    return IsDecimalType(expr->Type);
}

NAst::TExprPtr CastF64(TLocation loc, NAst::TExprPtr expr) {
    return std::make_shared<NAst::TCastExpr>(
        std::move(loc), std::move(expr), std::make_shared<NAst::TFloatType>());
}

NAst::TExprPtr F64Literal(TLocation loc, double value) {
    return std::make_shared<NAst::TNumberExpr>(std::move(loc), value);
}

NAst::TExprPtr Call(TLocation loc, std::string name, std::vector<NAst::TExprPtr> args) {
    return std::make_shared<NAst::TCallExpr>(
        loc, Ident(loc, std::move(name)), std::move(args));
}

NAst::TExprPtr Binary(
    TLocation loc,
    std::string op,
    NAst::TExprPtr left,
    NAst::TExprPtr right)
{
    return std::make_shared<NAst::TBinaryExpr>(
        std::move(loc), NAst::TOperator(std::move(op)), std::move(left), std::move(right));
}

bool IsStddevSupportedType(const NAst::TTypePtr& type) {
    auto value = NAst::UnwrapNamedType(UnwrapNullableType(type));
    return NAst::TMaybeType<NAst::TIntegerType>(value) ||
        NAst::TMaybeType<NAst::TFloatType>(value);
}

bool IsFloatType(const NAst::TTypePtr& type) {
    return static_cast<bool>(
        NAst::TMaybeType<NAst::TFloatType>(NAst::UnwrapNamedType(UnwrapNullableType(type))));
}

// Replaces aggregate calls in an expression with references to synthetic
// aggregate output columns, collecting the corresponding specs.
class TAggCollector {
public:
    explicit TAggCollector(const NAst::TStructType* inputSchema = nullptr)
        : InputSchema_(inputSchema)
    { }

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
    std::expected<NAst::TTypePtr, TError> StddevArgType(
        const TAggCall& agg,
        TLocation loc) const
    {
        if (!agg.Arg || agg.Star) {
            return std::unexpected(TError(loc, "stddev_samp requires one argument"));
        }
        if (IsDecimalExprHint(agg.Arg)) {
            return std::unexpected(TError(loc, "stddev_samp(decimal) is not supported in qdb v1"));
        }
        if (auto cast = NAst::TMaybeNode<NAst::TCastExpr>(agg.Arg);
            cast && (!InputSchema_ || InputSchema_->Fields.empty()))
        {
            if (!IsStddevSupportedType(cast.Cast()->Type)) {
                return std::unexpected(TError(loc,
                    "stddev_samp argument must be integer or f64 in qdb v1"));
            }
            return cast.Cast()->Type;
        }
        if (!InputSchema_ || InputSchema_->Fields.empty()) {
            return NAst::TTypePtr{};
        }

        NAst::TTypePtr type;
        try {
            type = NKernel::AnnotateExprType(agg.Arg, *InputSchema_);
        } catch (const TError& error) {
            return std::unexpected(error);
        }
        if (IsDecimalType(type)) {
            return std::unexpected(TError(loc, "stddev_samp(decimal) is not supported in qdb v1"));
        }
        if (!IsStddevSupportedType(type)) {
            return std::unexpected(TError(loc,
                "stddev_samp argument must be integer or f64 in qdb v1"));
        }
        return type;
    }

    std::expected<NAst::TExprPtr, TError> EmitStddevSamp(const TAggCall& agg, TLocation loc) {
        auto type = StddevArgType(agg, loc);
        if (!type) {
            return std::unexpected(type.error());
        }

        const bool needsCast = *type && !IsFloatType(*type);
        auto valueArg = [&](TLocation argLoc) -> NAst::TExprPtr {
            auto arg = CloneExpr(agg.Arg);
            return needsCast ? CastF64(std::move(argLoc), std::move(arg)) : arg;
        };

        std::string sumName = NextName("sum");
        Specs_.push_back({
            .Name = sumName,
            .Func = "sum",
            .Arg = valueArg(loc),
        });

        std::string sumSqName = NextName("sum");
        auto squared = Binary(loc, "*", valueArg(loc), valueArg(loc));
        Specs_.push_back({
            .Name = sumSqName,
            .Func = "sum",
            .Arg = std::move(squared),
        });

        std::string countName = NextName("count");
        Specs_.push_back({
            .Name = countName,
            .Func = "count",
            .Arg = CloneExpr(agg.Arg),
        });

        auto count = Ident(loc, countName);
        auto countF = CastF64(loc, Ident(loc, countName));
        auto numerator = Binary(loc, "-",
            Ident(loc, sumSqName),
            Binary(loc, "/",
                Binary(loc, "*", Ident(loc, sumName), Ident(loc, sumName)),
                CastF64(loc, Ident(loc, countName))));
        auto variance = Binary(loc, "/",
            std::move(numerator),
            Binary(loc, "-", std::move(countF), F64Literal(loc, 1.0)));
        auto stddev = Call(loc, "qdb_sqrt", {std::move(variance)});
        return std::make_shared<NAst::TIfExpr>(
            loc,
            Binary(loc, "<", std::move(count), std::make_shared<NAst::TNumberExpr>(loc, int64_t{2})),
            Call(loc, "qdb_sql_null", {}),
            std::move(stddev));
    }

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

        if (agg.Func == "stddev_samp") {
            return EmitStddevSamp(agg, loc);
        }

        // avg has no aggregate kernel; express it as sum / count.
        if (agg.Func == "avg") {
            std::string sumName = NextName("sum");
            Specs_.push_back({ .Name = sumName, .Func = "sum", .Arg = agg.Arg });
            std::string countName = NextName("count");
            Specs_.push_back({ .Name = countName, .Func = "count", .Arg = nullptr });

            NAst::TExprPtr count = Ident(loc, countName);
            if (!IsDecimalExprHint(agg.Arg)) {
                count = std::make_shared<NAst::TCastExpr>(
                    loc, std::move(count), std::make_shared<NAst::TFloatType>());
            }
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
    const NAst::TStructType* InputSchema_ = nullptr;
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
    bool HasGroupingSyntax = false;
};

bool IsGroupingSyntax(const NSql::TSqlNodePtr& element) {
    return NSql::TMaybeNode<NSql::TSqlRollUp>(element)
        || NSql::TMaybeNode<NSql::TSqlCube>(element)
        || NSql::TMaybeNode<NSql::TSqlGroupingSet>(element);
}

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
    for (const auto& item : groupBy.Items) {
        if (IsGroupingSyntax(item)) {
            result.HasGroupingSyntax = true;
            break;
        }
    }
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

bool IsGroupingCall(const NAst::TExprPtr& expr) {
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr);
    if (!call) {
        return false;
    }
    auto callee = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee);
    return callee && ToLower(callee.Cast()->Name) == "grouping";
}

bool HasGroupingCall(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (IsGroupingCall(expr)) {
        return true;
    }
    for (const auto& child : expr->Children()) {
        if (HasGroupingCall(child)) {
            return true;
        }
    }
    return false;
}

NAst::TExprPtr I64Literal(TLocation loc, int64_t value) {
    return std::make_shared<NAst::TNumberExpr>(std::move(loc), value);
}

NAst::TExprPtr I32Literal(TLocation loc, int32_t value) {
    auto ret = std::make_shared<NAst::TNumberExpr>(std::move(loc), static_cast<int64_t>(value));
    ret->Type = std::make_shared<NAst::TIntegerType>(NAst::TIntegerType::I32);
    return ret;
}

struct TGroupingRewriteInfo {
    bool HasGroupingSyntax = false;
    std::unordered_map<std::string, size_t> KeyIndex;
    std::vector<std::vector<size_t>> Sets;
};

TGroupingRewriteInfo MakeGroupingRewriteInfo(
    const std::vector<TGroupKey>& keys,
    const std::vector<std::vector<size_t>>& sets,
    bool hasGroupingSyntax)
{
    TGroupingRewriteInfo info;
    info.HasGroupingSyntax = hasGroupingSyntax;
    info.Sets = sets;
    for (size_t i = 0; i < keys.size(); ++i) {
        info.KeyIndex.emplace(NAst::NCore::PrintAst(keys[i].Expression), i);
        info.KeyIndex.emplace(NAst::NCore::PrintAst(Ident({}, keys[i].Name)), i);
    }
    return info;
}

bool GroupingSetContainsKey(const std::vector<size_t>& set, size_t keyIndex) {
    return std::find(set.begin(), set.end(), keyIndex) != set.end();
}

std::expected<NAst::TExprPtr, TError> BuildGroupingValue(
    const NAst::TExprPtr& expr,
    const TGroupingRewriteInfo& info)
{
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr);
    if (!call || call.Cast()->Args.size() != 1) {
        return std::unexpected(TError(expr->Location, "GROUPING() expects exactly one argument"));
    }
    if (!info.HasGroupingSyntax) {
        return std::unexpected(TError(expr->Location,
            "GROUPING() requires GROUPING SETS, ROLLUP, or CUBE"));
    }

    auto arg = call.Cast()->Args[0];
    auto key = info.KeyIndex.find(NAst::NCore::PrintAst(arg));
    if (key == info.KeyIndex.end()) {
        return std::unexpected(TError(arg->Location,
            "GROUPING() argument must be a GROUP BY key"));
    }

    NAst::TExprPtr result = I64Literal(expr->Location, 0);
    for (size_t setId = info.Sets.size(); setId-- > 0;) {
        if (GroupingSetContainsKey(info.Sets[setId], key->second)) {
            continue;
        }
        auto condition = std::make_shared<NAst::TBinaryExpr>(
            expr->Location,
            NAst::TOperator("=="),
            Ident(expr->Location, "__grouping_id__"),
            I32Literal(expr->Location, static_cast<int32_t>(setId)));
        result = std::make_shared<NAst::TIfExpr>(
            expr->Location,
            std::move(condition),
            I64Literal(expr->Location, 1),
            std::move(result));
    }
    return result;
}

std::expected<NAst::TExprPtr, TError> RewriteGroupingCalls(
    NAst::TExprPtr expr,
    const TGroupingRewriteInfo& info)
{
    if (!expr) {
        return expr;
    }
    if (IsGroupingCall(expr)) {
        return BuildGroupingValue(expr, info);
    }
    for (auto* child : expr->MutableChildren()) {
        auto rewritten = RewriteGroupingCalls(*child, info);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
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
    // Rebuild through the ctor so the operator's output Type is recomputed from
    // the renamed columns (mutating Projections in place would leave it stale).
    std::vector<TProjectionSpec> renamed = project.Cast()->Projections();
    for (size_t i = 0; i < renamed.size(); ++i) {
        renamed[i].Name = aliases[i];
    }
    return std::make_shared<TProjectOperator>(project.Cast()->Input(), std::move(renamed));
}

// Qualifies a subplan's output columns as `alias.col`. Source tables get this from
// AssignSourceAliases, but subqueries/CTEs used with an alias need it here so the
// outer query can reference `alias.col` (and equi-join keys on them are extracted).
TOperatorPtr AliasSubplan(TOperatorPtr plan, const std::string& alias) {
    auto* schema = static_cast<NAst::TStructType*>(plan->OutputColumns().get());
    if (!schema) {
        return plan;
    }
    std::vector<TProjectionSpec> projections;
    projections.reserve(schema->Fields.size());
    for (const auto& [name, _] : schema->Fields) {
        auto dot = name.rfind('.');
        std::string bare = dot != std::string::npos ? name.substr(dot + 1) : name;
        projections.push_back({ .Name = alias + "." + bare, .Expression = Ident({}, name) });
    }
    return std::make_shared<TProjectOperator>(std::move(plan), std::move(projections));
}

// SELECT DISTINCT: dedup by grouping on every output column. A pass-through project
// on top keeps a project-typed root (scalar-subquery/CTE handling relies on it) and
// preserves column order.
TOperatorPtr ApplyDistinct(TOperatorPtr projected, const std::vector<std::string>& columns) {
    auto agg = std::make_shared<TAggregateOperator>(
        std::move(projected), columns, std::vector<TAggregateSpec>{});
    std::vector<TProjectionSpec> passthrough;
    passthrough.reserve(columns.size());
    for (const auto& col : columns) {
        passthrough.push_back({ .Name = col, .Expression = Ident({}, col) });
    }
    return std::make_shared<TProjectOperator>(std::move(agg), std::move(passthrough));
}

std::expected<std::vector<std::string>, TError> OutputColumnNames(
    const TOperatorPtr& op,
    const std::string& context)
{
    auto* output = static_cast<NAst::TStructType*>(op->OutputColumns().get());
    if (!output) {
        return std::unexpected(TError(context + " output schema must be a struct"));
    }
    std::vector<std::string> names;
    names.reserve(output->Fields.size());
    for (const auto& [name, _] : output->Fields) {
        names.push_back(name);
    }
    return names;
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
            } else {
                // A CTE inlined as a subplan: qualify its columns by the alias.
                return AliasSubplan(std::move(*source), *node->Alias);
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
            plan = ApplyColumnAliases(std::move(*plan), node->ColumnAliases->Items);
            if (!plan) {
                return plan;
            }
        }
        if (node->Alias) {
            return AliasSubplan(std::move(*plan), *node->Alias);
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
    // Rename the subquery's output to a synthetic name so the residual's two sides
    // never share a name: otherwise `x IN (SELECT x …)` yields `x == x`, which
    // qualification cannot bind to the left side and key extraction drops entirely,
    // leaving a keyless (unschedulable, condition-less) semi-join. The name is
    // internal — a semi/anti join exposes only its left side.
    auto& spec = project.Cast()->MutableProjections()[0];
    spec.Name = "__qdb_in__" + spec.Name;
    auto column = Ident(subquery.Operand->Location, spec.Name);
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
        auto node = table.Cast();
        if (auto src = sources(TableName(node->Name))) {
            if (auto source = TMaybeOp<TSourceOperator>(*src)) {
                // Qualify by the table's alias (or its name), so a local column of
                // `item j` (`j.i_category`) is distinguishable from an outer column
                // of another alias of the same table (`i.i_category`); a bare name
                // is also kept for unqualified references.
                const std::string prefix =
                    node->Alias ? *node->Alias : node->Name.back();
                for (const auto& col : source.Cast()->GetSource().Schema().Columns) {
                    out.insert(std::string(col.Name));
                    out.insert(prefix + "." + std::string(col.Name));
                }
            }
        }
    } else if (auto join = NSql::TMaybeNode<NSql::TSqlJoin>(ref)) {
        CollectLocalColumns(join.Cast()->Left, sources, out);
        CollectLocalColumns(join.Cast()->Right, sources, out);
    }
}

// Collects correlation equalities (outer_col == local_col) anywhere in a subquery
// predicate — including inside OR/AND — so a correlation buried in a disjunction is
// lifted too (not only top-level conjuncts).
void CollectCorrelations(
    const NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& local,
    std::vector<std::pair<std::string, std::string>>& out) // (outer, local)
{
    if (!expr) {
        return;
    }
    if (auto binary = NAst::TMaybeNode<NAst::TBinaryExpr>(expr);
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
                out.emplace_back(rn, ln);
                return;
            }
            if (rLocal && !lLocal) {
                out.emplace_back(ln, rn);
                return;
            }
        }
    }
    for (auto* child : expr->MutableChildren()) {
        CollectCorrelations(*child, local, out);
    }
}

// Renames identifier leaves in place per `rename` (outer column -> local column).
void RenameIdents(
    const NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& rename)
{
    if (!expr) {
        return;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        if (auto it = rename.find(ident.Cast()->Name); it != rename.end()) {
            ident.Cast()->Name = it->second;
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        RenameIdents(*child, rename);
    }
}

// True if any identifier leaf is not a local column (a leftover outer reference not
// covered by a correlation equality — that shape we cannot decorrelate).
bool HasUnmappedOuter(
    const NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& local)
{
    if (!expr) {
        return false;
    }
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        return local.count(ident.Cast()->Name) == 0;
    }
    for (auto* child : expr->MutableChildren()) {
        if (HasUnmappedOuter(*child, local)) {
            return true;
        }
    }
    return false;
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

            // Correlation = an equality with one local and one outer column, found
            // anywhere in the predicate (deduplicated by the (outer, local) pair).
            std::vector<std::pair<std::string, std::string>> correlation; // (outer, local)
            {
                std::vector<std::pair<std::string, std::string>> found;
                CollectCorrelations(sel->Where, local, found);
                std::set<std::pair<std::string, std::string>> seen;
                for (auto& pair : found) {
                    if (seen.insert(pair).second) {
                        correlation.push_back(pair);
                    }
                }
            }

            if (!correlation.empty()) {
                int id = counter++;
                if (!sel->GroupBy) {
                    sel->GroupBy = std::make_shared<NSql::TSqlGroupBy>();
                }
                std::vector<NSql::TSqlPtr<NSql::TSqlSelectItem>> prepend;
                std::vector<TJoinKey> joinKeys;
                std::unordered_map<std::string, std::string> outerToLocal;
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
                    outerToLocal[outerCol] = localCol;
                }
                sel->SelectList->Items.insert(
                    sel->SelectList->Items.begin(), prepend.begin(), prepend.end());
                // Rewrite outer refs to their correlated local columns: a correlation
                // buried in a disjunction (`a = out AND P1) OR (a = out AND P2)`)
                // becomes fully local; the correlation itself is enforced by the
                // group-by + join keys above.
                RenameIdents(sel->Where, outerToLocal);
                if (HasUnmappedOuter(sel->Where, local)) {
                    return std::unexpected(
                        TError("unsupported correlated subquery (outer column outside an equality)"));
                }

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

    auto inputSchemaType = node->OutputColumns();
    auto inputSchema = NAst::TMaybeType<NAst::TStructType>(inputSchemaType);
    TAggCollector collector(inputSchema ? inputSchema.Cast().get() : nullptr);
    std::vector<TProjectionSpec> projections;
    for (size_t i = 0; i < select.SelectList->Items.size(); ++i) {
        const auto& item = select.SelectList->Items[i];
        if (item->Star) {
            auto* schema = static_cast<NAst::TStructType*>(node->OutputColumns().get());
            if (!schema) {
                return std::unexpected(TError("'*' over an unresolved input"));
            }
            std::string prefix;
            for (const auto& part : item->StarPrefix) {
                prefix += part + ".";
            }
            for (const auto& [colName, _] : schema->Fields) {
                if (!prefix.empty() && colName.rfind(prefix, 0) != 0) {
                    continue;
                }
                auto dot = colName.rfind('.');
                std::string bare = dot != std::string::npos ? colName.substr(dot + 1) : colName;
                projections.push_back({ .Name = bare, .Expression = Ident({}, colName) });
            }
            continue;
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

    std::vector<std::string> outputNames;
    outputNames.reserve(projections.size());
    for (const auto& p : projections) {
        outputNames.push_back(p.Name);
    }
    const bool distinct = select.Quantifier == NSql::ESetQuantifier::Distinct;

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
        for (const auto& projection : projections) {
            if (HasGroupingCall(projection.Expression)) {
                return std::unexpected(TError(projection.Expression->Location,
                    "GROUPING() requires GROUPING SETS, ROLLUP, or CUBE"));
            }
        }
        if (having) {
            return std::unexpected(TError("HAVING requires aggregation"));
        }
        TOperatorPtr projected =
            std::make_shared<TProjectOperator>(std::move(node), std::move(projections));
        return distinct ? ApplyDistinct(std::move(projected), outputNames) : projected;
    }

    std::vector<TGroupKey> groupKeys;
    std::vector<std::vector<size_t>> groupingSets;
    bool hasGroupingSyntax = false;
    if (select.GroupBy) {
        auto parsed = ParseGrouping(*select.GroupBy);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        groupKeys = std::move(parsed->Keys);
        groupingSets = std::move(parsed->Sets);
        hasGroupingSyntax = parsed->HasGroupingSyntax;
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

    auto groupingInfo = MakeGroupingRewriteInfo(groupKeys, groupingSets, hasGroupingSyntax);
    for (auto& projection : projections) {
        auto rewritten = RewriteGroupingCalls(projection.Expression, groupingInfo);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        projection.Expression = std::move(*rewritten);
    }
    if (having) {
        auto rewritten = RewriteGroupingCalls(having, groupingInfo);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        having = std::move(*rewritten);
    }

    std::vector<std::string> keys;
    keys.reserve(groupKeys.size());
    for (const auto& key : groupKeys) {
        keys.push_back(key.Name);
    }

    auto specs = collector.TakeSpecs();
    for (const auto& spec : specs) {
        if (HasGroupingCall(spec.Arg)) {
            return std::unexpected(TError(spec.Arg->Location,
                "GROUPING() inside aggregate arguments is not supported"));
        }
    }

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
    TOperatorPtr projected =
        std::make_shared<TProjectOperator>(std::move(node), std::move(projections));
    return distinct ? ApplyDistinct(std::move(projected), outputNames) : projected;
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
            if (HasGroupingCall(item->Expr)) {
                return std::unexpected(TError(item->Expr->Location,
                    "ORDER BY GROUPING() expression must appear in the select list"));
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
    if (setOp.Op == NSql::TSqlSetOp::EOp::Union) {
        // Flatten only a nested chain of the same UNION quantifier. Mixed chains such
        // as `(a UNION b) UNION ALL c` must keep the inner DISTINCT boundary.
        std::vector<TOperatorPtr> branches;
        std::function<std::expected<void, TError>(const NSql::TSqlNodePtr&)> collect =
            [&](const NSql::TSqlNodePtr& node) -> std::expected<void, TError> {
            auto inner = NSql::TMaybeNode<NSql::TSqlSetOp>(node);
            if (inner
                && inner.Cast()->Op == NSql::TSqlSetOp::EOp::Union
                && inner.Cast()->Quantifier == setOp.Quantifier)
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

        auto unionAll = std::make_shared<TUnionAllOperator>(std::move(branches));
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return unionAll;
        }

        auto outputNames = OutputColumnNames(unionAll, "UNION");
        if (!outputNames) {
            return std::unexpected(outputNames.error());
        }
        return ApplyDistinct(std::move(unionAll), *outputNames);
    }

    if (setOp.Op == NSql::TSqlSetOp::EOp::Intersect
        || setOp.Op == NSql::TSqlSetOp::EOp::Except)
    {
        const bool isIntersect = setOp.Op == NSql::TSqlSetOp::EOp::Intersect;
        const std::string opName = isIntersect ? "INTERSECT" : "EXCEPT";
        if (setOp.Quantifier == NSql::ESetQuantifier::All) {
            return std::unexpected(TError(opName + " ALL is not supported yet"));
        }

        auto left = BuildQueryBody(setOp.Left, sources);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = BuildQueryBody(setOp.Right, sources);
        if (!right) {
            return std::unexpected(right.error());
        }

        auto leftNames = OutputColumnNames(*left, opName + " left branch");
        if (!leftNames) {
            return std::unexpected(leftNames.error());
        }
        auto rightNames = OutputColumnNames(*right, opName + " right branch");
        if (!rightNames) {
            return std::unexpected(rightNames.error());
        }
        if (leftNames->size() != rightNames->size()) {
            return std::unexpected(TError(
                opName + " branches must have the same number of columns"));
        }

        auto leftDistinct = ApplyDistinct(std::move(*left), *leftNames);
        auto rightDistinct = ApplyDistinct(std::move(*right), *rightNames);

        std::vector<std::pair<std::string, std::string>> keys;
        keys.reserve(leftNames->size());
        for (size_t i = 0; i < leftNames->size(); ++i) {
            keys.emplace_back((*leftNames)[i], (*rightNames)[i]);
        }

        auto join = MakeJoin(
            std::move(leftDistinct), std::move(rightDistinct),
            std::move(keys),
            isIntersect ? EJoinType::LeftSemi : EJoinType::LeftAnti);
        if (!join) {
            return std::unexpected(join.error());
        }
        return *join;
    }

    return std::unexpected(TError("unsupported set operation"));
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

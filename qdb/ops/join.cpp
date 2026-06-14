#include <qdb/ops/join.h>

#include <qdb/pipeline/unbound_vars.h>
#include <qdb/types/nullable.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>
#include <unordered_set>

namespace NQqb {

using namespace NQumir::NAst;

namespace {

using TFields = std::vector<std::pair<std::string, TTypePtr>>;

TFields CopyFields(const TStructType* s) {
    TFields out;
    if (s) {
        out.assign(s->Fields.begin(), s->Fields.end());
    }
    return out;
}

TFields NullableFields(const TStructType* s) {
    TFields out;
    if (s) {
        for (const auto& [name, type] : s->Fields) {
            out.emplace_back(name,
                IsNullableType(type) ? type : std::make_shared<TNullable>(type));
        }
    }
    return out;
}

} // namespace

std::string_view JoinTypeName(EJoinType type) {
    switch (type) {
        case EJoinType::Inner:     return "inner";
        case EJoinType::Left:      return "left";
        case EJoinType::Right:     return "right";
        case EJoinType::Full:      return "full";
        case EJoinType::LeftSemi:  return "left_semi";
        case EJoinType::RightSemi: return "right_semi";
        case EJoinType::LeftAnti:  return "left_anti";
        case EJoinType::RightAnti: return "right_anti";
    }
    return "inner";
}

std::optional<EJoinType> ParseJoinType(std::string_view name) {
    if (name == "inner")      return EJoinType::Inner;
    if (name == "left")       return EJoinType::Left;
    if (name == "right")      return EJoinType::Right;
    if (name == "full")       return EJoinType::Full;
    if (name == "left_semi")  return EJoinType::LeftSemi;
    if (name == "right_semi") return EJoinType::RightSemi;
    if (name == "left_anti")  return EJoinType::LeftAnti;
    if (name == "right_anti") return EJoinType::RightAnti;
    return std::nullopt;
}

std::expected<TTypePtr, NQumir::TError> ComputeJoinOutputType(
    const TTypePtr& left, const TTypePtr& right, EJoinType type)
{
    auto* leftStruct = static_cast<TStructType*>(left.get());
    auto* rightStruct = static_cast<TStructType*>(right.get());

    // SEMI / ANTI keep only the preserved side's columns (nullability intact).
    switch (type) {
        case EJoinType::LeftSemi:
        case EJoinType::LeftAnti:
            return std::make_shared<TStructType>(CopyFields(leftStruct));
        case EJoinType::RightSemi:
        case EJoinType::RightAnti:
            return std::make_shared<TStructType>(CopyFields(rightStruct));
        default:
            break;
    }

    // INNER/OUTER: both sides contribute. NULL-padded side becomes nullable.
    TFields leftFields = (type == EJoinType::Right || type == EJoinType::Full)
        ? NullableFields(leftStruct)
        : CopyFields(leftStruct);
    TFields rightFields = (type == EJoinType::Left || type == EJoinType::Full)
        ? NullableFields(rightStruct)
        : CopyFields(rightStruct);

    std::unordered_set<std::string> seen;
    TFields out;
    out.reserve(leftFields.size() + rightFields.size());
    for (auto& field : leftFields) {
        seen.insert(field.first);
        out.push_back(std::move(field));
    }
    for (auto& field : rightFields) {
        if (seen.contains(field.first)) {
            return std::unexpected(NQumir::TError(
                "join output column name '" + field.first +
                "' appears on both sides; aliasing is not supported"));
        }
        out.push_back(std::move(field));
    }
    return std::make_shared<TStructType>(std::move(out));
}

TJoinOperator::TJoinOperator(TOperatorPtr left, TOperatorPtr right,
    std::vector<TJoinKey> keys, EJoinType type, TExprPtr filter)
    : Left_(std::move(left))
    , Right_(std::move(right))
    , Keys_(std::move(keys))
    , Type_(type)
    , Filter_(std::move(filter))
{
    auto leftSchema = Left_->OutputColumns();
    auto rightSchema = Right_->OutputColumns();
    auto output = ComputeJoinOutputType(leftSchema, rightSchema, Type_);
    // Callers go through MakeJoin, which validates before construction; on the
    // unvalidated path leave the output type null rather than throwing.
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{leftSchema, rightSchema},
        output ? *output : nullptr);
}

std::unordered_set<std::string> TJoinOperator::ComputeReferencedColumns() const {
    std::unordered_set<std::string> refs;
    // Both sides' key columns are referenced regardless of which columns the
    // join emits (matching always needs both keys).
    for (const auto& key : Keys_) {
        refs.insert(key.Left);
        refs.insert(key.Right);
    }
    if (Filter_) {
        for (auto& col : FindUnboundVars(Filter_)) {
            refs.insert(col);
        }
    }
    return refs;
}

const std::string TJoinOperator::ToString() const {
    using namespace NQumir::NAst::NCore;
    std::string s = "(rel join " + Left_->ToString() + " " + Right_->ToString();
    for (const auto& key : Keys_) {
        s += " (on " + key.Left + " " + key.Right + ")";
    }
    s += " (type " + std::string(JoinTypeName(Type_)) + ")";
    if (Filter_) {
        s += " (filter " + PrintAst(Filter_) + ")";
    }
    return s + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeJoin(TOperatorPtr left, TOperatorPtr right,
    std::vector<std::pair<std::string, std::string>> keys,
    EJoinType type, const std::string& filter)
{
    if (keys.empty()) {
        return std::unexpected(NQumir::TError("join requires at least one key pair"));
    }
    std::vector<TJoinKey> joinKeys;
    joinKeys.reserve(keys.size());
    for (auto& [l, r] : keys) {
        joinKeys.push_back({std::move(l), std::move(r)});
    }

    TExprPtr filterExpr;
    if (!filter.empty()) {
        std::istringstream ss(filter);
        NQumir::NAst::NCore::TTokenStream tokens(ss);
        NQumir::NAst::NCore::TParser parser;
        auto parsed = parser.Parse(tokens);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        filterExpr = std::move(*parsed);
    }

    // Validate the output schema (duplicate column names) before constructing.
    auto output = ComputeJoinOutputType(
        left->OutputColumns(), right->OutputColumns(), type);
    if (!output) {
        return std::unexpected(output.error());
    }

    return std::make_shared<TJoinOperator>(std::move(left), std::move(right),
        std::move(joinKeys), type, std::move(filterExpr));
}

} // namespace NQqb

#include <qdb/io/parquet/row_group_predicate.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

constexpr const char* EvalFn = "__row_group_predicate__";

TVmFrontendContext& RowGroupVmContext() {
    static TVmFrontendContext context({
        std::string(QDB_SOURCE_DIR) + "/modules/row_group_interval.oz",
    });
    return context;
}

enum class ENumberKind {
    Invalid,
    Signed,
    Unsigned,
    Float,
};

struct TNumberInfo {
    ENumberKind Kind = ENumberKind::Invalid;
    int BitWidth = 0;

    explicit operator bool() const {
        return Kind != ENumberKind::Invalid;
    }
};

TNumberInfo NumberInfo(const TTypePtr& type) {
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return {
            .Kind = integer.Cast()->IsSigned()
                ? ENumberKind::Signed
                : ENumberKind::Unsigned,
            .BitWidth = integer.Cast()->BitWidth(),
        };
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return {.Kind = ENumberKind::Float, .BitWidth = 64};
    }
    return {};
}

TTypePtr PrimitiveType(ENumberKind kind) {
    switch (kind) {
        case ENumberKind::Signed:
            return std::make_shared<TIntegerType>(TIntegerType::I64);
        case ENumberKind::Unsigned:
            return std::make_shared<TIntegerType>(TIntegerType::U64);
        case ENumberKind::Float:
            return std::make_shared<TFloatType>();
        case ENumberKind::Invalid:
            return nullptr;
    }
    return nullptr;
}

TExprPtr Ident(std::string name) {
    return std::make_shared<TIdentExpr>(NQumir::TLocation{}, std::move(name));
}

TExprPtr Call(std::string name, std::vector<TExprPtr> args = {}) {
    return std::make_shared<TCallExpr>(
        NQumir::TLocation{}, Ident(std::move(name)), std::move(args));
}

TExprPtr I64(int64_t value) {
    return std::make_shared<TNumberExpr>(NQumir::TLocation{}, value);
}

TExprPtr U64(uint64_t value) {
    auto out = std::make_shared<TNumberExpr>(
        NQumir::TLocation{}, std::bit_cast<int64_t>(value));
    out->Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    return out;
}

TExprPtr F64(double value) {
    return std::make_shared<TNumberExpr>(NQumir::TLocation{}, value);
}

std::string ParamName(size_t column, std::string_view suffix) {
    return "__rg_" + std::to_string(column) + "_" + std::string(suffix);
}

struct TLoweredNumber {
    TExprPtr Expr;
    TNumberInfo Info;

    explicit operator bool() const {
        return Expr != nullptr && static_cast<bool>(Info);
    }
};

class TPredicateLowerer {
public:
    TPredicateLowerer(const TSchema& schema, std::string_view sourceAlias)
        : Schema_(schema)
        , SourceAlias_(sourceAlias)
    {
        for (size_t i = 0; i < schema.Columns.size(); ++i) {
            const std::string bare(schema.Columns[i].Name);
            BareColumns_.emplace(bare, i);
            if (!SourceAlias_.empty()) {
                QualifiedColumns_.emplace(SourceAlias_ + "." + bare, i);
            }
        }
    }

    TExprPtr LowerTruth(const TExprPtr& expr) {
        if (!expr) {
            return UnknownTruth();
        }
        if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
            const auto op = binary.Cast()->Operator.ToString();
            if (op == "&&" || op == "||") {
                return Call(op == "&&" ? "rg_truth_and" : "rg_truth_or", {
                    LowerTruth(binary.Cast()->Left),
                    LowerTruth(binary.Cast()->Right),
                });
            }
            if (op == "==" || op == "!=" || op == "<" || op == "<=" ||
                op == ">" || op == ">=")
            {
                return LowerComparison(op, binary.Cast()->Left, binary.Cast()->Right);
            }
            return UnknownTruth();
        }
        if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
            if (unary.Cast()->Operator == "!") {
                return Call("rg_truth_not", {LowerTruth(unary.Cast()->Operand)});
            }
            return UnknownTruth();
        }
        if (auto number = TMaybeNode<TNumberExpr>(expr)) {
            auto boolType = TMaybeType<TBoolType>(UnwrapNullableType(number.Cast()->Type));
            if (boolType) {
                return Call(number.Cast()->IntValue
                    ? "rg_truth_true"
                    : "rg_truth_false");
            }
            return UnknownTruth();
        }
        if (auto call = TMaybeNode<TCallExpr>(expr)) {
            auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
            if (!callee) {
                return UnknownTruth();
            }
            const auto& name = callee.Cast()->Name;
            if (name == "qdb_is_true" && call.Cast()->Args.size() == 1) {
                return Call("rg_truth_is_true", {LowerTruth(call.Cast()->Args.front())});
            }
            if (name == "qdb_is_null" && call.Cast()->Args.size() == 1) {
                auto value = LowerNumber(call.Cast()->Args.front(), std::nullopt);
                return value
                    ? Call("rg_is_null", {std::move(value.Expr)})
                    : UnknownTruth();
            }
        }
        return UnknownTruth();
    }

    std::vector<TParam> Params() const {
        std::vector<TParam> out;
        for (size_t column = 0; column < Schema_.Columns.size(); ++column) {
            auto info = NumberInfo(Schema_.Columns[column].Type);
            if (!info) {
                continue;
            }
            auto type = PrimitiveType(info.Kind);
            out.push_back(std::make_shared<TVarStmt>(
                NQumir::TLocation{}, ParamName(column, "lo"), type));
            out.push_back(std::make_shared<TVarStmt>(
                NQumir::TLocation{}, ParamName(column, "hi"), type));
            out.push_back(std::make_shared<TVarStmt>(
                NQumir::TLocation{}, ParamName(column, "flags"),
                std::make_shared<TIntegerType>(TIntegerType::I64)));
        }
        return out;
    }

    std::vector<size_t> ParamColumns() const {
        std::vector<size_t> out;
        for (size_t column = 0; column < Schema_.Columns.size(); ++column) {
            if (NumberInfo(Schema_.Columns[column].Type)) {
                out.push_back(column);
            }
        }
        return out;
    }

private:
    TExprPtr UnknownTruth() const {
        return Call("rg_truth_unknown");
    }

    std::optional<size_t> ResolveColumn(const std::string& name) const {
        if (auto it = QualifiedColumns_.find(name); it != QualifiedColumns_.end()) {
            return it->second;
        }
        if (auto it = BareColumns_.find(name); it != BareColumns_.end()) {
            return it->second;
        }
        // Qualified identifiers from an already-annotated source are safe to
        // resolve by their final component because this evaluator is attached
        // only to a predicate directly above one source.
        if (auto dot = name.rfind('.'); dot != std::string::npos) {
            if (auto it = BareColumns_.find(name.substr(dot + 1));
                it != BareColumns_.end())
            {
                return it->second;
            }
        }
        return std::nullopt;
    }

    TLoweredNumber Convert(TLoweredNumber value, TNumberInfo target) {
        if (!value || !target) {
            return {};
        }
        if (value.Info.Kind == target.Kind) {
            value.Info = target;
            return value;
        }
        if (target.Kind == ENumberKind::Float) {
            if (value.Info.Kind == ENumberKind::Signed) {
                return {
                    .Expr = Call("rg_f64_from_i64", {std::move(value.Expr)}),
                    .Info = target,
                };
            }
            if (value.Info.Kind == ENumberKind::Unsigned) {
                return {
                    .Expr = Call("rg_f64_from_u64", {std::move(value.Expr)}),
                    .Info = target,
                };
            }
        }
        return {};
    }

    TLoweredNumber LowerLiteral(
        const std::shared_ptr<TNumberExpr>& number,
        std::optional<TNumberInfo> expected)
    {
        TNumberInfo target = expected.value_or(NumberInfo(number->Type));
        if (!target) {
            return {};
        }
        TExprPtr literal;
        switch (target.Kind) {
            case ENumberKind::Float:
                literal = F64(number->IsFloat()
                    ? number->FloatValue
                    : static_cast<double>(number->IntValue));
                break;
            case ENumberKind::Signed:
                if (number->IsFloat()) {
                    return {};
                }
                literal = I64(number->IntValue);
                break;
            case ENumberKind::Unsigned:
                if (number->IsFloat() || number->IntValue < 0) {
                    return {};
                }
                literal = U64(static_cast<uint64_t>(number->IntValue));
                break;
            case ENumberKind::Invalid:
                return {};
        }
        return {
            .Expr = Call("rg_const", {std::move(literal)}),
            .Info = target,
        };
    }

    TLoweredNumber LowerIdentifier(
        const std::shared_ptr<TIdentExpr>& ident,
        std::optional<TNumberInfo> expected)
    {
        auto column = ResolveColumn(ident->Name);
        if (!column) {
            return {};
        }
        auto info = NumberInfo(Schema_.Columns[*column].Type);
        if (!info) {
            return {};
        }
        auto value = TLoweredNumber{
            .Expr = Call("rg_range", {
                Ident(ParamName(*column, "lo")),
                Ident(ParamName(*column, "hi")),
                Ident(ParamName(*column, "flags")),
            }),
            .Info = info,
        };
        return expected ? Convert(std::move(value), *expected) : value;
    }

    static std::pair<int64_t, int64_t> SignedLimits(int bits) {
        if (bits >= 64) {
            return {
                std::numeric_limits<int64_t>::min(),
                std::numeric_limits<int64_t>::max(),
            };
        }
        const int64_t high = (int64_t{1} << (bits - 1)) - 1;
        return {-high - 1, high};
    }

    static uint64_t UnsignedHigh(int bits) {
        return bits >= 64
            ? std::numeric_limits<uint64_t>::max()
            : (uint64_t{1} << bits) - 1;
    }

    TNumberInfo ExpressionNumberInfo(const TExprPtr& expr) const {
        if (!expr) {
            return {};
        }
        if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
            if (auto column = ResolveColumn(ident.Cast()->Name)) {
                return NumberInfo(Schema_.Columns[*column].Type);
            }
            return {};
        }
        return NumberInfo(expr->Type);
    }

    TLoweredNumber LowerArithmetic(
        const std::string& op,
        const TExprPtr& leftExpr,
        const TExprPtr& rightExpr,
        TNumberInfo native,
        std::optional<TNumberInfo> expected)
    {
        if (!native) {
            return {};
        }
        auto left = LowerNumber(leftExpr, native);
        auto right = LowerNumber(rightExpr, native);
        if (!left || !right) {
            return {};
        }
        TExprPtr result;
        if (native.Kind == ENumberKind::Signed) {
            auto [low, high] = SignedLimits(native.BitWidth);
            const std::string fn = op == "+" ? "rg_i64_add"
                : op == "-" ? "rg_i64_sub"
                : "rg_i64_mul";
            result = Call(fn, {
                std::move(left.Expr), std::move(right.Expr), I64(low), I64(high),
            });
        } else if (native.Kind == ENumberKind::Unsigned) {
            const std::string fn = op == "+" ? "rg_u64_add"
                : op == "-" ? "rg_u64_sub"
                : "rg_u64_mul";
            std::vector<TExprPtr> args{
                std::move(left.Expr), std::move(right.Expr),
            };
            if (op != "-") {
                args.push_back(U64(UnsignedHigh(native.BitWidth)));
            }
            result = Call(fn, std::move(args));
        } else if (native.Kind == ENumberKind::Float) {
            const std::string fn = op == "+" ? "rg_f64_add"
                : op == "-" ? "rg_f64_sub"
                : "rg_f64_mul";
            result = Call(fn, {std::move(left.Expr), std::move(right.Expr)});
        } else {
            return {};
        }
        TLoweredNumber value{.Expr = std::move(result), .Info = native};
        return expected ? Convert(std::move(value), *expected) : value;
    }

    TLoweredNumber LowerNumber(
        const TExprPtr& expr,
        std::optional<TNumberInfo> expected)
    {
        if (!expr) {
            return {};
        }
        if (auto number = TMaybeNode<TNumberExpr>(expr)) {
            return LowerLiteral(number.Cast(), expected);
        }
        if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
            return LowerIdentifier(ident.Cast(), expected);
        }
        if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
            const auto op = binary.Cast()->Operator.ToString();
            if (op != "+" && op != "-" && op != "*") {
                return {};
            }
            auto native = ExpressionNumberInfo(expr);
            if (!native && expected) {
                native = *expected;
            }
            return LowerArithmetic(
                op, binary.Cast()->Left, binary.Cast()->Right, native, expected);
        }
        if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
            if (unary.Cast()->Operator != "-") {
                return {};
            }
            auto native = ExpressionNumberInfo(expr);
            if (!native && expected) {
                native = *expected;
            }
            auto operand = LowerNumber(unary.Cast()->Operand, native);
            if (!operand) {
                return {};
            }
            TExprPtr result;
            if (native.Kind == ENumberKind::Signed) {
                auto [low, high] = SignedLimits(native.BitWidth);
                result = Call("rg_i64_neg", {
                    std::move(operand.Expr), I64(low), I64(high),
                });
            } else if (native.Kind == ENumberKind::Float) {
                result = Call("rg_f64_neg", {std::move(operand.Expr)});
            } else {
                return {};
            }
            TLoweredNumber value{.Expr = std::move(result), .Info = native};
            return expected ? Convert(std::move(value), *expected) : value;
        }
        // Casts and scalar calls intentionally remain unknown in v1.
        return {};
    }

    TNumberInfo ComparisonKind(const TExprPtr& left, const TExprPtr& right) const {
        return NumberInfo(NKernel::EffectiveBinaryNumericType(left, right));
    }

    TExprPtr LowerComparison(
        const std::string& op,
        const TExprPtr& leftExpr,
        const TExprPtr& rightExpr)
    {
        auto kind = ComparisonKind(leftExpr, rightExpr);
        if (!kind) {
            return UnknownTruth();
        }
        auto left = LowerNumber(leftExpr, kind);
        auto right = LowerNumber(rightExpr, kind);
        if (!left || !right) {
            return UnknownTruth();
        }
        const std::string fn = op == "==" ? "rg_cmp_eq"
            : op == "!=" ? "rg_cmp_ne"
            : op == "<" ? "rg_cmp_lt"
            : op == "<=" ? "rg_cmp_le"
            : op == ">" ? "rg_cmp_gt"
            : "rg_cmp_ge";
        return Call(fn, {std::move(left.Expr), std::move(right.Expr)});
    }

private:
    const TSchema& Schema_;
    std::string SourceAlias_;
    std::unordered_map<std::string, size_t> BareColumns_;
    std::unordered_map<std::string, size_t> QualifiedColumns_;
};

} // namespace

struct TRowGroupPredicateEvaluator::TImpl {
    std::unique_ptr<TVmFunction> Function;
    std::vector<size_t> ParamColumns;
};

TRowGroupPredicateEvaluator::TRowGroupPredicateEvaluator(std::unique_ptr<TImpl> impl)
    : Impl_(std::move(impl))
{}

TRowGroupPredicateEvaluator::~TRowGroupPredicateEvaluator() = default;

std::unique_ptr<TRowGroupPredicateEvaluator> TRowGroupPredicateEvaluator::Compile(
    const TExprPtr& predicate,
    const TSchema& schema,
    std::string_view sourceAlias,
    std::ostream* diagnostics)
{
    try {
        auto diagnostic = [&](const std::string& message) {
            if (diagnostics) {
                *diagnostics << "row-group predicate: " << message << '\n';
            }
        };
        TPredicateLowerer lowerer(schema, sourceAlias);
        auto typedPredicate = CloneExpr(predicate);
        std::vector<std::pair<std::string, TTypePtr>> fields;
        fields.reserve(schema.Columns.size() * (sourceAlias.empty() ? 1 : 2));
        for (const auto& column : schema.Columns) {
            fields.emplace_back(std::string(column.Name), column.Type);
            if (!sourceAlias.empty()) {
                fields.emplace_back(
                    std::string(sourceAlias) + "." + std::string(column.Name),
                    column.Type);
            }
        }
        NKernel::AnnotateExprTreeTypes(
            typedPredicate, TStructType(std::move(fields)));
        auto truth = lowerer.LowerTruth(typedPredicate);
        auto params = lowerer.Params();

        NQumir::TLocation loc{};
        auto body = std::make_shared<TBlockExpr>(
            loc,
            std::vector<TExprPtr>{std::make_shared<TReturnExpr>(loc, std::move(truth))});
        if (diagnostics) {
            diagnostic("compiled body " + NQumir::NAst::NCore::PrintAst(body));
        }

        auto compiled = TVmFunction::Compile(RowGroupVmContext(), {
            .NamePrefix = EvalFn,
            .Params = std::move(params),
            .Body = std::move(body),
            .ReturnType =
                std::make_shared<TIntegerType>(TIntegerType::I64),
            .Imports = {"row_group_interval"},
        });
        if (!compiled) {
            diagnostic(compiled.error());
            return nullptr;
        }

        auto impl = std::make_unique<TImpl>();
        impl->Function = std::move(*compiled);
        impl->ParamColumns = lowerer.ParamColumns();
        return std::unique_ptr<TRowGroupPredicateEvaluator>(
            new TRowGroupPredicateEvaluator(std::move(impl)));
    } catch (const std::exception& error) {
        if (diagnostics) {
            *diagnostics << "row-group predicate: " << error.what() << '\n';
        }
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

bool TRowGroupPredicateEvaluator::MayBeTrue(
    std::span<const TRowGroupColumnRange> columns) const noexcept
{
    if (!Impl_ || !Impl_->Function) {
        return true;
    }
    try {
        std::vector<int64_t> args;
        args.reserve(Impl_->ParamColumns.size() * 3);
        for (size_t column : Impl_->ParamColumns) {
            if (column >= columns.size()) {
                return true;
            }
            const auto& range = columns[column];
            args.push_back(std::bit_cast<int64_t>(range.MinValue));
            args.push_back(std::bit_cast<int64_t>(range.MaxValue));
            int64_t flags = 0;
            if (range.HasBounds) flags |= 1;
            if (range.MayBeNull) flags |= 2;
            if (range.MayBeValue) flags |= 4;
            if (range.MayBeNaN) flags |= 8;
            args.push_back(flags);
        }
        auto result = Impl_->Function->EvalRaw(std::move(args));
        if (!result) {
            return true;
        }
        return (*result & 2) != 0;
    } catch (...) {
        return true;
    }
}

} // namespace NQdb

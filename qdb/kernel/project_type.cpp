#include <qdb/kernel/project_type.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <string>
#include <unordered_map>

namespace NQdb::NKernel {

using namespace NQumir::NAst;

namespace {

bool IsFloat(const TTypePtr& type) {
    return static_cast<bool>(TMaybeType<TFloatType>(UnwrapNamedType(type)));
}

bool IsInteger(const TTypePtr& type) {
    return static_cast<bool>(TMaybeType<TIntegerType>(UnwrapNamedType(type)));
}

bool IsComparison(const TOperator& op) {
    return op == "<" || op == ">" || op == "<=" || op == ">=" ||
           op == "==" || op == "!=";
}

bool IsLogical(const TOperator& op) {
    return op == "&&" || op == "||";
}

const std::unordered_map<std::string, TTypePtr>& ExternalFuncReturnTypes() {
    static const auto map = []() {
        std::unordered_map<std::string, TTypePtr> m;
        auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
        auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
        auto string = std::make_shared<TStringType>();
        m.emplace("qdb_string_view_sql_like", i64);
        m.emplace("qdb_string_view_cmp_cstr", i64);
        m.emplace("qdb_cstr_cmp_cstr", i64);
        m.emplace("qdb_substring", string);
        m.emplace("qdb_sql_bool_and", i64);
        m.emplace("qdb_sql_bool_or", i64);
        m.emplace("qdb_sql_bool_not", i64);
        m.emplace("qdb_date_year", i32);
        m.emplace("qdb_sql_date", i32);
        m.emplace("qdb_sql_interval", i32);
        return m;
    }();
    return map;
}

} // namespace

TTypePtr InferProjectExprType(const TExprPtr& expr, const TStructType& inputType) {
    if (!expr) {
        throw NQumir::TError("project type inference: null expression");
    }

    // An explicit type annotation/cast ((: expr T) or (cast expr T)) sets
    // expr->Type and overrides inference.
    if (expr->Type) {
        return UnwrapNamedType(UnwrapNullableType(expr->Type));
    }

    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        const std::string& name = ident.Cast()->Name;
        for (const auto& [fieldName, fieldType] : inputType.Fields) {
            if (fieldName == name) {
                return UnwrapNullableType(fieldType); // Stage 1: drop nullability
            }
        }
        throw NQumir::TError("project type inference: unknown column '" + name + "'");
    }

    if (auto number = TMaybeNode<TNumberExpr>(expr)) {
        return number.Cast()->IsFloat()
            ? std::static_pointer_cast<TType>(std::make_shared<TFloatType>())
            : std::static_pointer_cast<TType>(
                  std::make_shared<TIntegerType>(TIntegerType::I64));
    }

    if (TMaybeNode<TStringLiteralExpr>(expr)) {
        return std::make_shared<TStringType>();
    }

    if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
        if (unary.Cast()->Operator == "!") {
            return std::make_shared<TBoolType>();
        }
        return InferProjectExprType(unary.Cast()->Operand, inputType);
    }

    if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
        const auto op = binary.Cast()->Operator;
        if (IsComparison(op) || IsLogical(op)) {
            return std::make_shared<TBoolType>();
        }
        // Arithmetic: conservative numeric promotion.
        auto left = InferProjectExprType(binary.Cast()->Left, inputType);
        auto right = InferProjectExprType(binary.Cast()->Right, inputType);
        if (IsFloat(left) || IsFloat(right)) {
            return std::make_shared<TFloatType>();
        }
        if (IsInteger(left) && IsInteger(right)) {
            return std::make_shared<TIntegerType>(TIntegerType::I64);
        }
        throw NQumir::TError(
            "project type inference: unsupported operand types for '" +
            op.ToString() + "'");
    }

    // (if cond then else) — infer from the then-branch.
    if (auto ifExpr = TMaybeNode<TIfExpr>(expr)) {
        return InferProjectExprType(ifExpr.Cast()->Then, inputType);
    }

    // (call fn arg...) — look up function return type from the module registry.
    if (auto callExpr = TMaybeNode<TCallExpr>(expr)) {
        if (auto callee = TMaybeNode<TIdentExpr>(callExpr.Cast()->Callee)) {
            const auto& retTypes = ExternalFuncReturnTypes();
            auto it = retTypes.find(callee.Cast()->Name);
            if (it != retTypes.end()) {
                // Named("StringView") → TStringType so the output column is
                // treated as a variable-length string column by the planner.
                if (auto named = TMaybeType<TNamedType>(it->second)) {
                    if (named.Cast()->Name == "StringView") {
                        return std::make_shared<TStringType>();
                    }
                }
                return UnwrapNamedType(UnwrapNullableType(it->second));
            }
        }
    }

    // (block stmt... expr) — infer from the last statement.
    if (auto blockExpr = TMaybeNode<TBlockExpr>(expr)) {
        const auto& stmts = blockExpr.Cast()->Stmts;
        if (!stmts.empty()) {
            return InferProjectExprType(stmts.back(), inputType);
        }
    }

    throw NQumir::TError(
        "project type inference: unsupported expression '" +
        std::string(expr->NodeName()) + "'");
}

} // namespace NQdb::NKernel

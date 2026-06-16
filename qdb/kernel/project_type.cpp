#include <qdb/kernel/project_type.h>

#include <qdb/types/nullable.h>

#include <qumir/error.h>

#include <string>

namespace NQqb::NKernel {

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

    throw NQumir::TError(
        "project type inference: unsupported expression '" +
        std::string(expr->NodeName()) + "'");
}

} // namespace NQqb::NKernel

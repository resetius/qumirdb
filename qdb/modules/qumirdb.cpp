#include "qumirdb.h"

#include <qumir/parser/ast.h>

namespace NQumir {
namespace NRegistry {

namespace {

using namespace NAst;

TExprPtr ident(const std::string& name) {
    return std::make_shared<TIdentExpr>(TLocation{}, name);
}

TExprPtr number(TTypePtr type, int64_t value) {
    auto expr = std::make_shared<TNumberExpr>(TLocation{}, value);
    expr->Type = std::move(type);
    return expr;
}

TExprPtr binary(const char* op, TExprPtr left, TExprPtr right, TTypePtr type) {
    auto expr = std::make_shared<TBinaryExpr>(left->Location, TOperator(op), std::move(left), std::move(right));
    expr->Type = std::move(type);
    return expr;
}

TExprPtr cast(TExprPtr expr, TTypePtr type) {
    return std::make_shared<TCastExpr>(expr->Location, std::move(expr), std::move(type));
}

} // namespace

QumirDbModule::QumirDbModule() {
    auto boolType = std::make_shared<TBoolType>();
    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);

    ExternalFunctions_ = {
        {
            .Name = "bitoff",
            .MangledName = "qdb_bitoff",
            .ArgTypes = { ptrU8Type, i64Type, i64Type },
            .ReturnType = boolType,
            .Inline = [boolType, i64Type](std::vector<TExprPtr> args) -> TExprPtr {
                std::vector<TLetExpr::TBinding> bindings;
                bindings.push_back({ .Name = "$$bitmap", .Value = args[0] });
                bindings.push_back({ .Name = "$$index", .Value = args[1] });
                bindings.push_back({ .Name = "$$bitoff", .Value = args[2] });

                auto bitIndex = binary("+", ident("$$index"), ident("$$bitoff"), i64Type);
                auto byteIndex = binary(">>", bitIndex, number(i64Type, 3), i64Type);
                auto bitPos = binary("&", bitIndex, number(i64Type, 7), i64Type);
                auto byte = std::make_shared<TIndexExpr>(TLocation{}, ident("$$bitmap"), byteIndex);
                auto byteAsI64 = cast(byte, i64Type);
                auto shifted = binary(">>", byteAsI64, bitPos, i64Type);
                auto masked = binary("&", shifted, number(i64Type, 1), i64Type);
                auto body = binary("!=", masked, number(i64Type, 0), boolType);
                return std::make_shared<TLetExpr>(TLocation{}, std::move(bindings), std::move(body));
            },
        },
    };
}

} // namespace NRegistry
} // namespace NQumir

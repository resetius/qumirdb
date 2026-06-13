#include <qdb/kernel/column_value.h>

#include <qumir/location.h>
#include <qumir/parser/operator.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace NQqb::NKernel {

TColumnValueAst BuildColumnValueAst(
    const std::string& columnName,
    const std::string& rowName,
    const std::string& temporaryPrefix,
    const NQumir::NAst::TTypePtr& logicalType,
    const NQumir::NAst::TTypePtr& stringViewType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type = nullptr) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = type ? std::move(type) : i64Type;
        return result;
    };
    auto field = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(columnName), name);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto cast = [&](TExprPtr value, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(value), std::move(type));
    };
    auto index = [&](TExprPtr collection, TExprPtr offset) -> TExprPtr {
        return std::make_shared<TIndexExpr>(
            loc, std::move(collection), std::move(offset));
    };

    auto maskIsNull = binary("==", cast(field("Mask"), i64Type), number(0));
    auto maskBit = std::make_shared<TCallExpr>(loc, ident("bitoff"),
        std::vector<TExprPtr>{field("Mask"), ident(rowName), field("MaskBitOffset")});
    auto isValid = binary("||", std::move(maskIsNull), std::move(maskBit));

    auto type = UnwrapNamedType(logicalType);
    if (TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type)) {
        auto pointerType = std::make_shared<TPointerType>(logicalType);
        const std::string dataName = temporaryPrefix + "_data";
        std::vector<TExprPtr> setup = {
            std::make_shared<TVarStmt>(loc, dataName, pointerType),
            std::make_shared<TAssignExpr>(loc, dataName,
                cast(cast(field("Data"), i64Type), pointerType)),
        };
        return {
            .Setup = std::move(setup),
            .Value = index(ident(dataName), ident(rowName)),
            .IsValid = std::move(isValid),
            .ValueType = logicalType,
        };
    }
    if (TMaybeType<TBoolType>(type)) {
        auto data = cast(cast(field("Data"), i64Type), ptrU8Type);
        auto value = std::make_shared<TCallExpr>(loc, ident("bitoff"),
            std::vector<TExprPtr>{
                std::move(data), ident(rowName), field("DataBitOffset")});
        return {
            .Setup = {},
            .Value = std::move(value),
            .IsValid = std::move(isValid),
            .ValueType = logicalType,
        };
    }
    if (TMaybeType<TStringType>(type)) {
        if (!stringViewType) {
            throw std::invalid_argument("BuildColumnValueAst: missing StringView type");
        }
        auto namedStringView = std::make_shared<TNamedType>(
            "StringView", stringViewType);
        const std::string offsets32Name = temporaryPrefix + "_offsets32";
        const std::string offsets64Name = temporaryPrefix + "_offsets64";
        const std::string startName = temporaryPrefix + "_start";
        const std::string endName = temporaryPrefix + "_end";
        auto ptrI32Type = std::make_shared<TPointerType>(i32Type);
        auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
        std::vector<TExprPtr> setup = {
            std::make_shared<TVarStmt>(loc, offsets32Name, ptrI32Type),
            std::make_shared<TAssignExpr>(loc, offsets32Name,
                cast(cast(field("Offsets"), i64Type), ptrI32Type)),
            std::make_shared<TVarStmt>(loc, offsets64Name, ptrI64Type),
            std::make_shared<TAssignExpr>(loc, offsets64Name,
                cast(cast(field("Offsets"), i64Type), ptrI64Type)),
            std::make_shared<TVarStmt>(loc, startName, i64Type),
            std::make_shared<TVarStmt>(loc, endName, i64Type),
        };
        auto buildOffsetsBranch = [&](const std::string& offsetsName) -> TExprPtr {
            auto start = binary("-",
                cast(index(ident(offsetsName), ident(rowName)), i64Type),
                cast(index(ident(offsetsName), number(0)), i64Type));
            auto end = binary("-",
                cast(index(ident(offsetsName),
                    binary("+", ident(rowName), number(1))), i64Type),
                cast(index(ident(offsetsName), number(0)), i64Type));
            return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
                std::make_shared<TAssignExpr>(loc, startName, std::move(start)),
                std::make_shared<TAssignExpr>(loc, endName, std::move(end)),
            });
        };
        setup.push_back(std::make_shared<TIfExpr>(loc,
            binary("==", field("OffsetWidth"), number(4, u8Type)),
            buildOffsetsBranch(offsets32Name), buildOffsetsBranch(offsets64Name)));
        auto dataAddress = binary("+", cast(field("Data"), i64Type),
            ident(startName));
        auto data = cast(std::move(dataAddress), ptrU8Type);
        auto size = binary("-", ident(endName), ident(startName));
        auto value = std::make_shared<TStructConstructExpr>(loc, namedStringView,
            std::vector<TExprPtr>{std::move(data), std::move(size)});
        return {
            .Setup = std::move(setup),
            .Value = std::move(value),
            .IsValid = std::move(isValid),
            .ValueType = std::move(namedStringView),
        };
    }

    throw std::invalid_argument(
        "BuildColumnValueAst: unsupported logical type " +
        (logicalType ? logicalType->ToString() : std::string("<null>")));
}

} // namespace NQqb::NKernel

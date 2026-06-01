#include "qumirdb.h"

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>

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
    auto i8Type = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto voidPtrType = std::make_shared<TPointerType>(i64Type);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI8Type = std::make_shared<TPointerType>(i8Type);

    // TColumn C layout (all fields at 8-byte boundaries due to pointer padding):
    // offset  0: Data          char*      <ptr i8>
    // offset  8: DataBitOffset int32_t    i32  (+4 pad)
    // offset 16: Mask          uint8_t*   <ptr u8>
    // offset 24: MaskBitOffset int32_t    i32  (+4 pad)
    // offset 32: Offsets       void*      <ptr i64>
    // offset 40: OffsetWidth   uint8_t    u8   (+7 pad)
    // sizeof = 48
    auto columnType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Data", ptrI8Type},
            {"DataBitOffset", i32Type},
            {"Mask", ptrU8Type},
            {"MaskBitOffset", i32Type},
            {"Offsets", voidPtrType},
            {"OffsetWidth", u8Type},
        });
    auto columnNamedType = std::make_shared<TNamedType>("TColumn", columnType);
    auto ptrColumnType = std::make_shared<TPointerType>(columnNamedType);

    // TRowSet C layout (int64_t for counts so all fields are 8 bytes):
    // offset  0: Columns     TColumn*   <ptr TColumn>
    // offset  8: ColumnCount int64_t    i64
    // offset 16: RowCount    int64_t    i64
    // offset 24: Selection   uint8_t*   <ptr u8>
    // offset 32: Destroy     void*      <ptr i64>
    // offset 40: Private     void*      <ptr i64>
    // offset 48: RefCount    int64_t    i64
    // sizeof = 56
    auto rowSetType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Columns", ptrColumnType},
            {"ColumnCount", i64Type},
            {"RowCount", i64Type},
            {"Selection", ptrU8Type},
            {"Destroy", voidPtrType},
            {"Private", voidPtrType},
            {"RefCount", i64Type},
        });

    ExternalTypes_ = {
        { .Name = "TColumn", .Type = columnType },
        { .Name = "TRowSet", .Type = rowSetType },
    };

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

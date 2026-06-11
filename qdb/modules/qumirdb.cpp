#include "qumirdb.h"

#include <qdb/modules/qumirdb_runtime.h>

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

    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);

    // HashTable C layout (Stage 1: integer keys; aggregate accumulators are
    // all stored as int64_t regardless of the output column type). Every
    // array field is int64_t-based (including Dist/SlotId) so the embedded
    // Robin Hood kernel (kernel/rh_source.h) never has to mix integer
    // widths.
    //
    // Open-addressing arrays (Robin Hood, swap-on-insert; size == Capacity):
    // offset  0: Keys       int64_t*   <ptr i64>      probe table, Capacity*NumKeys, row-major
    // offset  8: Dist       int64_t*   <ptr i64>      probe distance; -1 = empty slot
    // offset 16: SlotId     int64_t*   <ptr i64>      dense-storage slot id for the occupied entry
    //
    // Dense storage, indexed by stable SlotId (never moves; size == Capacity):
    // offset 24: GroupKeys  int64_t*   <ptr i64>      Capacity*NumKeys, row-major
    // offset 32: AggBuffers int64_t**  <ptr <ptr i64>> NumAggs pointers, each -> int64_t[Capacity]
    //
    // Scratch buffers (NumKeys elements each):
    // offset 40: Scratch    int64_t*   <ptr i64>      ping-pong buffer used by rh_insert_slot's swaps
    // offset 48: Scratch2   int64_t*   <ptr i64>      ping-pong buffer used by rh_insert_slot's swaps
    // offset 56: QueryKey   int64_t*   <ptr i64>      staging buffer for the row currently being looked up
    //
    // Scalars:
    // offset 64: Capacity   int64_t    i64   (power of two; 0 before first grow)
    // offset 72: Size       int64_t    i64   (number of groups in dense storage)
    // offset 80: NumAggs    int64_t    i64
    // offset 88: NumKeys    int64_t    i64
    // sizeof = 96
    auto hashTableType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Keys", ptrI64Type},
            {"Dist", ptrI64Type},
            {"SlotId", ptrI64Type},
            {"GroupKeys", ptrI64Type},
            {"AggBuffers", ptrPtrI64Type},
            {"Scratch", ptrI64Type},
            {"Scratch2", ptrI64Type},
            {"QueryKey", ptrI64Type},
            {"Capacity", i64Type},
            {"Size", i64Type},
            {"NumAggs", i64Type},
            {"NumKeys", i64Type},
        });

    ExternalTypes_ = {
        { .Name = "TColumn", .Type = columnType },
        { .Name = "TRowSet", .Type = rowSetType },
        { .Name = "HashTable", .Type = hashTableType },
    };

    ExternalFunctions_ = {
        {
            .Name = "bitoff",
            .MangledName = "qdb_bitoff",
            .ArgTypes = { ptrU8Type, i64Type, i64Type },
            .ReturnType = boolType,
            .Inline = [boolType, i64Type](std::vector<TExprPtr> args) -> TExprPtr {
                std::vector<TExprPtr> stmts;
                auto addVar = [&](const std::string& name, TExprPtr value) {
                    auto var = std::make_shared<TVarStmt>(TLocation{}, name, nullptr);
                    var->Init = std::move(value);
                    stmts.push_back(std::move(var));
                };
                addVar("$$bitmap", args[0]);
                addVar("$$index", args[1]);
                addVar("$$bitoff", args[2]);

                auto bitIndex = binary("+", ident("$$index"), ident("$$bitoff"), i64Type);
                auto byteIndex = binary(">>", bitIndex, number(i64Type, 3), i64Type);
                auto bitPos = binary("&", bitIndex, number(i64Type, 7), i64Type);
                auto byte = std::make_shared<TIndexExpr>(TLocation{}, ident("$$bitmap"), byteIndex);
                auto byteAsI64 = cast(byte, i64Type);
                auto shifted = binary(">>", byteAsI64, bitPos, i64Type);
                auto masked = binary("&", shifted, number(i64Type, 1), i64Type);
                auto body = binary("!=", masked, number(i64Type, 0), boolType);

                auto bodyType = body->Type;
                stmts.push_back(std::move(body));
                auto block = std::make_shared<TBlockExpr>(TLocation{}, std::move(stmts));
                block->Type = std::move(bodyType);
                return block;
            },
        },
        {
            .Name = "qdb_alloc",
            .MangledName = "qdb_alloc",
            .Ptr = reinterpret_cast<void*>(static_cast<void*(*)(int64_t)>(qdb_alloc)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return reinterpret_cast<uint64_t>(qdb_alloc(static_cast<int64_t>(args[0])));
            },
            .ArgTypes = { i64Type },
            .ReturnType = ptrI8Type,
        },
        {
            .Name = "qdb_realloc",
            .MangledName = "qdb_realloc",
            .Ptr = reinterpret_cast<void*>(static_cast<void*(*)(void*, int64_t)>(qdb_realloc)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return reinterpret_cast<uint64_t>(
                    qdb_realloc(reinterpret_cast<void*>(args[0]), static_cast<int64_t>(args[1])));
            },
            .ArgTypes = { ptrI8Type, i64Type },
            .ReturnType = ptrI8Type,
        },
        {
            .Name = "qdb_free",
            .MangledName = "qdb_free",
            .Ptr = reinterpret_cast<void*>(static_cast<void(*)(void*)>(qdb_free)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                qdb_free(reinterpret_cast<void*>(args[0]));
                return 0;
            },
            .ArgTypes = { ptrI8Type },
            .ReturnType = std::make_shared<TVoidType>(),
        },
    };
}

} // namespace NRegistry
} // namespace NQumir

#include "qumirdb.h"

#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>

#include <bit>

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
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto f64Type = std::make_shared<TFloatType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto voidPtrType = std::make_shared<TPointerType>(i64Type);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI8Type = std::make_shared<TPointerType>(i8Type);

    auto makeStringHandleType = [&]() {
        return std::make_shared<TStructType>(
            std::vector<std::pair<std::string, TTypePtr>>{
                {"Data", ptrU8Type},
                {"Size", i64Type},
            });
    };
    auto stringViewType = makeStringHandleType();
    auto ownedStringType = makeStringHandleType();

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
            {"__qdb_padding_column_0", u8Type},
            {"__qdb_padding_column_1", u8Type},
            {"__qdb_padding_column_2", u8Type},
            {"__qdb_padding_column_3", u8Type},
            {"Mask", ptrU8Type},
            {"MaskBitOffset", i32Type},
            {"__qdb_padding_column_4", u8Type},
            {"__qdb_padding_column_5", u8Type},
            {"__qdb_padding_column_6", u8Type},
            {"__qdb_padding_column_7", u8Type},
            {"Offsets", voidPtrType},
            {"OffsetWidth", u8Type},
            {"__qdb_padding_column_8", u8Type},
            {"__qdb_padding_column_9", u8Type},
            {"__qdb_padding_column_10", u8Type},
            {"__qdb_padding_column_11", u8Type},
            {"__qdb_padding_column_12", u8Type},
            {"__qdb_padding_column_13", u8Type},
            {"__qdb_padding_column_14", u8Type},
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
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);

    // HashTable C layout. Key-owned storage is byte-addressed so generated
    // kernels can reinterpret it as the query's concrete Key type. Probe
    // metadata and aggregate accumulators remain int64_t-based.
    //
    // Open-addressing arrays (Robin Hood, swap-on-insert; size == Capacity):
    // offset  0: Keys       uint8_t*   <ptr u8>       probe table, Capacity*KeySize bytes
    // offset  8: Dist       int64_t*   <ptr i64>      probe distance; -1 = empty slot
    // offset 16: SlotId     int64_t*   <ptr i64>      dense-storage slot id for the occupied entry
    //
    // Dense storage, indexed by stable SlotId (never moves; size == Capacity):
    // offset 24: GroupKeys  uint8_t*   <ptr u8>       dense keys, Capacity*KeySize bytes
    // offset 32: AggBuffers int64_t**  <ptr <ptr i64>> NumAggs pointers, each -> int64_t[Capacity]
    //
    // Owned byte registry:
    // offset 40: OwnedBlocks        uint8_t**  <ptr <ptr u8>>
    // offset 48: OwnedBlockCount    int64_t    i64
    // offset 56: OwnedBlockCapacity int64_t    i64
    //
    // Scalars:
    // offset 64: Capacity   int64_t    i64   (power of two; 0 before first grow)
    // offset 72: Size       int64_t    i64   (number of groups in dense storage)
    // offset 80: NumAggs    int64_t    i64
    // offset 88: NumKeys    int64_t    i64
    // offset 96: KeySize    int64_t    i64   (sizeof the query-local Key)
    // sizeof = 104
    auto hashTableType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Keys", ptrU8Type},
            {"Dist", ptrI64Type},
            {"SlotId", ptrI64Type},
            {"GroupKeys", ptrU8Type},
            {"AggBuffers", ptrPtrI64Type},
            {"OwnedBlocks", ptrPtrU8Type},
            {"OwnedBlockCount", i64Type},
            {"OwnedBlockCapacity", i64Type},
            {"Capacity", i64Type},
            {"Size", i64Type},
            {"NumAggs", i64Type},
            {"NumKeys", i64Type},
            {"KeySize", i64Type},
        });

    // JoinTable C layout (symmetric hash join, one per side). Sparse Robin Hood
    // probe arrays + dense storage indexed by stable SlotId. The dense per-slot
    // RowId bucket is kept as three parallel arrays (count/capacity/data),
    // mirroring AggBuffers, so growth/rehash never mutate a struct in place.
    //
    // offset  0: Keys        uint8_t*   <ptr u8>        probe table, Capacity*KeySize
    // offset  8: Dist        int64_t*   <ptr i64>       probe distance; -1 = empty
    // offset 16: SlotId      int64_t*   <ptr i64>       dense slot id for occupied entry
    // offset 24: GroupKeys   uint8_t*   <ptr u8>        dense keys, Capacity*KeySize
    // offset 32: BucketCount int64_t*   <ptr i64>       per dense slot: # of RowIds
    // offset 40: BucketCap   int64_t*   <ptr i64>       per dense slot: bucket capacity
    // offset 48: BucketData  int64_t**  <ptr <ptr i64>> per dense slot: heap RowId array
    // offset 56: Capacity    int64_t    i64             power of two
    // offset 64: Size        int64_t    i64             number of distinct keys
    // offset 72: KeySize     int64_t    i64             sizeof the query-local Key
    // sizeof = 80
    auto joinTableType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Keys", ptrU8Type},
            {"Dist", ptrI64Type},
            {"SlotId", ptrI64Type},
            {"GroupKeys", ptrU8Type},
            {"BucketCount", ptrI64Type},
            {"BucketCap", ptrI64Type},
            {"BucketData", ptrPtrI64Type},
            {"Capacity", i64Type},
            {"Size", i64Type},
            {"KeySize", i64Type},
        });

    // PairBuffer C layout: growable list of (leftRowId, rightRowId) i64 pairs.
    // Data holds 2*Count int64 (interleaved). sizeof = 24.
    auto pairBufferType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Count", i64Type},
            {"Capacity", i64Type},
            {"Data", ptrI64Type},
        });

    ExternalTypes_ = {
        { .Name = "TColumn", .Type = columnType },
        { .Name = "TRowSet", .Type = rowSetType },
        { .Name = "HashTable", .Type = hashTableType },
        { .Name = "JoinTable", .Type = joinTableType },
        { .Name = "PairBuffer", .Type = pairBufferType },
        { .Name = "StringView", .Type = stringViewType },
        { .Name = "OwnedString", .Type = ownedStringType },
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
        {
            .Name = "qdb_f64_bits",
            .MangledName = "qdb_f64_bits",
            .Ptr = reinterpret_cast<void*>(static_cast<uint64_t(*)(double)>(qdb_f64_bits)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return qdb_f64_bits(std::bit_cast<double>(args[0]));
            },
            .ArgTypes = {f64Type},
            .ReturnType = u64Type,
        },
        {
            .Name = "qdb_filter_string_compare",
            .MangledName = "qdb_filter_string_compare",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                const uint8_t*, int64_t, const uint8_t*, int64_t)>(
                    qdb_filter_string_compare)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(qdb_filter_string_compare(
                    reinterpret_cast<const uint8_t*>(args[0]),
                    static_cast<int64_t>(args[1]),
                    reinterpret_cast<const uint8_t*>(args[2]),
                    static_cast<int64_t>(args[3])));
            },
            .ArgTypes = {ptrU8Type, i64Type, ptrU8Type, i64Type},
            .ReturnType = i64Type,
        },
        {
            .Name = "qdb_bitmap_set_valid",
            .MangledName = "qdb_bitmap_set_valid",
            .Ptr = reinterpret_cast<void*>(static_cast<void(*)(
                uint8_t*, int64_t, bool)>(qdb_bitmap_set_valid)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                qdb_bitmap_set_valid(
                    reinterpret_cast<uint8_t*>(args[0]),
                    static_cast<int64_t>(args[1]), args[2] != 0);
                return 0;
            },
            .ArgTypes = {ptrU8Type, i64Type, boolType},
            .ReturnType = std::make_shared<TVoidType>(),
        },
        {
            .Name = "qdb_sql_bool_and",
            .MangledName = "qdb_sql_bool_and",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                int64_t, int64_t)>(qdb_sql_bool_and)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return qdb_sql_bool_and(args[0], args[1]);
            },
            .ArgTypes = {i64Type, i64Type},
            .ReturnType = i64Type,
        },
        {
            .Name = "qdb_sql_bool_or",
            .MangledName = "qdb_sql_bool_or",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                int64_t, int64_t)>(qdb_sql_bool_or)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return qdb_sql_bool_or(args[0], args[1]);
            },
            .ArgTypes = {i64Type, i64Type},
            .ReturnType = i64Type,
        },
        {
            .Name = "qdb_sql_bool_not",
            .MangledName = "qdb_sql_bool_not",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                int64_t)>(qdb_sql_bool_not)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return qdb_sql_bool_not(args[0]);
            },
            .ArgTypes = {i64Type},
            .ReturnType = i64Type,
        },
    };
}

} // namespace NRegistry
} // namespace NQumir

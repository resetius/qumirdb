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

TExprPtr field(TExprPtr obj, const std::string& name) {
    return std::make_shared<TFieldAccessExpr>(obj->Location, std::move(obj), name);
}

TExprPtr call(const std::string& fn, std::vector<TExprPtr> args) {
    TLocation loc{};
    return std::make_shared<TCallExpr>(loc, ident(fn), std::move(args));
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
    auto namedStringViewType = std::make_shared<TNamedType>("StringView", stringViewType);
    auto ownedStringType = makeStringHandleType();
    auto stringLiteralType = std::make_shared<TStringType>();

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

    // PairBuffer C layout: growable list of (leftRowId, rightRowId) i64 pairs.
    // Data holds 2*Count int64 (interleaved). sizeof = 24. The symmetric hash
    // join reuses the aggregation HashTable as its hash map (dense per-slot
    // RowId bucket stored in AggBuffers with NumAggs=3), so no separate
    // JoinTable type is needed.
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
            .Name = "qdb_string_view_sql_like",
            .MangledName = "qdb_string_view_sql_like",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                qdb_string_view, const char*)>(qdb_string_view_sql_like)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                qdb_string_view str = *reinterpret_cast<const qdb_string_view*>(args[0]);
                return static_cast<uint64_t>(qdb_string_view_sql_like(str, reinterpret_cast<const char*>(args[1])));
            },
            .ArgTypes = {namedStringViewType, stringLiteralType},
            .ReturnType = i64Type,
        },
        {
            .Name = "qdb_string_view_cmp_cstr",
            .MangledName = "qdb_string_view_cmp_cstr",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                const uint8_t*, int64_t, const char*)>(qdb_string_view_cmp_cstr)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(qdb_string_view_cmp_cstr(
                    reinterpret_cast<const uint8_t*>(args[0]),
                    static_cast<int64_t>(args[1]),
                    reinterpret_cast<const char*>(args[2])));
            },
            .ArgTypes = {ptrU8Type, i64Type, stringLiteralType},
            .ReturnType = i64Type,
        },
        {
            .Name = "qdb_cstr_cmp_cstr",
            .MangledName = "qdb_cstr_cmp_cstr",
            .Ptr = reinterpret_cast<void*>(static_cast<int64_t(*)(
                const char*, const char*)>(qdb_cstr_cmp_cstr)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(qdb_cstr_cmp_cstr(
                    reinterpret_cast<const char*>(args[0]),
                    reinterpret_cast<const char*>(args[1])));
            },
            .ArgTypes = {stringLiteralType, stringLiteralType},
            .ReturnType = i64Type,
        },
        // ── StringView comparison operators ──────────────────────────────────
        // All inlines reduce to cmp(...) op 0 so the result is i1 (sext→u8 = 0xff/0x00).
#define QDB_SV_SV_OP(opname, mangled, op) \
        { \
            .Name = opname, \
            .MangledName = mangled, \
            .ArgTypes = {namedStringViewType, namedStringViewType}, \
            .ReturnType = i64Type, \
            .IsOp = true, \
            .Inline = [i64Type](std::vector<TExprPtr> args) -> TExprPtr { \
                auto cmp = call("qdb_filter_string_compare", { \
                    field(args[0], "Data"), field(args[0], "Size"), \
                    field(args[1], "Data"), field(args[1], "Size"), \
                }); \
                return std::make_shared<TBinaryExpr>(TLocation{}, TOperator(op), \
                    std::move(cmp), number(i64Type, 0)); \
            }, \
        },
        QDB_SV_SV_OP("==", "qdb_sv_sv_eq", "==")
        QDB_SV_SV_OP("!=", "qdb_sv_sv_ne", "!=")
        QDB_SV_SV_OP("<",  "qdb_sv_sv_lt", "<")
        QDB_SV_SV_OP("<=", "qdb_sv_sv_le", "<=")
        QDB_SV_SV_OP(">",  "qdb_sv_sv_gt", ">")
        QDB_SV_SV_OP(">=", "qdb_sv_sv_ge", ">=")
#undef QDB_SV_SV_OP
#define QDB_SV_LIT_OP(opname, mangled, op) \
        { \
            .Name = opname, \
            .MangledName = mangled, \
            .ArgTypes = {namedStringViewType, stringLiteralType}, \
            .ReturnType = i64Type, \
            .IsOp = true, \
            .Inline = [i64Type](std::vector<TExprPtr> args) -> TExprPtr { \
                auto cmp = call("qdb_string_view_cmp_cstr", { \
                    field(args[0], "Data"), field(args[0], "Size"), args[1], \
                }); \
                return std::make_shared<TBinaryExpr>(TLocation{}, TOperator(op), \
                    std::move(cmp), number(i64Type, 0)); \
            }, \
        },
        QDB_SV_LIT_OP("==", "qdb_sv_lit_eq", "==")
        QDB_SV_LIT_OP("!=", "qdb_sv_lit_ne", "!=")
        QDB_SV_LIT_OP("<",  "qdb_sv_lit_lt", "<")
        QDB_SV_LIT_OP("<=", "qdb_sv_lit_le", "<=")
        QDB_SV_LIT_OP(">",  "qdb_sv_lit_gt", ">")
        QDB_SV_LIT_OP(">=", "qdb_sv_lit_ge", ">=")
#undef QDB_SV_LIT_OP
#define QDB_LIT_SV_OP(opname, mangled, op) \
        { \
            .Name = opname, \
            .MangledName = mangled, \
            .ArgTypes = {stringLiteralType, namedStringViewType}, \
            .ReturnType = i64Type, \
            .IsOp = true, \
            .Inline = [i64Type](std::vector<TExprPtr> args) -> TExprPtr { \
                auto cmp = call("qdb_string_view_cmp_cstr", { \
                    field(args[1], "Data"), field(args[1], "Size"), args[0], \
                }); \
                return std::make_shared<TBinaryExpr>(TLocation{}, TOperator(op), \
                    number(i64Type, 0), std::move(cmp)); \
            }, \
        },
        QDB_LIT_SV_OP("==", "qdb_lit_sv_eq", "==")
        QDB_LIT_SV_OP("!=", "qdb_lit_sv_ne", "!=")
        QDB_LIT_SV_OP("<",  "qdb_lit_sv_lt", "<")
        QDB_LIT_SV_OP("<=", "qdb_lit_sv_le", "<=")
        QDB_LIT_SV_OP(">",  "qdb_lit_sv_gt", ">")
        QDB_LIT_SV_OP(">=", "qdb_lit_sv_ge", ">=")
#undef QDB_LIT_SV_OP
#define QDB_LIT_LIT_OP(opname, mangled, op) \
        { \
            .Name = opname, \
            .MangledName = mangled, \
            .ArgTypes = {stringLiteralType, stringLiteralType}, \
            .ReturnType = i64Type, \
            .IsOp = true, \
            .Inline = [i64Type](std::vector<TExprPtr> args) -> TExprPtr { \
                auto cmp = call("qdb_cstr_cmp_cstr", {args[0], args[1]}); \
                return std::make_shared<TBinaryExpr>(TLocation{}, TOperator(op), \
                    std::move(cmp), number(i64Type, 0)); \
            }, \
        },
        QDB_LIT_LIT_OP("==", "qdb_lit_lit_eq", "==")
        QDB_LIT_LIT_OP("!=", "qdb_lit_lit_ne", "!=")
        QDB_LIT_LIT_OP("<",  "qdb_lit_lit_lt", "<")
        QDB_LIT_LIT_OP("<=", "qdb_lit_lit_le", "<=")
        QDB_LIT_LIT_OP(">",  "qdb_lit_lit_gt", ">")
        QDB_LIT_LIT_OP(">=", "qdb_lit_lit_ge", ">=")
#undef QDB_LIT_LIT_OP
        {
            .Name = "qdb_substring",
            .MangledName = "qdb_substring",
            .Ptr = reinterpret_cast<void*>(static_cast<qdb_string_view(*)(
                qdb_string_view, int32_t, int32_t)>(qdb_substring)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                // args[0] = sret pointer (destination qdb_string_view)
                // args[1] = pointer to source qdb_string_view
                // args[2] = start (1-based), args[3] = length
                auto* dst = reinterpret_cast<qdb_string_view*>(args[0]);
                qdb_string_view str = *reinterpret_cast<const qdb_string_view*>(args[1]);
                int32_t start  = static_cast<int32_t>(args[2]);
                int32_t length = static_cast<int32_t>(args[3]);
                *dst = qdb_substring(str, start, length);
                return 0;
            },
            .ArgTypes = {namedStringViewType, i32Type, i32Type},
            .ReturnType = namedStringViewType,
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
        {
            .Name = "qdb_date_year",
            .MangledName = "qdb_date_year",
            .Ptr = reinterpret_cast<void*>(static_cast<int32_t(*)(int32_t)>(qdb_date_year)),
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(static_cast<uint32_t>(
                    qdb_date_year(static_cast<int32_t>(args[0]))));
            },
            .ArgTypes = {i32Type},
            .ReturnType = i32Type,
        },
    };
}

} // namespace NRegistry
} // namespace NQumir

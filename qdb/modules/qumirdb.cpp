#include "qumirdb.h"

#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/parser/ast.h>

namespace NQumir {
namespace NRegistry {

using namespace NAst;

const void* const QumirDbRuntimeSymbolAnchors[] __attribute__((used)) = {
    reinterpret_cast<const void*>(&qdb_alloc),
    reinterpret_cast<const void*>(&qdb_free),
    reinterpret_cast<const void*>(&qdb_filter_string_compare),
    reinterpret_cast<const void*>(&qdb_string_view_sql_like),
    reinterpret_cast<const void*>(&qdb_substring),
    reinterpret_cast<const void*>(&qdb_date_year),
    reinterpret_cast<const void*>(&qdb_sql_date),
    reinterpret_cast<const void*>(&qdb_sql_interval),
    reinterpret_cast<const void*>(&qdb_sql_bool_and),
    reinterpret_cast<const void*>(&qdb_sql_bool_or),
    reinterpret_cast<const void*>(&qdb_sql_bool_not),
};

std::vector<TExternalType> BuildQumirDbExternalTypes() {
    auto i8Type = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>();
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

    auto pairBufferType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Count", i64Type},
            {"Capacity", i64Type},
            {"Data", ptrI64Type},
        });

    return {
        { .Name = "TColumn", .Type = columnType },
        { .Name = "TRowSet", .Type = rowSetType },
        { .Name = "HashTable", .Type = hashTableType },
        { .Name = "PairBuffer", .Type = pairBufferType },
        { .Name = "StringView", .Type = stringViewType },
        { .Name = "OwnedString", .Type = ownedStringType },
    };
}

const std::vector<TExternalType>& QumirDbExternalTypes() {
    static const auto types = BuildQumirDbExternalTypes();
    return types;
}

void EnsureQumirDbRuntimeSymbolsLinked() {
    (void)QumirDbRuntimeSymbolAnchors;
}

} // namespace NRegistry
} // namespace NQumir

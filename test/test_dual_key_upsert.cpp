#include <gtest/gtest.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/lib.h>
#include <qdb/io/io.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/modules/qumirdb_types.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace {

struct THashTable {
    uint8_t* Keys = nullptr;
    int64_t* Dist = nullptr;
    int64_t* SlotId = nullptr;
    uint8_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    uint8_t** OwnedBlocks = nullptr;
    int64_t OwnedBlockCount = 0;
    int64_t OwnedBlockCapacity = 0;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
    int64_t KeySize = 0;
};

static_assert(sizeof(THashTable) == 104);

std::unique_ptr<NQumir::TLLVMRunner> CompileDualUpsert(
    const NQqb::NKernel::TAggregateKeyDescriptor& key,
    const std::string& entrySource,
    const std::string& entryName,
    void*& entry)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    std::vector<TExprPtr> stmts;
    for (const char* file : {
             "string_ops.oz", "owned_blocks.oz", "robin_hood_dual_key.oz"}) {
        auto library = NQqb::NKernel::ParseFunctionLibrary(
            NQqb::NKernel::ReadAggregationKernel(file));
        if (!library) {
            ADD_FAILURE() << library.error().ToString();
            return {};
        }
        stmts.insert(stmts.end(), library->begin(), library->end());
    }
    auto operations = NQqb::NKernel::GenKeyOperationFunDecls(key);
    stmts.insert(stmts.end(), operations.begin(), operations.end());
    auto ownership = NQqb::NKernel::GenKeyOwnershipFunDecls(key);
    stmts.insert(stmts.end(), ownership.begin(), ownership.end());
    auto wrapper = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    if (!wrapper) {
        ADD_FAILURE() << wrapper.error().ToString();
        return {};
    }
    stmts.insert(stmts.end(), wrapper->begin(), wrapper->end());

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<TBlockExpr>(TLocation{}, std::move(stmts));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileAggregateDispatch(
    const NQumir::NAst::TStructType& input,
    const NQqb::NKernel::TAggregateKeyDescriptor& key,
    void*& entry)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto module = std::make_shared<NRegistry::QumirDbModule>();
    TTypePtr columnType;
    TTypePtr rowSetType;
    TTypePtr hashTableType;
    for (const auto& type : module->ExternalTypes()) {
        if (type.Name == "TColumn") {
            columnType = type.Type;
        } else if (type.Name == "TRowSet") {
            rowSetType = type.Type;
        } else if (type.Name == "HashTable") {
            hashTableType = type.Type;
        }
    }
    auto program = NQqb::NKernel::BuildGenericAggregateProgramAst(
        input, key, std::string("value"), {"count", "sum"},
        columnType, rowSetType, hashTableType);
    if (!program) {
        ADD_FAILURE() << program.error().ToString();
        return {};
    }

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(module, true);
    std::string error;
    entry = runner->CompileKernelAst(*program, "agg_dispatch", &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

void DestroyOwnedBlocks(THashTable& table) {
    for (int64_t i = 0; i < table.OwnedBlockCount; ++i) {
        qdb_free(table.OwnedBlocks[i]);
    }
    qdb_free(table.OwnedBlocks);
    table.OwnedBlocks = nullptr;
    table.OwnedBlockCount = 0;
    table.OwnedBlockCapacity = 0;
}

template <typename T>
void InitHeapTable(THashTable& table, int64_t capacity) {
    table.Keys = static_cast<uint8_t*>(qdb_alloc(capacity * sizeof(T)));
    table.Dist = static_cast<int64_t*>(qdb_alloc(capacity * sizeof(int64_t)));
    table.SlotId = static_cast<int64_t*>(qdb_alloc(capacity * sizeof(int64_t)));
    table.GroupKeys = static_cast<uint8_t*>(qdb_alloc(capacity * sizeof(T)));
    ASSERT_NE(table.Keys, nullptr);
    ASSERT_NE(table.Dist, nullptr);
    ASSERT_NE(table.SlotId, nullptr);
    ASSERT_NE(table.GroupKeys, nullptr);
    for (int64_t i = 0; i < capacity; ++i) {
        table.Dist[i] = -1;
        table.SlotId[i] = -1;
    }
    table.Capacity = capacity;
    table.KeySize = sizeof(T);
}

void DestroyHeapTable(THashTable& table) {
    qdb_free(table.Keys);
    qdb_free(table.Dist);
    qdb_free(table.SlotId);
    qdb_free(table.GroupKeys);
    for (int64_t i = 0; i < table.NumAggs; ++i) {
        qdb_free(table.AggBuffers[i]);
    }
    qdb_free(table.AggBuffers);
    DestroyOwnedBlocks(table);
}

void AddAggregateBuffers(THashTable& table, int64_t count) {
    table.AggBuffers = static_cast<int64_t**>(
        qdb_alloc(count * sizeof(int64_t*)));
    ASSERT_NE(table.AggBuffers, nullptr);
    table.NumAggs = count;
    for (int64_t agg = 0; agg < count; ++agg) {
        table.AggBuffers[agg] = static_cast<int64_t*>(
            qdb_alloc(table.Capacity * sizeof(int64_t)));
        ASSERT_NE(table.AggBuffers[agg], nullptr);
    }
}

TEST(DualKeyUpsert, FixedWidthUsesIdentityClone) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({{"key", i64}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    const std::string source = R"oz(
(block
  (fun upsert_i64 ((var ht <ref HashTable>)
                   (var key i64)
                   (var out_is_new <ptr i64>)) -> i64
    (block (return (call aht_upsert_dual
      ht key key (index out_is_new (: 0 i64)))))))
)oz";
    void* entry = nullptr;
    auto runner = CompileDualUpsert(key, source, "upsert_i64", entry);
    ASSERT_NE(entry, nullptr);
    auto upsert = reinterpret_cast<int64_t(*)(
        THashTable*, int64_t, int64_t*)>(entry);

    std::array<int64_t, 4> keys{};
    std::array<int64_t, 4> groupKeys{};
    std::array<int64_t, 4> dist = {-1, -1, -1, -1};
    std::array<int64_t, 4> slotIds = {-1, -1, -1, -1};
    THashTable table{
        .Keys = reinterpret_cast<uint8_t*>(keys.data()),
        .Dist = dist.data(),
        .SlotId = slotIds.data(),
        .GroupKeys = reinterpret_cast<uint8_t*>(groupKeys.data()),
        .Capacity = 4,
        .KeySize = 8,
    };
    int64_t isNew = 0;
    EXPECT_EQ(upsert(&table, 42, &isNew), 0);
    EXPECT_EQ(isNew, 1);
    EXPECT_EQ(table.OwnedBlockCount, 0);
    EXPECT_EQ(groupKeys[0], 42);
    EXPECT_EQ(upsert(&table, 42, &isNew), 0);
    EXPECT_EQ(isNew, 0);
    EXPECT_EQ(table.Size, 1);
}

TEST(DualKeyUpsert, StringClonesOnlyAfterMiss) {
    using namespace NQumir::NAst;
    TStructType input({{"key", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    const std::string source = R"oz(
(block
  (fun upsert_string ((var ht <ref HashTable>)
                      (var key StringView)
                      (var stored_witness OwnedString)
                      (var out_is_new <ptr i64>)) -> i64
    (block (return (call aht_upsert_dual
      ht key stored_witness (index out_is_new (: 0 i64)))))))
)oz";
    void* entry = nullptr;
    auto runner = CompileDualUpsert(key, source, "upsert_string", entry);
    ASSERT_NE(entry, nullptr);
    auto upsert = reinterpret_cast<int64_t(*)(
        THashTable*, NQqb::TStringView, NQqb::TOwnedString, int64_t*)>(entry);

    std::array<NQqb::TOwnedString, 4> keys{};
    std::array<NQqb::TOwnedString, 4> groupKeys{};
    std::array<int64_t, 4> dist = {-1, -1, -1, -1};
    std::array<int64_t, 4> slotIds = {-1, -1, -1, -1};
    THashTable table{
        .Keys = reinterpret_cast<uint8_t*>(keys.data()),
        .Dist = dist.data(),
        .SlotId = slotIds.data(),
        .GroupKeys = reinterpret_cast<uint8_t*>(groupKeys.data()),
        .Capacity = 4,
        .KeySize = 16,
    };
    std::string first = "alpha";
    NQqb::TStringView firstView{
        .Data = reinterpret_cast<uint8_t*>(first.data()),
        .Size = static_cast<int64_t>(first.size()),
    };
    int64_t isNew = 0;
    EXPECT_EQ(upsert(&table, firstView, {}, &isNew), 0);
    ASSERT_EQ(isNew, 1);
    ASSERT_EQ(table.OwnedBlockCount, 1);
    ASSERT_NE(groupKeys[0].Data, firstView.Data);
    EXPECT_EQ(std::string(
        reinterpret_cast<char*>(groupKeys[0].Data), groupKeys[0].Size), first);

    std::string duplicate = "alpha";
    NQqb::TStringView duplicateView{
        .Data = reinterpret_cast<uint8_t*>(duplicate.data()),
        .Size = static_cast<int64_t>(duplicate.size()),
    };
    EXPECT_EQ(upsert(&table, duplicateView, {}, &isNew), 0);
    EXPECT_EQ(isNew, 0);
    EXPECT_EQ(table.OwnedBlockCount, 1);
    EXPECT_EQ(table.Size, 1);

    std::string second = "beta";
    NQqb::TStringView secondView{
        .Data = reinterpret_cast<uint8_t*>(second.data()),
        .Size = static_cast<int64_t>(second.size()),
    };
    EXPECT_EQ(upsert(&table, secondView, {}, &isNew), 1);
    EXPECT_EQ(isNew, 1);
    EXPECT_EQ(table.OwnedBlockCount, 2);
    EXPECT_EQ(table.Size, 2);
    DestroyOwnedBlocks(table);
}

TEST(DualKeyUpsert, StoredRehashPreservesOwnedPointers) {
    using namespace NQumir::NAst;
    TStructType input({{"key", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    const std::string upsertSource = R"oz(
(block
  (fun upsert_string ((var ht <ref HashTable>)
                      (var key StringView)
                      (var stored_witness OwnedString)
                      (var out_is_new <ptr i64>)) -> i64
    (block (return (call aht_upsert_dual
      ht key stored_witness (index out_is_new (: 0 i64)))))))
)oz";
    const std::string rehashSource = R"oz(
(block
  (fun rehash_string ((var old_keys <ptr OwnedString>)
                      (var old_dist <ptr i64>)
                      (var old_slot_ids <ptr i64>)
                      (var old_capacity i64)
                      (var new_keys <ptr OwnedString>)
                      (var new_dist <ptr i64>)
                      (var new_slot_ids <ptr i64>)
                      (var new_capacity i64)
                      (var stored_witness OwnedString)) -> bool
    (block (return (call rh_rehash_stored
      old_keys old_dist old_slot_ids old_capacity
      new_keys new_dist new_slot_ids new_capacity stored_witness)))))
)oz";
    void* upsertEntry = nullptr;
    auto upsertRunner = CompileDualUpsert(
        key, upsertSource, "upsert_string", upsertEntry);
    ASSERT_NE(upsertEntry, nullptr);
    void* rehashEntry = nullptr;
    auto rehashRunner = CompileDualUpsert(
        key, rehashSource, "rehash_string", rehashEntry);
    ASSERT_NE(rehashEntry, nullptr);
    auto upsert = reinterpret_cast<int64_t(*)(
        THashTable*, NQqb::TStringView, NQqb::TOwnedString, int64_t*)>(
        upsertEntry);
    auto rehash = reinterpret_cast<bool(*)(
        NQqb::TOwnedString*, int64_t*, int64_t*, int64_t,
        NQqb::TOwnedString*, int64_t*, int64_t*, int64_t,
        NQqb::TOwnedString)>(rehashEntry);

    std::array<NQqb::TOwnedString, 4> oldKeys{};
    std::array<NQqb::TOwnedString, 4> groupKeys{};
    std::array<int64_t, 4> oldDist = {-1, -1, -1, -1};
    std::array<int64_t, 4> oldSlotIds = {-1, -1, -1, -1};
    THashTable table{
        .Keys = reinterpret_cast<uint8_t*>(oldKeys.data()),
        .Dist = oldDist.data(),
        .SlotId = oldSlotIds.data(),
        .GroupKeys = reinterpret_cast<uint8_t*>(groupKeys.data()),
        .Capacity = 4,
        .KeySize = 16,
    };
    std::array<std::string, 3> values = {"alpha", "beta", "gamma"};
    int64_t isNew = 0;
    for (auto& value : values) {
        NQqb::TStringView view{
            .Data = reinterpret_cast<uint8_t*>(value.data()),
            .Size = static_cast<int64_t>(value.size()),
        };
        ASSERT_GE(upsert(&table, view, {}, &isNew), 0);
        ASSERT_EQ(isNew, 1);
    }
    ASSERT_EQ(table.OwnedBlockCount, 3);
    std::array<uint8_t*, 3> ownedPointers = {
        groupKeys[0].Data, groupKeys[1].Data, groupKeys[2].Data};

    std::array<NQqb::TOwnedString, 8> newKeys{};
    std::array<int64_t, 8> newDist{};
    std::array<int64_t, 8> newSlotIds{};
    ASSERT_TRUE(rehash(
        oldKeys.data(), oldDist.data(), oldSlotIds.data(), 4,
        newKeys.data(), newDist.data(), newSlotIds.data(), 8, {}));
    std::array<bool, 3> found{};
    for (size_t slot = 0; slot < newKeys.size(); ++slot) {
        if (newDist[slot] < 0) {
            continue;
        }
        for (size_t i = 0; i < ownedPointers.size(); ++i) {
            if (newKeys[slot].Data == ownedPointers[i]) {
                found[i] = true;
            }
        }
    }
    EXPECT_EQ(found, (std::array<bool, 3>{true, true, true}));
    EXPECT_EQ(table.OwnedBlockCount, 3);
    DestroyOwnedBlocks(table);
}

TEST(DualKeyUpsert, FixedWidthGrowsThroughGenericPath) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({{"key", i64}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    const std::string source = R"oz(
(block
  (fun upsert_i64 ((var ht <ref HashTable>)
                   (var key i64)
                   (var out_is_new <ptr i64>)) -> i64
    (block (return (call aht_upsert_dual
      ht key key (index out_is_new (: 0 i64)))))))
)oz";
    void* entry = nullptr;
    auto runner = CompileDualUpsert(key, source, "upsert_i64", entry);
    ASSERT_NE(entry, nullptr);
    auto upsert = reinterpret_cast<int64_t(*)(
        THashTable*, int64_t, int64_t*)>(entry);

    THashTable table;
    InitHeapTable<int64_t>(table, 2);
    int64_t isNew = 0;
    for (int64_t value = 0; value < 17; ++value) {
        EXPECT_EQ(upsert(&table, value, &isNew), value);
        EXPECT_EQ(isNew, 1);
    }
    EXPECT_EQ(table.Capacity, 32);
    EXPECT_EQ(table.Size, 17);
    EXPECT_EQ(table.OwnedBlockCount, 0);
    auto* groupKeys = reinterpret_cast<int64_t*>(table.GroupKeys);
    for (int64_t value = 0; value < 17; ++value) {
        EXPECT_EQ(groupKeys[value], value);
        EXPECT_EQ(upsert(&table, value, &isNew), value);
        EXPECT_EQ(isNew, 0);
    }
    DestroyHeapTable(table);
}

TEST(DualKeyUpsert, StringGrowPreservesOwnedPointers) {
    using namespace NQumir::NAst;
    TStructType input({{"key", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    const std::string source = R"oz(
(block
  (fun upsert_string ((var ht <ref HashTable>)
                      (var key StringView)
                      (var stored_witness OwnedString)
                      (var out_is_new <ptr i64>)) -> i64
    (block (return (call aht_upsert_dual
      ht key stored_witness (index out_is_new (: 0 i64)))))))
)oz";
    void* entry = nullptr;
    auto runner = CompileDualUpsert(key, source, "upsert_string", entry);
    ASSERT_NE(entry, nullptr);
    auto upsert = reinterpret_cast<int64_t(*)(
        THashTable*, NQqb::TStringView, NQqb::TOwnedString, int64_t*)>(entry);

    THashTable table;
    InitHeapTable<NQqb::TOwnedString>(table, 2);
    AddAggregateBuffers(table, 2);
    std::vector<std::string> values;
    std::vector<uint8_t*> ownedPointers;
    int64_t isNew = 0;
    for (int64_t i = 0; i < 17; ++i) {
        values.push_back("key_" + std::to_string(i));
        auto& value = values.back();
        NQqb::TStringView view{
            .Data = reinterpret_cast<uint8_t*>(value.data()),
            .Size = static_cast<int64_t>(value.size()),
        };
        ASSERT_EQ(upsert(&table, view, {}, &isNew), i);
        ASSERT_EQ(isNew, 1);
        table.AggBuffers[0][i] = i * 10;
        table.AggBuffers[1][i] = i * 100;
        auto* groupKeys = reinterpret_cast<NQqb::TOwnedString*>(table.GroupKeys);
        ownedPointers.push_back(groupKeys[i].Data);
    }
    ASSERT_EQ(table.Capacity, 32);
    ASSERT_EQ(table.Size, 17);
    ASSERT_EQ(table.OwnedBlockCount, 17);
    auto* groupKeys = reinterpret_cast<NQqb::TOwnedString*>(table.GroupKeys);
    for (int64_t i = 0; i < 17; ++i) {
        EXPECT_EQ(groupKeys[i].Data, ownedPointers[i]);
        EXPECT_EQ(std::string(
            reinterpret_cast<char*>(groupKeys[i].Data), groupKeys[i].Size),
            values[i]);
        EXPECT_EQ(table.AggBuffers[0][i], i * 10);
        EXPECT_EQ(table.AggBuffers[1][i], i * 100);
        NQqb::TStringView duplicate{
            .Data = reinterpret_cast<uint8_t*>(values[i].data()),
            .Size = static_cast<int64_t>(values[i].size()),
        };
        EXPECT_EQ(upsert(&table, duplicate, {}, &isNew), i);
        EXPECT_EQ(isNew, 0);
    }
    EXPECT_EQ(table.OwnedBlockCount, 17);
    DestroyHeapTable(table);
}

TEST(DualKeyUpsert, ProductionDispatchMaterializesStringKeys) {
    using namespace NQumir::NAst;
    auto stringType = std::make_shared<TStringType>();
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({{"key", stringType}, {"value", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});

    void* entry = nullptr;
    auto runner = CompileAggregateDispatch(input, key, entry);
    ASSERT_NE(entry, nullptr);
    using TDispatch = int64_t(*)(THashTable*, NQqb::TRowSet*, int64_t, int64_t);
    auto dispatch = reinterpret_cast<TDispatch>(entry);

    std::string data1 = "alphabetaalphagamma";
    std::array<int32_t, 5> offsets1 = {0, 5, 9, 14, 19};
    std::array<int64_t, 4> values1 = {10, 20, 5, 100};
    std::array<uint8_t, 4> selection1 = {1, 1, 1, 0};
    std::array<NQqb::TColumn, 2> columns1 = {
        NQqb::TColumn{
            .Data = data1.data(),
            .Mask = nullptr,
            .Offsets = offsets1.data(),
            .OffsetWidth = 4,
        },
        NQqb::TColumn{
            .Data = reinterpret_cast<char*>(values1.data()),
            .Mask = nullptr,
            .Offsets = nullptr,
            .OffsetWidth = 0,
        },
    };
    NQqb::TRowSet batch1{
        .Columns = columns1.data(),
        .ColumnCount = 2,
        .RowCount = 4,
        .Selection = selection1.data(),
        .RefCount = 1,
    };

    std::string data2 = "gammadeltabeta";
    std::array<int32_t, 4> offsets2 = {0, 5, 10, 14};
    std::array<int64_t, 3> values2 = {7, 11, -3};
    std::array<NQqb::TColumn, 2> columns2 = {
        NQqb::TColumn{
            .Data = data2.data(),
            .Mask = nullptr,
            .Offsets = offsets2.data(),
            .OffsetWidth = 4,
        },
        NQqb::TColumn{
            .Data = reinterpret_cast<char*>(values2.data()),
            .Mask = nullptr,
            .Offsets = nullptr,
            .OffsetWidth = 0,
        },
    };
    NQqb::TRowSet batch2{
        .Columns = columns2.data(),
        .ColumnCount = 2,
        .RowCount = 3,
        .Selection = nullptr,
        .RefCount = 1,
    };

    THashTable table;
    ASSERT_NE(dispatch(&table, &batch1, 2, 0), 0);
    ASSERT_EQ(dispatch(&table, &batch1, 0, 1), 0);
    ASSERT_EQ(dispatch(&table, &batch2, 0, 1), 0);
    ASSERT_EQ(table.Size, 4);
    ASSERT_EQ(table.Capacity, 8);
    ASSERT_EQ(table.OwnedBlockCount, 4);

    const std::map<std::string, std::pair<int64_t, int64_t>> expected = {
        {"alpha", {2, 15}},
        {"beta", {2, 17}},
        {"gamma", {1, 7}},
        {"delta", {1, 11}},
    };
    auto* groupKeys = reinterpret_cast<NQqb::TOwnedString*>(table.GroupKeys);
    for (int64_t slot = 0; slot < table.Size; ++slot) {
        std::string value(
            reinterpret_cast<char*>(groupKeys[slot].Data), groupKeys[slot].Size);
        auto it = expected.find(value);
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(table.AggBuffers[0][slot], it->second.first);
        EXPECT_EQ(table.AggBuffers[1][slot], it->second.second);
        EXPECT_NE(groupKeys[slot].Data,
            reinterpret_cast<uint8_t*>(data1.data()));
        EXPECT_NE(groupKeys[slot].Data,
            reinterpret_cast<uint8_t*>(data2.data()));
    }

    EXPECT_EQ(dispatch(&table, &batch2, 0, 2), 1);
}

TEST(DualKeyUpsert, ProductionDispatchMaterializesMixedStringKeys) {
    using namespace NQumir::NAst;
    auto stringType = std::make_shared<TStringType>();
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({
        {"id", i64Type}, {"name", stringType}, {"value", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
        input, {"id", "name"});

    void* entry = nullptr;
    auto runner = CompileAggregateDispatch(input, key, entry);
    ASSERT_NE(entry, nullptr);
    using TDispatch = int64_t(*)(THashTable*, NQqb::TRowSet*, int64_t, int64_t);
    auto dispatch = reinterpret_cast<TDispatch>(entry);

    std::array<int64_t, 4> ids1 = {1, 1, 2, 3};
    std::string data1 = "alphaalphabetagamma";
    std::array<int32_t, 5> offsets1 = {0, 5, 10, 14, 19};
    std::array<int64_t, 4> values1 = {10, 5, 20, 100};
    std::array<uint8_t, 4> selection1 = {1, 1, 1, 0};
    std::array<NQqb::TColumn, 3> columns1 = {
        NQqb::TColumn{
            .Data = reinterpret_cast<char*>(ids1.data()), .Mask = nullptr},
        NQqb::TColumn{
            .Data = data1.data(), .Mask = nullptr,
            .Offsets = offsets1.data(), .OffsetWidth = 4},
        NQqb::TColumn{
            .Data = reinterpret_cast<char*>(values1.data()), .Mask = nullptr},
    };
    NQqb::TRowSet batch1{
        .Columns = columns1.data(), .ColumnCount = 3, .RowCount = 4,
        .Selection = selection1.data(), .RefCount = 1};

    std::array<int64_t, 3> ids2 = {2, 4, 1};
    std::string data2 = "betadeltaalpha";
    std::array<int32_t, 4> offsets2 = {0, 4, 9, 14};
    std::array<int64_t, 3> values2 = {-3, 11, 7};
    std::array<NQqb::TColumn, 3> columns2 = {
        NQqb::TColumn{
            .Data = reinterpret_cast<char*>(ids2.data()), .Mask = nullptr},
        NQqb::TColumn{
            .Data = data2.data(), .Mask = nullptr,
            .Offsets = offsets2.data(), .OffsetWidth = 4},
        NQqb::TColumn{
            .Data = reinterpret_cast<char*>(values2.data()), .Mask = nullptr},
    };
    NQqb::TRowSet batch2{
        .Columns = columns2.data(), .ColumnCount = 3, .RowCount = 3,
        .Selection = nullptr, .RefCount = 1};

    THashTable table;
    ASSERT_NE(dispatch(&table, &batch1, 2, 0), 0);
    ASSERT_EQ(dispatch(&table, &batch1, 0, 1), 0);
    ASSERT_EQ(dispatch(&table, &batch2, 0, 1), 0);
    ASSERT_EQ(table.Size, 3);
    ASSERT_EQ(table.Capacity, 4);
    ASSERT_EQ(table.OwnedBlockCount, 3);

    struct TStoredKey {
        int64_t Id;
        NQqb::TOwnedString Name;
    };
    static_assert(sizeof(TStoredKey) == 24);
    const std::map<std::pair<int64_t, std::string>,
        std::pair<int64_t, int64_t>> expected = {
        {{1, "alpha"}, {3, 22}},
        {{2, "beta"}, {2, 17}},
        {{4, "delta"}, {1, 11}},
    };
    auto* groupKeys = reinterpret_cast<TStoredKey*>(table.GroupKeys);
    for (int64_t slot = 0; slot < table.Size; ++slot) {
        std::string name(reinterpret_cast<char*>(groupKeys[slot].Name.Data),
            groupKeys[slot].Name.Size);
        auto it = expected.find({groupKeys[slot].Id, name});
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(table.AggBuffers[0][slot], it->second.first);
        EXPECT_EQ(table.AggBuffers[1][slot], it->second.second);
    }

    EXPECT_EQ(dispatch(&table, &batch2, 0, 2), 1);
}

TEST(DualKeyUpsert, MeasuresFixedAndStringOutputBytes) {
    using namespace NQumir;
    using namespace NQumir::NAst;
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({
        {"id", i64Type}, {"name", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
        input, {"id", "name"});

    auto module = std::make_shared<NRegistry::QumirDbModule>();
    TTypePtr hashTableType;
    for (const auto& type : module->ExternalTypes()) {
        if (type.Name == "HashTable") {
            hashTableType = type.Type;
        }
    }
    auto program = NQqb::NKernel::BuildGenericAggregateMeasureProgramAst(
        key, hashTableType);
    ASSERT_TRUE(program) << program.error().ToString();
    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    runner.RegisterModule(module, true);
    std::string error;
    void* entry = runner.CompileKernelAst(
        *program, "agg_measure_keys", &error);
    ASSERT_NE(entry, nullptr) << error;

    struct TStoredKey {
        int64_t Id;
        NQqb::TOwnedString Name;
    };
    std::string first = "alpha";
    std::string second = "";
    std::string third = "мир";
    std::array<TStoredKey, 3> keys = {{
        {1, {reinterpret_cast<uint8_t*>(first.data()),
             static_cast<int64_t>(first.size())}},
        {2, {reinterpret_cast<uint8_t*>(second.data()),
             static_cast<int64_t>(second.size())}},
        {3, {reinterpret_cast<uint8_t*>(third.data()),
             static_cast<int64_t>(third.size())}},
    }};
    THashTable table{
        .GroupKeys = reinterpret_cast<uint8_t*>(keys.data()),
        .Size = static_cast<int64_t>(keys.size()),
    };
    std::array<int64_t, 2> bytes = {-1, -1};
    auto measure = reinterpret_cast<int64_t(*)(
        THashTable*, int64_t*, int64_t)>(entry);
    EXPECT_EQ(measure(&table, bytes.data(), 2), -1);
    ASSERT_EQ(measure(&table, bytes.data(), keys.size()), 3);
    EXPECT_EQ(bytes[0], 3 * static_cast<int64_t>(sizeof(int64_t)));
    EXPECT_EQ(bytes[1], static_cast<int64_t>(
        first.size() + second.size() + third.size()));
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

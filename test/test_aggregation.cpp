#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <qdb/modules/qumirdb.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path KernelDir = "qdb/kernel/aggregation";

std::string ReadKernel(const std::string& name) {
    const auto path = KernelDir / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open aggregation kernel: " + path.string());
    }
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

std::unique_ptr<NQumir::TLLVMRunner> CompileKernel(
    const std::string& name,
    void*& entry,
    std::string& error)
{
    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.ResolveCoreInput = true;
    options.AllowOverloads = true;
    options.NativeCode = true;

    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    entry = runner->CompileKernel(ReadKernel(name), &error);
    return runner;
}

struct THashTable {
    int64_t* Keys = nullptr;
    int64_t* Dist = nullptr;
    int64_t* SlotId = nullptr;
    int64_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    int64_t* Scratch = nullptr;
    int64_t* Scratch2 = nullptr;
    int64_t* QueryKey = nullptr;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
};

static_assert(sizeof(THashTable) == 96);

uint64_t HashI64(int64_t key) {
    uint64_t h = static_cast<uint64_t>(key);
    h ^= h >> 12;
    h ^= h << 25;
    h ^= h >> 27;
    return h * UINT64_C(2685821657736338717);
}

std::vector<int64_t> KeysForHome(int64_t home, int64_t capacity, size_t count) {
    std::vector<int64_t> result;
    for (int64_t key = 0; result.size() < count; ++key) {
        if (static_cast<int64_t>(HashI64(key) & static_cast<uint64_t>(capacity - 1)) == home) {
            result.push_back(key);
        }
    }
    return result;
}

} // namespace

TEST(AggregationKernel, GenericKeyDispatchInstantiatesI64) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_key_smoke.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TSmokeFn = int64_t(*)(int64_t);
    auto smoke = reinterpret_cast<TSmokeFn>(entry);
    EXPECT_EQ(smoke(0), 0);
    EXPECT_EQ(smoke(42), 42);
    EXPECT_EQ(smoke(-17), -17);
}

TEST(AggregationKernel, GenericKeyEqualityDispatchesToI64Overload) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_key_equal.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TEqualFn = bool(*)(int64_t, int64_t);
    auto equal = reinterpret_cast<TEqualFn>(entry);
    EXPECT_TRUE(equal(0, 0));
    EXPECT_TRUE(equal(-17, -17));
    EXPECT_FALSE(equal(1, -1));
    EXPECT_FALSE(equal(42, 43));
}

TEST(AggregationKernel, GenericKeyPointerReadWriteAndSwap) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_key_memory.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TMemoryFn = int64_t(*)(int64_t*, int64_t, int64_t, int64_t);
    auto mutate = reinterpret_cast<TMemoryFn>(entry);
    int64_t keys[] = {10, 20, 30};

    EXPECT_EQ(mutate(keys, 0, 2, 99), 30);
    EXPECT_EQ(keys[0], 30);
    EXPECT_EQ(keys[1], 20);
    EXPECT_EQ(keys[2], 99);
}

TEST(AggregationKernel, ConcreteI64PointerWriteControl) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("concrete_key_memory.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TMemoryFn = int64_t(*)(int64_t*, int64_t, int64_t);
    auto write = reinterpret_cast<TMemoryFn>(entry);
    int64_t keys[] = {10, 20, 30};

    EXPECT_EQ(write(keys, 1, 77), 77);
    EXPECT_EQ(keys[0], 10);
    EXPECT_EQ(keys[1], 77);
    EXPECT_EQ(keys[2], 30);
}

TEST(AggregationKernel, I64HashHasStableBoundaryVectors) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("i64_hash.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using THashFn = int64_t(*)(int64_t);
    auto hash = reinterpret_cast<THashFn>(entry);
    EXPECT_EQ(hash(0), 0);
    EXPECT_EQ(hash(1), 5180492295206395165LL);
    EXPECT_EQ(hash(-1), -491796270583644160LL);
    EXPECT_EQ(hash(std::numeric_limits<int64_t>::min()), -1079387622448562176LL);
    EXPECT_EQ(hash(std::numeric_limits<int64_t>::max()), -245898135291822080LL);
}

TEST(AggregationKernel, HomeSlotAndNextIndexUsePowerOfTwoMask) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("slot_math.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TSlotMathFn = int64_t(*)(int64_t, int64_t, int64_t);
    auto slotMath = reinterpret_cast<TSlotMathFn>(entry);
    constexpr std::array<int64_t, 4> capacities = {4, 8, 16, 32};
    constexpr std::array<int64_t, 5> hashes = {
        0, 1, -1, 17, std::numeric_limits<int64_t>::min()};
    for (int64_t capacity : capacities) {
        for (int64_t hash : hashes) {
            const auto home = slotMath(hash, capacity, 0);
            EXPECT_GE(home, 0);
            EXPECT_LT(home, capacity);
            EXPECT_EQ(home, hash & (capacity - 1));
        }
        EXPECT_EQ(slotMath(capacity - 2, capacity, 1), capacity - 1);
        EXPECT_EQ(slotMath(capacity - 1, capacity, 1), 0);
    }
}

TEST(AggregationKernel, ReadOnlyRobinHoodLookup) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("lookup.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TLookupFn = int64_t(*)(int64_t*, int64_t*, int64_t, int64_t);
    auto lookup = reinterpret_cast<TLookupFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    dist.fill(-1);

    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, 42), -1);

    const auto home3 = KeysForHome(3, capacity, 3);
    keys[3] = home3[0];
    dist[3] = 0;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home3[0]), 3);

    keys[4] = home3[1];
    dist[4] = 1;
    keys[5] = home3[2];
    dist[5] = 2;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home3[2]), 5);

    keys.fill(0);
    dist.fill(-1);
    const auto home7 = KeysForHome(7, capacity, 2);
    keys[7] = home7[0];
    dist[7] = 0;
    keys[0] = home7[1];
    dist[0] = 1;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home7[1]), 0);

    keys.fill(0);
    dist.fill(-1);
    const auto home2 = KeysForHome(2, capacity, 2);
    const auto home3Single = KeysForHome(3, capacity, 1);
    keys[2] = home2[0];
    dist[2] = 0;
    keys[3] = home3Single[0];
    dist[3] = 0;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home2[1]), -1);

    for (int64_t i = 0; i < capacity; ++i) {
        keys[i] = i + 1000;
        dist[i] = capacity;
    }
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, -999), -1);
}

TEST(AggregationKernel, FixedCapacityRobinHoodInsertion) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("insert_fixed.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TInsertFn = int64_t(*)(int64_t*, int64_t*, int64_t, int64_t);
    auto insert = reinterpret_cast<TInsertFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    dist.fill(-1);

    const auto home4 = KeysForHome(4, capacity, 3);
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[0]), 4);
    EXPECT_EQ(keys[4], home4[0]);
    EXPECT_EQ(dist[4], 0);

    const auto beforeKeys = keys;
    const auto beforeDist = dist;
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[0]), 4);
    EXPECT_EQ(keys, beforeKeys);
    EXPECT_EQ(dist, beforeDist);

    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[1]), 5);
    EXPECT_EQ(keys[5], home4[1]);
    EXPECT_EQ(dist[5], 1);

    keys.fill(0);
    dist.fill(-1);
    const auto home5 = KeysForHome(5, capacity, 1);
    const auto home6 = KeysForHome(6, capacity, 1);
    keys[4] = home4[0];
    dist[4] = 0;
    keys[5] = home5[0];
    dist[5] = 0;
    keys[6] = home6[0];
    dist[6] = 0;
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[1]), 5);
    EXPECT_EQ(keys[4], home4[0]);
    EXPECT_EQ(keys[5], home4[1]);
    EXPECT_EQ(keys[6], home5[0]);
    EXPECT_EQ(keys[7], home6[0]);
    EXPECT_EQ(dist[4], 0);
    EXPECT_EQ(dist[5], 1);
    EXPECT_EQ(dist[6], 1);
    EXPECT_EQ(dist[7], 1);

    keys.fill(0);
    dist.fill(-1);
    const auto home7 = KeysForHome(7, capacity, 2);
    const auto home0 = KeysForHome(0, capacity, 1);
    keys[7] = home7[0];
    dist[7] = 0;
    keys[0] = home0[0];
    dist[0] = 0;
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home7[1]), 0);
    EXPECT_EQ(keys[7], home7[0]);
    EXPECT_EQ(keys[0], home7[1]);
    EXPECT_EQ(keys[1], home0[0]);
    EXPECT_EQ(dist[0], 1);
    EXPECT_EQ(dist[1], 1);

    for (int64_t i = 0; i < capacity; ++i) {
        keys[i] = i;
        dist[i] = capacity;
    }
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, 123456), -1);
}

TEST(AggregationKernel, FixedCapacityTableMaintainsInvariants) {
    void* insertEntry = nullptr;
    std::string insertError;
    auto insertRunner = CompileKernel("insert_fixed.oz", insertEntry, insertError);
    ASSERT_NE(insertEntry, nullptr) << insertError;

    void* checkEntry = nullptr;
    std::string checkError;
    auto checkRunner = CompileKernel("check_invariants.oz", checkEntry, checkError);
    ASSERT_NE(checkEntry, nullptr) << checkError;

    using TInsertFn = int64_t(*)(int64_t*, int64_t*, int64_t, int64_t);
    using TCheckFn = bool(*)(int64_t*, int64_t*, int64_t);
    auto insert = reinterpret_cast<TInsertFn>(insertEntry);
    auto check = reinterpret_cast<TCheckFn>(checkEntry);
    constexpr int64_t capacity = 16;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    dist.fill(-1);

    constexpr std::array<int64_t, 12> input = {
        0, 1, -1, 17, 33, 49, 65, 81, 97, 113, 129, 145};
    EXPECT_TRUE(check(keys.data(), dist.data(), capacity));
    for (int64_t key : input) {
        ASSERT_NE(insert(keys.data(), dist.data(), capacity, key), -1);
        EXPECT_TRUE(check(keys.data(), dist.data(), capacity)) << "after key " << key;
    }

    auto corrupted = dist;
    for (int64_t i = 0; i < capacity; ++i) {
        if (corrupted[i] >= 0) {
            ++corrupted[i];
            break;
        }
    }
    EXPECT_FALSE(check(keys.data(), corrupted.data(), capacity));
}

TEST(AggregationKernel, StableDenseSlotIdsSurviveDisplacement) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("insert_slot_id.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TInsertFn = int64_t(*)(
        int64_t*, int64_t*, int64_t*, int64_t, int64_t*, int64_t);
    auto insert = reinterpret_cast<TInsertFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    std::array<int64_t, capacity> slotIds{};
    dist.fill(-1);
    slotIds.fill(-1);
    int64_t size = 0;

    const auto home4 = KeysForHome(4, capacity, 2);
    const auto home5 = KeysForHome(5, capacity, 1);
    const auto home6 = KeysForHome(6, capacity, 1);
    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home4[0]), 0);
    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home5[0]), 1);
    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home6[0]), 2);
    EXPECT_EQ(size, 3);

    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home4[1]), 3);
    EXPECT_EQ(size, 4);
    EXPECT_EQ(keys[4], home4[0]);
    EXPECT_EQ(slotIds[4], 0);
    EXPECT_EQ(keys[5], home4[1]);
    EXPECT_EQ(slotIds[5], 3);
    EXPECT_EQ(keys[6], home5[0]);
    EXPECT_EQ(slotIds[6], 1);
    EXPECT_EQ(keys[7], home6[0]);
    EXPECT_EQ(slotIds[7], 2);

    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home5[0]), 1);
    EXPECT_EQ(size, 4);

    std::array<bool, capacity> seen{};
    for (int64_t i = 0; i < capacity; ++i) {
        if (dist[i] >= 0) {
            ASSERT_GE(slotIds[i], 0);
            ASSERT_LT(slotIds[i], size);
            EXPECT_FALSE(seen[slotIds[i]]);
            seen[slotIds[i]] = true;
        }
    }
    for (int64_t slot = 0; slot < size; ++slot) {
        EXPECT_TRUE(seen[slot]);
    }
}

TEST(AggregationKernel, OzTableLifecycleAllocatesInitializesAndDestroys) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("table_lifecycle.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TLifecycleFn = int64_t(*)(THashTable*, int64_t, int64_t);
    auto lifecycle = reinterpret_cast<TLifecycleFn>(entry);
    THashTable table;

    ASSERT_TRUE(lifecycle(&table, 8, 0));
    ASSERT_NE(table.Keys, nullptr);
    ASSERT_NE(table.Dist, nullptr);
    ASSERT_NE(table.SlotId, nullptr);
    EXPECT_EQ(table.Capacity, 8);
    EXPECT_EQ(table.Size, 0);
    EXPECT_EQ(table.NumKeys, 1);
    for (int64_t i = 0; i < table.Capacity; ++i) {
        EXPECT_EQ(table.Keys[i], 0);
        EXPECT_EQ(table.Dist[i], -1);
        EXPECT_EQ(table.SlotId[i], -1);
    }

    EXPECT_TRUE(lifecycle(&table, 0, 1));
    EXPECT_EQ(table.Keys, nullptr);
    EXPECT_EQ(table.Dist, nullptr);
    EXPECT_EQ(table.SlotId, nullptr);
    EXPECT_EQ(table.Capacity, 0);
    EXPECT_EQ(table.Size, 0);
}

TEST(AggregationKernel, OzTableGrowsAndPreservesStableSlotIds) {
    void* lifecycleEntry = nullptr;
    void* growEntry = nullptr;
    void* checkEntry = nullptr;
    std::string error;
    auto lifecycleRunner = CompileKernel("table_lifecycle.oz", lifecycleEntry, error);
    ASSERT_NE(lifecycleEntry, nullptr) << error;
    auto growRunner = CompileKernel("table_grow.oz", growEntry, error);
    ASSERT_NE(growEntry, nullptr) << error;
    auto checkRunner = CompileKernel("check_invariants.oz", checkEntry, error);
    ASSERT_NE(checkEntry, nullptr) << error;

    using TLifecycleFn = int64_t(*)(THashTable*, int64_t, int64_t);
    using TGrowFn = int64_t(*)(THashTable*, int64_t, int64_t);
    using TCheckFn = bool(*)(int64_t*, int64_t*, int64_t);
    auto lifecycle = reinterpret_cast<TLifecycleFn>(lifecycleEntry);
    auto insertOrLookup = reinterpret_cast<TGrowFn>(growEntry);
    auto check = reinterpret_cast<TCheckFn>(checkEntry);

    THashTable table;
    ASSERT_TRUE(lifecycle(&table, 4, 0));
    constexpr std::array<int64_t, 20> input = {
        0, 1, -1, 17, 33, 49, 65, 81, 97, 113,
        129, 145, 161, 177, 193, 209, 225, 241, 257, 273};
    constexpr std::array<int64_t, 4> expectedCapacities = {4, 8, 16, 32};
    size_t capacityIndex = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        const int64_t oldCapacity = table.Capacity;
        EXPECT_EQ(insertOrLookup(&table, input[i], 0), static_cast<int64_t>(i));
        ASSERT_EQ(table.Size, static_cast<int64_t>(i + 1));
        if (table.Capacity != oldCapacity) {
            ++capacityIndex;
            ASSERT_LT(capacityIndex, expectedCapacities.size());
            EXPECT_EQ(table.Capacity, expectedCapacities[capacityIndex]);
        }
        EXPECT_TRUE(check(table.Keys, table.Dist, table.Capacity));
        for (size_t keyIndex = 0; keyIndex <= i; ++keyIndex) {
            EXPECT_EQ(insertOrLookup(&table, input[keyIndex], 1),
                      static_cast<int64_t>(keyIndex));
        }
    }

    EXPECT_EQ(capacityIndex, expectedCapacities.size() - 1);
    const int64_t sizeBeforeDuplicate = table.Size;
    EXPECT_EQ(insertOrLookup(&table, input[3], 0), 3);
    EXPECT_EQ(table.Size, sizeBeforeDuplicate);
    EXPECT_EQ(table.Capacity, 32);

    std::array<bool, input.size()> seen{};
    for (int64_t i = 0; i < table.Capacity; ++i) {
        if (table.Dist[i] >= 0) {
            ASSERT_GE(table.SlotId[i], 0);
            ASSERT_LT(table.SlotId[i], table.Size);
            EXPECT_FALSE(seen[table.SlotId[i]]);
            seen[table.SlotId[i]] = true;
        }
    }
    for (bool present : seen) {
        EXPECT_TRUE(present);
    }

    EXPECT_TRUE(lifecycle(&table, 0, 1));
}

int main(int argc, char** argv) {
    if (argc > 1) {
        KernelDir = argv[1];
    }
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}

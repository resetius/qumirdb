#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <qdb/modules/qumirdb.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

struct TPairI64Key {
    int64_t First;
    int64_t Second;

    bool operator==(const TPairI64Key&) const = default;
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

TEST(AggregationKernel, GenericHashDispatchesToConcretePairOverload) {
    void* i64Entry = nullptr;
    void* pairEntry = nullptr;
    std::string error;
    auto i64Runner = CompileKernel("generic_dispatch_i64.oz", i64Entry, error);
    ASSERT_NE(i64Entry, nullptr) << error;
    auto pairRunner = CompileKernel("generic_dispatch_pair_i64.oz", pairEntry, error);
    ASSERT_NE(pairEntry, nullptr) << error;

    using TI64DispatchFn = int64_t(*)(int64_t);
    using TPairDispatchFn = int64_t(*)(TPairI64Key);
    auto dispatchI64 = reinterpret_cast<TI64DispatchFn>(i64Entry);
    auto dispatchPair = reinterpret_cast<TPairDispatchFn>(pairEntry);
    EXPECT_EQ(dispatchI64(7), 1007);
    EXPECT_EQ(dispatchPair({7, 13}), 713);
}

TEST(AggregationKernel, GenericPairRobinHoodHandlesCollisionsDuplicatesAndReducers) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_pair_fixed.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TPairBatchFn = int64_t(*)(
        TPairI64Key*, int64_t*, int64_t*, int64_t, int64_t*, TPairI64Key*,
        int64_t*, int64_t*, TPairI64Key*, int64_t*, int64_t);
    auto process = reinterpret_cast<TPairBatchFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<TPairI64Key, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    std::array<int64_t, capacity> slotIds{};
    std::array<TPairI64Key, capacity> groupKeys{};
    std::array<int64_t, capacity> counts{};
    std::array<int64_t, capacity> sums{};
    dist.fill(-1);
    slotIds.fill(-1);
    int64_t size = 0;

    std::array<TPairI64Key, 7> input = {
        TPairI64Key{1, 10}, {2, 20}, {1, 10}, {3, 30}, {4, 40}, {2, 20}, {5, 50}};
    std::array<int64_t, 7> values = {5, 7, 11, 13, 17, -2, 19};
    ASSERT_EQ(process(keys.data(), dist.data(), slotIds.data(), capacity, &size,
                      groupKeys.data(), counts.data(), sums.data(), input.data(),
                      values.data(), input.size()), 5);

    EXPECT_EQ(size, 5);
    EXPECT_EQ(groupKeys[0], (TPairI64Key{1, 10}));
    EXPECT_EQ(groupKeys[1], (TPairI64Key{2, 20}));
    EXPECT_EQ(groupKeys[2], (TPairI64Key{3, 30}));
    EXPECT_EQ(groupKeys[3], (TPairI64Key{4, 40}));
    EXPECT_EQ(groupKeys[4], (TPairI64Key{5, 50}));
    EXPECT_EQ((std::array<int64_t, 5>{counts[0], counts[1], counts[2], counts[3], counts[4]}),
              (std::array<int64_t, 5>{2, 2, 1, 1, 1}));
    EXPECT_EQ((std::array<int64_t, 5>{sums[0], sums[1], sums[2], sums[3], sums[4]}),
              (std::array<int64_t, 5>{16, 5, 13, 17, 19}));

    std::array<bool, capacity> seen{};
    for (int64_t slot = 0; slot < capacity; ++slot) {
        if (dist[slot] >= 0) {
            ASSERT_GE(slotIds[slot], 0);
            ASSERT_LT(slotIds[slot], size);
            EXPECT_FALSE(seen[slotIds[slot]]);
            seen[slotIds[slot]] = true;
        }
    }
    for (int64_t slot = 0; slot < size; ++slot) {
        EXPECT_TRUE(seen[slot]);
    }
}

TEST(AggregationKernel, GenericPairRehashPreservesSlotsAndAggregationState) {
    void* processEntry = nullptr;
    void* rehashEntry = nullptr;
    std::string error;
    auto processRunner = CompileKernel("generic_pair_fixed.oz", processEntry, error);
    ASSERT_NE(processEntry, nullptr) << error;
    auto rehashRunner = CompileKernel("generic_pair_rehash.oz", rehashEntry, error);
    ASSERT_NE(rehashEntry, nullptr) << error;

    using TPairBatchFn = int64_t(*)(
        TPairI64Key*, int64_t*, int64_t*, int64_t, int64_t*, TPairI64Key*,
        int64_t*, int64_t*, TPairI64Key*, int64_t*, int64_t);
    using TPairRehashFn = int64_t(*)(
        TPairI64Key*, int64_t*, int64_t*, int64_t, int64_t, TPairI64Key*,
        int64_t*, int64_t*, TPairI64Key*, int64_t*, int64_t*, int64_t,
        TPairI64Key*, int64_t*, int64_t*);
    auto process = reinterpret_cast<TPairBatchFn>(processEntry);
    auto rehash = reinterpret_cast<TPairRehashFn>(rehashEntry);

    constexpr int64_t oldCapacity = 8;
    constexpr int64_t newCapacity = 16;
    std::array<TPairI64Key, oldCapacity> oldKeys{};
    std::array<int64_t, oldCapacity> oldDist{};
    std::array<int64_t, oldCapacity> oldSlotIds{};
    std::array<TPairI64Key, oldCapacity> oldGroupKeys{};
    std::array<int64_t, oldCapacity> oldCounts{};
    std::array<int64_t, oldCapacity> oldSums{};
    oldDist.fill(-1);
    oldSlotIds.fill(-1);
    int64_t size = 0;
    std::array<TPairI64Key, 5> initialKeys = {
        TPairI64Key{1, 2}, {3, 4}, {5, 6}, {1, 2}, {7, 8}};
    std::array<int64_t, 5> initialValues = {10, 20, 30, 5, 40};
    ASSERT_EQ(process(oldKeys.data(), oldDist.data(), oldSlotIds.data(), oldCapacity,
                      &size, oldGroupKeys.data(), oldCounts.data(), oldSums.data(),
                      initialKeys.data(), initialValues.data(), initialKeys.size()), 4);

    std::array<TPairI64Key, newCapacity> newKeys{};
    std::array<int64_t, newCapacity> newDist{};
    std::array<int64_t, newCapacity> newSlotIds{};
    std::array<TPairI64Key, newCapacity> newGroupKeys{};
    std::array<int64_t, newCapacity> newCounts{};
    std::array<int64_t, newCapacity> newSums{};
    ASSERT_EQ(rehash(oldKeys.data(), oldDist.data(), oldSlotIds.data(), oldCapacity,
                     size, oldGroupKeys.data(), oldCounts.data(), oldSums.data(),
                     newKeys.data(), newDist.data(), newSlotIds.data(), newCapacity,
                     newGroupKeys.data(), newCounts.data(), newSums.data()), size);

    EXPECT_EQ(newGroupKeys[0], (TPairI64Key{1, 2}));
    EXPECT_EQ(newGroupKeys[1], (TPairI64Key{3, 4}));
    EXPECT_EQ(newGroupKeys[2], (TPairI64Key{5, 6}));
    EXPECT_EQ(newGroupKeys[3], (TPairI64Key{7, 8}));
    EXPECT_EQ((std::array<int64_t, 4>{newCounts[0], newCounts[1], newCounts[2], newCounts[3]}),
              (std::array<int64_t, 4>{2, 1, 1, 1}));
    EXPECT_EQ((std::array<int64_t, 4>{newSums[0], newSums[1], newSums[2], newSums[3]}),
              (std::array<int64_t, 4>{15, 20, 30, 40}));

    std::array<TPairI64Key, 3> moreKeys = {
        TPairI64Key{3, 4}, {9, 10}, {11, 12}};
    std::array<int64_t, 3> moreValues = {-7, 50, 60};
    ASSERT_EQ(process(newKeys.data(), newDist.data(), newSlotIds.data(), newCapacity,
                      &size, newGroupKeys.data(), newCounts.data(), newSums.data(),
                      moreKeys.data(), moreValues.data(), moreKeys.size()), 6);
    EXPECT_EQ(newGroupKeys[4], (TPairI64Key{9, 10}));
    EXPECT_EQ(newGroupKeys[5], (TPairI64Key{11, 12}));
    EXPECT_EQ(newCounts[1], 2);
    EXPECT_EQ(newSums[1], 13);
    EXPECT_EQ(newCounts[4], 1);
    EXPECT_EQ(newSums[4], 50);

    std::array<bool, newCapacity> seen{};
    for (int64_t slot = 0; slot < newCapacity; ++slot) {
        if (newDist[slot] >= 0) {
            ASSERT_GE(newSlotIds[slot], 0);
            ASSERT_LT(newSlotIds[slot], size);
            EXPECT_FALSE(seen[newSlotIds[slot]]);
            seen[newSlotIds[slot]] = true;
        }
    }
    for (int64_t slot = 0; slot < size; ++slot) {
        EXPECT_TRUE(seen[slot]);
    }
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

TEST(AggregationKernel, OzTableLifecycleRejectsAllocationSizeOverflow) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("table_lifecycle.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TLifecycleFn = int64_t(*)(THashTable*, int64_t, int64_t);
    auto lifecycle = reinterpret_cast<TLifecycleFn>(entry);
    THashTable table;
    EXPECT_FALSE(lifecycle(&table, INT64_C(1152921504606846976), 0));
    EXPECT_EQ(table.Keys, nullptr);
    EXPECT_EQ(table.Capacity, 0);
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

TEST(AggregationKernel, OzTableRehashRejectsAllocationSizeOverflow) {
    void* lifecycleEntry = nullptr;
    void* growEntry = nullptr;
    std::string error;
    auto lifecycleRunner = CompileKernel("table_lifecycle.oz", lifecycleEntry, error);
    ASSERT_NE(lifecycleEntry, nullptr) << error;
    auto growRunner = CompileKernel("table_grow.oz", growEntry, error);
    ASSERT_NE(growEntry, nullptr) << error;

    using TLifecycleFn = int64_t(*)(THashTable*, int64_t, int64_t);
    using TGrowFn = int64_t(*)(THashTable*, int64_t, int64_t);
    auto lifecycle = reinterpret_cast<TLifecycleFn>(lifecycleEntry);
    auto grow = reinterpret_cast<TGrowFn>(growEntry);
    THashTable table;
    ASSERT_TRUE(lifecycle(&table, 4, 0));
    int64_t* const oldKeys = table.Keys;
    EXPECT_FALSE(grow(&table, INT64_C(1152921504606846976), 2));
    EXPECT_EQ(table.Keys, oldKeys);
    EXPECT_EQ(table.Capacity, 4);
    EXPECT_EQ(table.Size, 0);
    EXPECT_TRUE(lifecycle(&table, 0, 1));
}

TEST(AggregationKernel, OzI64ReducersUseStableDenseSlotsAcrossGrow) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("count.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TCountFn = int64_t(*)(THashTable*, int64_t*, int64_t*, int64_t, int64_t);
    auto count = reinterpret_cast<TCountFn>(entry);
    THashTable table;

    ASSERT_TRUE(count(&table, nullptr, nullptr, 4, 0));
    ASSERT_NE(table.GroupKeys, nullptr);
    ASSERT_NE(table.AggBuffers, nullptr);
    ASSERT_NE(table.AggBuffers[0], nullptr);
    ASSERT_NE(table.AggBuffers[1], nullptr);
    ASSERT_NE(table.AggBuffers[2], nullptr);
    ASSERT_NE(table.AggBuffers[3], nullptr);
    EXPECT_EQ(table.NumAggs, 4);
    EXPECT_EQ(table.Size, 0);
    EXPECT_EQ(count(&table, nullptr, nullptr, 999, 2), -1);

    constexpr std::array<int64_t, 22> input = {
        10, 20, 10, -1, 30, 20, 40, 50, 10, 60,
        70, 80, 90, 100, 110, 120, -1, 50, 120, 120, 130, 140};
    constexpr std::array<int64_t, 22> values = {
        5, 7, -2, 11, 13, 17, 19, 23, 29, 31,
        37, 41, 43, 47, 53, 59, -3, -5, 61, -7,
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()};
    constexpr std::array<int64_t, 15> uniqueKeys = {
        10, 20, -1, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140};
    constexpr std::array<int64_t, 15> expectedCounts = {
        3, 2, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 3, 1, 1};
    constexpr std::array<int64_t, 15> expectedSums = {
        32, 24, 8, 13, 19, 18, 31, 37, 41, 43, 47, 53, 113,
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()};
    constexpr std::array<int64_t, 15> expectedMins = {
        -2, 7, -3, 13, 19, -5, 31, 37, 41, 43, 47, 53, -7,
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()};
    constexpr std::array<int64_t, 15> expectedMaxs = {
        29, 17, 11, 13, 19, 23, 31, 37, 41, 43, 47, 53, 61,
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()};

    ASSERT_TRUE(count(&table, const_cast<int64_t*>(input.data()),
                      const_cast<int64_t*>(values.data()), input.size(), 1));

    EXPECT_EQ(table.Capacity, 32);
    ASSERT_EQ(table.Size, static_cast<int64_t>(uniqueKeys.size()));
    for (size_t i = 0; i < uniqueKeys.size(); ++i) {
        EXPECT_EQ(table.GroupKeys[i], uniqueKeys[i]);
        EXPECT_EQ(table.AggBuffers[0][i], expectedCounts[i]);
        EXPECT_EQ(table.AggBuffers[1][i], expectedSums[i]);
        EXPECT_EQ(table.AggBuffers[2][i], expectedMins[i]);
        EXPECT_EQ(table.AggBuffers[3][i], expectedMaxs[i]);
        EXPECT_EQ(count(&table, nullptr, nullptr, uniqueKeys[i], 2), expectedCounts[i]);
        EXPECT_EQ(count(&table, nullptr, nullptr, uniqueKeys[i], 3), expectedSums[i]);
        EXPECT_EQ(count(&table, nullptr, nullptr, uniqueKeys[i], 4), expectedMins[i]);
        EXPECT_EQ(count(&table, nullptr, nullptr, uniqueKeys[i], 5), expectedMaxs[i]);
    }

    EXPECT_TRUE(count(&table, nullptr, nullptr, 0, 6));
    EXPECT_EQ(table.Keys, nullptr);
    EXPECT_EQ(table.GroupKeys, nullptr);
    EXPECT_EQ(table.AggBuffers, nullptr);
    EXPECT_EQ(table.Capacity, 0);
    EXPECT_EQ(table.Size, 0);
}

TEST(AggregationKernel, OzBatchAggregationHandlesEmptySingleGroupAndUniqueInputs) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("count.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TBatchFn = int64_t(*)(THashTable*, int64_t*, int64_t*, int64_t, int64_t);
    auto aggregate = reinterpret_cast<TBatchFn>(entry);

    {
        THashTable table;
        ASSERT_TRUE(aggregate(&table, nullptr, nullptr, 4, 0));
        EXPECT_TRUE(aggregate(&table, nullptr, nullptr, 0, 1));
        EXPECT_EQ(table.Size, 0);
        EXPECT_FALSE(aggregate(&table, nullptr, nullptr, -1, 1));
        EXPECT_EQ(table.Size, 0);
        EXPECT_TRUE(aggregate(&table, nullptr, nullptr, 0, 6));
    }

    {
        THashTable table;
        std::array<int64_t, 4> keys = {7, 7, 7, 7};
        std::array<int64_t, 4> values = {5, -2, 11, 3};
        ASSERT_TRUE(aggregate(&table, nullptr, nullptr, 4, 0));
        ASSERT_TRUE(aggregate(&table, keys.data(), values.data(), keys.size(), 1));
        ASSERT_EQ(table.Size, 1);
        EXPECT_EQ(table.GroupKeys[0], 7);
        EXPECT_EQ(table.AggBuffers[0][0], 4);
        EXPECT_EQ(table.AggBuffers[1][0], 17);
        EXPECT_EQ(table.AggBuffers[2][0], -2);
        EXPECT_EQ(table.AggBuffers[3][0], 11);
        EXPECT_TRUE(aggregate(&table, nullptr, nullptr, 0, 6));
    }

    {
        THashTable table;
        std::array<int64_t, 8> keys = {3, 5, 7, 9, 11, 13, 15, 17};
        std::array<int64_t, 8> values = {30, 50, 70, 90, 110, 130, 150, 170};
        ASSERT_TRUE(aggregate(&table, nullptr, nullptr, 4, 0));
        ASSERT_TRUE(aggregate(&table, keys.data(), values.data(), keys.size(), 1));
        EXPECT_EQ(table.Size, 8);
        EXPECT_EQ(table.Capacity, 16);
        for (size_t i = 0; i < keys.size(); ++i) {
            EXPECT_EQ(table.GroupKeys[i], keys[i]);
            EXPECT_EQ(table.AggBuffers[0][i], 1);
            EXPECT_EQ(table.AggBuffers[1][i], values[i]);
            EXPECT_EQ(table.AggBuffers[2][i], values[i]);
            EXPECT_EQ(table.AggBuffers[3][i], values[i]);
        }
        EXPECT_TRUE(aggregate(&table, nullptr, nullptr, 0, 6));
    }
}

TEST(AggregationKernel, OzFinalizeCopiesDenseKeysAndAggregateBuffers) {
    void* aggregateEntry = nullptr;
    void* finalizeEntry = nullptr;
    std::string error;
    auto aggregateRunner = CompileKernel("count.oz", aggregateEntry, error);
    ASSERT_NE(aggregateEntry, nullptr) << error;
    auto finalizeRunner = CompileKernel("finalize.oz", finalizeEntry, error);
    ASSERT_NE(finalizeEntry, nullptr) << error;

    using TBatchFn = int64_t(*)(THashTable*, int64_t*, int64_t*, int64_t, int64_t);
    using TFinalizeFn = int64_t(*)(
        THashTable*, int64_t*, int64_t*, int64_t*, int64_t*, int64_t*, int64_t);
    auto aggregate = reinterpret_cast<TBatchFn>(aggregateEntry);
    auto finalize = reinterpret_cast<TFinalizeFn>(finalizeEntry);

    THashTable table;
    std::array<int64_t, 6> keys = {4, 2, 4, 8, 2, 4};
    std::array<int64_t, 6> values = {10, 7, -3, 11, 5, 20};
    ASSERT_TRUE(aggregate(&table, nullptr, nullptr, 4, 0));
    ASSERT_TRUE(aggregate(&table, keys.data(), values.data(), keys.size(), 1));
    ASSERT_EQ(table.Size, 3);

    std::array<int64_t, 3> outputKeys = {-1, -1, -1};
    std::array<int64_t, 3> outputCounts = {-1, -1, -1};
    std::array<int64_t, 3> outputSums = {-1, -1, -1};
    std::array<int64_t, 3> outputMins = {-1, -1, -1};
    std::array<int64_t, 3> outputMaxs = {-1, -1, -1};

    EXPECT_EQ(finalize(&table, outputKeys.data(), outputCounts.data(),
                       outputSums.data(), outputMins.data(), outputMaxs.data(), 2), -1);
    EXPECT_EQ(outputKeys[0], -1);
    ASSERT_EQ(finalize(&table, outputKeys.data(), outputCounts.data(),
                       outputSums.data(), outputMins.data(), outputMaxs.data(), 3), 3);
    EXPECT_EQ(outputKeys, (std::array<int64_t, 3>{4, 2, 8}));
    EXPECT_EQ(outputCounts, (std::array<int64_t, 3>{3, 2, 1}));
    EXPECT_EQ(outputSums, (std::array<int64_t, 3>{27, 12, 11}));
    EXPECT_EQ(outputMins, (std::array<int64_t, 3>{-3, 5, 11}));
    EXPECT_EQ(outputMaxs, (std::array<int64_t, 3>{20, 7, 11}));

    EXPECT_TRUE(aggregate(&table, nullptr, nullptr, 0, 6));
}

TEST(AggregationKernel, OzStressAggregationHandlesLargeDeterministicInput) {
    void* countEntry = nullptr;
    void* finalizeEntry = nullptr;
    void* checkEntry = nullptr;
    std::string error;
    auto countRunner = CompileKernel("count.oz", countEntry, error);
    ASSERT_NE(countEntry, nullptr) << error;
    auto finalizeRunner = CompileKernel("finalize.oz", finalizeEntry, error);
    ASSERT_NE(finalizeEntry, nullptr) << error;
    auto checkRunner = CompileKernel("check_invariants.oz", checkEntry, error);
    ASSERT_NE(checkEntry, nullptr) << error;

    using TBatchFn = int64_t(*)(THashTable*, int64_t*, int64_t*, int64_t, int64_t);
    using TFinalizeFn = int64_t(*)(
        THashTable*, int64_t*, int64_t*, int64_t*, int64_t*, int64_t*, int64_t);
    using TCheckFn = bool(*)(int64_t*, int64_t*, int64_t);
    auto aggregate = reinterpret_cast<TBatchFn>(countEntry);
    auto finalize = reinterpret_cast<TFinalizeFn>(finalizeEntry);
    auto check = reinterpret_cast<TCheckFn>(checkEntry);

    struct TGroupStats {
        int64_t Count = 0;
        int64_t Sum = 0;
        int64_t Min = 0;
        int64_t Max = 0;
    };

    constexpr size_t rowCount = 2000;
    std::vector<int64_t> keys(rowCount);
    std::vector<int64_t> values(rowCount);
    std::unordered_map<int64_t, TGroupStats> reference;

    uint64_t state = 88172645463325252ULL;
    auto next = [&state]() -> uint64_t {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    };
    for (size_t i = 0; i < rowCount; ++i) {
        const int64_t key = static_cast<int64_t>(next() % 301) - 150;
        const int64_t value = static_cast<int64_t>(next() % 2001) - 1000;
        keys[i] = key;
        values[i] = value;
        auto& group = reference[key];
        if (group.Count == 0) {
            group.Min = value;
            group.Max = value;
        } else {
            group.Min = std::min(group.Min, value);
            group.Max = std::max(group.Max, value);
        }
        group.Count += 1;
        group.Sum += value;
    }

    THashTable table;
    ASSERT_TRUE(aggregate(&table, nullptr, nullptr, 4, 0));

    constexpr size_t batchCount = 3;
    size_t offset = 0;
    for (size_t batch = 0; batch < batchCount; ++batch) {
        const size_t remaining = rowCount - offset;
        const size_t batchSize =
            (batch + 1 == batchCount) ? remaining : remaining / (batchCount - batch);
        ASSERT_TRUE(aggregate(&table, keys.data() + offset, values.data() + offset,
                              static_cast<int64_t>(batchSize), 1));
        offset += batchSize;
    }
    ASSERT_EQ(offset, rowCount);

    auto dumpTable = [&table]() {
        std::ostringstream out;
        out << "Capacity=" << table.Capacity << " Size=" << table.Size << "\n";
        for (int64_t slot = 0; slot < table.Capacity; ++slot) {
            if (table.Dist[slot] != -1) {
                out << "  slot " << slot << ": key=" << table.Keys[slot]
                    << " dist=" << table.Dist[slot]
                    << " slotId=" << table.SlotId[slot] << "\n";
            }
        }
        return out.str();
    };

    ASSERT_EQ(table.Size, static_cast<int64_t>(reference.size())) << dumpTable();
    ASSERT_GE(table.Capacity, 256) << dumpTable();
    ASSERT_TRUE(check(table.Keys, table.Dist, table.Capacity)) << dumpTable();

    std::vector<bool> seenSlotId(table.Size, false);
    for (int64_t slot = 0; slot < table.Capacity; ++slot) {
        if (table.Dist[slot] != -1) {
            const int64_t slotId = table.SlotId[slot];
            ASSERT_GE(slotId, 0) << dumpTable();
            ASSERT_LT(slotId, table.Size) << dumpTable();
            ASSERT_FALSE(seenSlotId[slotId]) << "duplicate dense slot id " << slotId << "\n"
                                              << dumpTable();
            seenSlotId[slotId] = true;
        }
    }
    for (bool present : seenSlotId) {
        ASSERT_TRUE(present) << dumpTable();
    }

    std::vector<int64_t> outputKeys(table.Size);
    std::vector<int64_t> outputCounts(table.Size);
    std::vector<int64_t> outputSums(table.Size);
    std::vector<int64_t> outputMins(table.Size);
    std::vector<int64_t> outputMaxs(table.Size);
    ASSERT_EQ(finalize(&table, outputKeys.data(), outputCounts.data(), outputSums.data(),
                       outputMins.data(), outputMaxs.data(), table.Size),
              table.Size);

    for (int64_t i = 0; i < table.Size; ++i) {
        const int64_t key = outputKeys[i];
        auto it = reference.find(key);
        ASSERT_NE(it, reference.end()) << "unexpected key " << key << "\n" << dumpTable();
        const auto& group = it->second;
        EXPECT_EQ(outputCounts[i], group.Count) << "key " << key << "\n" << dumpTable();
        EXPECT_EQ(outputSums[i], group.Sum) << "key " << key << "\n" << dumpTable();
        EXPECT_EQ(outputMins[i], group.Min) << "key " << key << "\n" << dumpTable();
        EXPECT_EQ(outputMaxs[i], group.Max) << "key " << key << "\n" << dumpTable();
        reference.erase(it);
    }
    EXPECT_TRUE(reference.empty()) << "missing " << reference.size() << " keys\n" << dumpTable();

    EXPECT_TRUE(aggregate(&table, nullptr, nullptr, 0, 6));
}

int main(int argc, char** argv) {
    if (argc > 1) {
        KernelDir = argv[1];
    }
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}

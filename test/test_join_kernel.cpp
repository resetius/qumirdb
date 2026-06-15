#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace NQqb;

namespace {

// Mirrors the HashTable C layout from modules/qumirdb.cpp. The join reuses it as
// each side's hash map; the dense per-slot RowId bucket lives in AggBuffers:
//   AggBuffers[0][slot] = length, [1] = capacity, [2] = data pointer (as i64).
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
static_assert(sizeof(THashTable) == TKernelCompiler::kHashTableSize);

struct TPairBuffer {
    int64_t Count = 0;
    int64_t Capacity = 0;
    int64_t* Data = nullptr;
};
static_assert(sizeof(TPairBuffer) == TKernelCompiler::kPairBufferSize);

std::unique_ptr<NQumir::TLLVMRunner> CompileJoinEntry(
    const std::string& entryName, void*& entry)
{
    auto lib = NKernel::BuildJoinKernelLibrary();
    if (!lib) {
        ADD_FAILURE() << lib.error().ToString();
        return {};
    }
    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    runner->RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*lib));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

} // namespace

TEST(JoinKernel, PairBufferLayout) {
    NQumir::NRegistry::QumirDbModule module;
    std::shared_ptr<NQumir::NAst::TStructType> pairBuffer;
    bool hasJoinTable = false;
    for (const auto& type : module.ExternalTypes()) {
        if (type.Name == "PairBuffer") {
            pairBuffer = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type.Type).Cast();
        } else if (type.Name == "JoinTable") {
            hasJoinTable = true;
        }
    }
    ASSERT_NE(pairBuffer, nullptr);
    ASSERT_EQ(pairBuffer->Fields.size(), 3u);
    EXPECT_EQ(pairBuffer->Fields[2].first, "Data");
    EXPECT_FALSE(hasJoinTable); // join reuses HashTable, no separate type
}

TEST(JoinKernel, InitReusesHashTableAndDestroyClears) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinEntry("jt_init", initEntry);
    ASSERT_NE(initEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);

    THashTable ht{};
    ASSERT_TRUE(jtInit(&ht, 8, 8));
    EXPECT_EQ(ht.Capacity, 8);
    EXPECT_EQ(ht.Size, 0);
    EXPECT_EQ(ht.NumAggs, 3); // three dense bucket columns
    EXPECT_EQ(ht.KeySize, 8);
    ASSERT_NE(ht.AggBuffers, nullptr);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(ht.Dist[i], -1);
        EXPECT_EQ(ht.AggBuffers[0][i], 0); // length
        EXPECT_EQ(ht.AggBuffers[1][i], 0); // capacity
        EXPECT_EQ(ht.AggBuffers[2][i], 0); // data pointer (null)
    }

    jtDestroy(&ht);
    EXPECT_EQ(ht.Keys, nullptr);
    EXPECT_EQ(ht.AggBuffers, nullptr);
    EXPECT_EQ(ht.Capacity, 0);
}

TEST(JoinKernel, BucketAppendGrowsAndStoresRowIds) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinEntry("jt_init", initEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
    void* appendEntry = nullptr;
    auto appendRunner = CompileJoinEntry("jb_append", appendEntry);
    ASSERT_NE(appendEntry, nullptr);
    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);
    auto jbAppend = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(appendEntry);

    THashTable ht{};
    ASSERT_TRUE(jtInit(&ht, 8, 8));
    const int64_t slot = 3;
    for (int64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(jbAppend(&ht, slot, 1000 + i));
    }
    EXPECT_EQ(ht.AggBuffers[0][slot], 10);
    EXPECT_EQ(ht.AggBuffers[1][slot], 16); // 4 -> 8 -> 16
    auto* bucket = reinterpret_cast<int64_t*>(ht.AggBuffers[2][slot]);
    ASSERT_NE(bucket, nullptr);
    for (int64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(bucket[i], 1000 + i);
    }
    EXPECT_EQ(ht.AggBuffers[0][0], 0); // untouched slot
    jtDestroy(&ht);
}

TEST(JoinKernel, PairBufferPushGrowsAndStoresPairs) {
    void* pushEntry = nullptr;
    auto pushRunner = CompileJoinEntry("pb_push", pushEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("pb_destroy", destroyEntry);
    auto pbPush = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(pushEntry);
    auto pbDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);

    TPairBuffer buf{};
    for (int64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(pbPush(&buf, 100 + i, 200 + i));
    }
    EXPECT_EQ(buf.Count, 10);
    ASSERT_NE(buf.Data, nullptr);
    for (int64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(buf.Data[2 * i], 100 + i);
        EXPECT_EQ(buf.Data[2 * i + 1], 200 + i);
    }
    pbDestroy(&buf);
    EXPECT_EQ(buf.Count, 0);
    EXPECT_EQ(buf.Data, nullptr);
}

TEST(JoinKernel, ProcessBatchMatchesNestedLoop) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinEntry("jt_init", initEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
    void* procEntry = nullptr;
    auto procRunner = CompileJoinEntry("jt_process_batch", procEntry);
    void* pbDestroyEntry = nullptr;
    auto pbDestroyRunner = CompileJoinEntry("pb_destroy", pbDestroyEntry);
    ASSERT_NE(procEntry, nullptr);

    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);
    auto pbDestroy = reinterpret_cast<void(*)(void*)>(pbDestroyEntry);
    auto process = reinterpret_cast<bool(*)(
        void*, void*, TRowSet*, int64_t, int64_t, int64_t, void*)>(procEntry);

    std::vector<int64_t> lkeys = {1, 2, 1};
    std::vector<int64_t> rkeys = {1, 1, 3};

    auto makeBatch = [](std::vector<int64_t>& keys, std::vector<TColumn>& cols) {
        cols = {TColumn{.Data = reinterpret_cast<char*>(keys.data())}};
        return TRowSet{.Columns = cols.data(), .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(keys.size()), .RefCount = 1};
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = makeBatch(lkeys, lcols);
    TRowSet rbatch = makeBatch(rkeys, rcols);

    THashTable left{}, right{};
    ASSERT_TRUE(jtInit(&left, 8, 8));
    ASSERT_TRUE(jtInit(&right, 8, 8));
    TPairBuffer pairs{};

    // Process left batch (probe right=empty, insert left), then right batch
    // (probe left, emit, insert right). batch_idx = 0 for both.
    ASSERT_TRUE(process(&left, &right, &lbatch, 0, 0, /*is_left=*/1, &pairs));
    ASSERT_TRUE(process(&right, &left, &rbatch, 0, 0, /*is_left=*/0, &pairs));

    // Collect emitted (leftRow, rightRow) pairs (batch_idx 0 -> RowId == rowIdx).
    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        int64_t leftId = pairs.Data[2 * i];
        int64_t rightId = pairs.Data[2 * i + 1];
        got.emplace_back(leftId & 0xffffffff, rightId & 0xffffffff);
    }

    // Nested-loop oracle.
    std::vector<std::tuple<int64_t, int64_t>> expected;
    for (size_t li = 0; li < lkeys.size(); ++li) {
        for (size_t ri = 0; ri < rkeys.size(); ++ri) {
            if (lkeys[li] == rkeys[ri]) {
                expected.emplace_back(static_cast<int64_t>(li), static_cast<int64_t>(ri));
            }
        }
    }
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);

    pbDestroy(&pairs);
    jtDestroy(&left);
    jtDestroy(&right);
}

TEST(JoinKernel, ProcessBatchTriggersRehashAndStaysCorrect) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinEntry("jt_init", initEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinEntry("jt_destroy", destroyEntry);
    void* procEntry = nullptr;
    auto procRunner = CompileJoinEntry("jt_process_batch", procEntry);
    void* pbDestroyEntry = nullptr;
    auto pbDestroyRunner = CompileJoinEntry("pb_destroy", pbDestroyEntry);
    ASSERT_NE(procEntry, nullptr);

    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);
    auto pbDestroy = reinterpret_cast<void(*)(void*)>(pbDestroyEntry);
    auto process = reinterpret_cast<bool(*)(
        void*, void*, TRowSet*, int64_t, int64_t, int64_t, void*)>(procEntry);

    // ~25 distinct keys over 60 rows per side forces several rehashes from an
    // initial capacity of 4 (also exercises bucket-pointer carry across rehash).
    int seed = 12345;
    auto rnd = [&]() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
    std::vector<int64_t> lkeys, rkeys;
    for (int i = 0; i < 60; ++i) lkeys.push_back(rnd() % 25);
    for (int i = 0; i < 60; ++i) rkeys.push_back(rnd() % 25);

    auto makeBatch = [](std::vector<int64_t>& keys, std::vector<TColumn>& cols) {
        cols = {TColumn{.Data = reinterpret_cast<char*>(keys.data())}};
        return TRowSet{.Columns = cols.data(), .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(keys.size()), .RefCount = 1};
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = makeBatch(lkeys, lcols);
    TRowSet rbatch = makeBatch(rkeys, rcols);

    THashTable left{}, right{};
    ASSERT_TRUE(jtInit(&left, 4, 8)); // tiny initial capacity -> forces rehash
    ASSERT_TRUE(jtInit(&right, 4, 8));
    TPairBuffer pairs{};
    ASSERT_TRUE(process(&left, &right, &lbatch, 0, 0, 1, &pairs));
    ASSERT_TRUE(process(&right, &left, &rbatch, 0, 0, 0, &pairs));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        int64_t leftRow = pairs.Data[2 * i] & 0xffffffff;
        int64_t rightRow = pairs.Data[2 * i + 1] & 0xffffffff;
        // Every emitted pair must have matching keys.
        ASSERT_EQ(lkeys[leftRow], rkeys[rightRow]);
        got.emplace_back(leftRow, rightRow);
    }

    std::vector<std::tuple<int64_t, int64_t>> expected;
    for (size_t li = 0; li < lkeys.size(); ++li) {
        for (size_t ri = 0; ri < rkeys.size(); ++ri) {
            if (lkeys[li] == rkeys[ri]) {
                expected.emplace_back(static_cast<int64_t>(li), static_cast<int64_t>(ri));
            }
        }
    }
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
    EXPECT_GT(left.Capacity, 4); // confirm a rehash actually happened

    pbDestroy(&pairs);
    jtDestroy(&left);
    jtDestroy(&right);
}

TEST(CompileJoin, ProducesWorkingInnerJoinKernels) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rk", i64()}, {"rv", i64()}});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(leftType, rightType, "lk", "rk", EJoinType::Inner);
    EXPECT_EQ(kernels.LeftKeyColIdx, 0);
    EXPECT_EQ(kernels.RightKeyColIdx, 0);

    std::vector<int64_t> lkeys = {1, 2, 1};
    std::vector<int64_t> rkeys = {1, 1, 3};
    auto makeBatch = [](std::vector<int64_t>& keys, std::vector<TColumn>& cols) {
        // Two columns (key, value); only the key column is read here.
        cols = {TColumn{.Data = reinterpret_cast<char*>(keys.data())},
                TColumn{.Data = reinterpret_cast<char*>(keys.data())}};
        return TRowSet{.Columns = cols.data(), .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(keys.size()), .RefCount = 1};
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = makeBatch(lkeys, lcols);
    TRowSet rbatch = makeBatch(rkeys, rcols);

    THashTable left{}, right{};
    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Init(&left, 8));
    ASSERT_TRUE(kernels.Init(&right, 8));
    ASSERT_TRUE(kernels.Process(&left, &right, &lbatch, kernels.LeftKeyColIdx, 0, 1, &pairs));
    ASSERT_TRUE(kernels.Process(&right, &left, &rbatch, kernels.RightKeyColIdx, 0, 0, &pairs));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    kernels.DestroyPairs(&pairs);
    kernels.DestroyTable(&left);
    kernels.DestroyTable(&right);
}

TEST(CompileJoin, RejectsNonI64KeyAndNonInner) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType strLeft({{"lk", std::make_shared<TStringType>()}});
    TStructType rightType({{"rk", i64()}});
    TKernelCompiler compiler;
    EXPECT_THROW(compiler.CompileJoin(strLeft, rightType, "lk", "rk", EJoinType::Inner),
                 NQumir::TError);

    TStructType leftType({{"lk", i64()}});
    EXPECT_THROW(compiler.CompileJoin(leftType, rightType, "lk", "rk", EJoinType::Left),
                 NQumir::TError);
    EXPECT_THROW(compiler.CompileJoin(leftType, rightType, "missing", "rk", EJoinType::Inner),
                 NQumir::TError);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

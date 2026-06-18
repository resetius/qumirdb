#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
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

TEST(CompileJoin, ProducesWorkingInnerJoinKernels) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rk", i64()}, {"rv", i64()}});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);

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
    // left_store / right_store are single-batch arrays (batch_idx 0). The default
    // (always-true) jt_residual_filter ignores them.
    ASSERT_TRUE(kernels.ProcessLeft(&left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch));
    ASSERT_TRUE(kernels.ProcessRight(&right, &left, &rbatch, 0, &pairs, &lbatch, &rbatch));

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

TEST(CompileJoin, ProbeOnlyStreamsWithoutInserting) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rk", i64()}, {"rv", i64()}});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);

    std::vector<int64_t> lkeys = {1, 2, 1};
    std::vector<int64_t> rkeys = {1, 1, 3};
    auto makeBatch = [](std::vector<int64_t>& keys, std::vector<TColumn>& cols) {
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
    ASSERT_TRUE(kernels.ProcessLeft(&left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch));
    ASSERT_EQ(pairs.Count, 0);
    ASSERT_TRUE(kernels.ProbeRightStream(&left, &rbatch, 7, &pairs, &lbatch, &rbatch));
    EXPECT_EQ(right.Size, 0);

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    pairs.Count = 0;
    kernels.DestroyTable(&left);
    kernels.DestroyTable(&right);
    THashTable left2{}, right2{};
    ASSERT_TRUE(kernels.Init(&left2, 8));
    ASSERT_TRUE(kernels.Init(&right2, 8));
    ASSERT_TRUE(kernels.ProcessRight(&right2, &left2, &rbatch, 0, &pairs, &lbatch, &rbatch));
    ASSERT_EQ(pairs.Count, 0);
    ASSERT_TRUE(kernels.ProbeLeftStream(&right2, &lbatch, 9, &pairs, &lbatch, &rbatch));
    EXPECT_EQ(left2.Size, 0);

    got.clear();
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    kernels.DestroyPairs(&pairs);
    kernels.DestroyTable(&left2);
    kernels.DestroyTable(&right2);
}

TEST(CompileJoin, RejectsStringKeyNonInnerAndIncompatible) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TKernelCompiler compiler;

    // String keys are not supported yet (fixed-key scope).
    TStructType strLeft({{"lk", std::make_shared<TStringType>()}});
    TStructType strRight({{"rk", std::make_shared<TStringType>()}});
    EXPECT_THROW(compiler.CompileJoin(strLeft, strRight, {{"lk", "rk"}}, EJoinType::Inner),
                 NQumir::TError);

    TStructType leftType({{"lk", i64()}});
    TStructType rightType({{"rk", i64()}});
    // Unsupported join type (Full not implemented yet).
    EXPECT_THROW(compiler.CompileJoin(leftType, rightType, {{"lk", "rk"}}, EJoinType::Full),
                 NQumir::TError);
    // Missing column.
    EXPECT_THROW(compiler.CompileJoin(leftType, rightType, {{"missing", "rk"}}, EJoinType::Inner),
                 NQumir::TError);
    // Incompatible key types (i64 vs string).
    EXPECT_THROW(compiler.CompileJoin(leftType, strRight, {{"lk", "rk"}}, EJoinType::Inner),
                 NQumir::TError);
}

namespace {

// Compiles one entry of the GENERIC join program: key-ops overloads for the
// join key + the shared library + generated jt_process_left/right.
std::unique_ptr<NQumir::TLLVMRunner> CompileGenericJoin(
    const NKernel::TJoinKeyDescriptor& keyDesc, const std::string& entry, void*& fn)
{
    using namespace NQumir::NAst;
    auto module = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType, rowSetType, hashTableType, pairBufferType;
    for (const auto& et : module->ExternalTypes()) {
        if (et.Name == "TColumn") columnType = et.Type;
        else if (et.Name == "TRowSet") rowSetType = et.Type;
        else if (et.Name == "HashTable") hashTableType = et.Type;
        else if (et.Name == "PairBuffer") pairBufferType = et.Type;
    }
    auto lib = NKernel::BuildJoinKernelLibrary();
    if (!lib) { ADD_FAILURE() << lib.error().ToString(); return {}; }
    std::vector<TExprPtr> program;
    for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) program.push_back(std::move(f));
    for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) program.push_back(std::move(f));
    for (auto& f : *lib) program.push_back(std::move(f));
    program.push_back(NKernel::GenJoinProcessAst(keyDesc, true, "jt_process_left",
        columnType, rowSetType, hashTableType, pairBufferType));
    program.push_back(NKernel::GenJoinProcessAst(keyDesc, false, "jt_process_right",
        columnType, rowSetType, hashTableType, pairBufferType));
    program.push_back(NKernel::GenJoinProbeAst(keyDesc, true, "jt_probe_left_stream",
        columnType, rowSetType, hashTableType, pairBufferType));
    program.push_back(NKernel::GenJoinProbeAst(keyDesc, false, "jt_probe_right_stream",
        columnType, rowSetType, hashTableType, pairBufferType));

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto prog = std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(program));
    std::string error;
    fn = runner->CompileKernelAst(prog, entry, &error);
    EXPECT_NE(fn, nullptr) << error;
    return runner;
}

} // namespace

TEST(JoinKernelGeneric, Int32KeyMatchesNestedLoop) {
    using namespace NQumir::NAst;
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i32}, {"lv", i64}});
    TStructType rightType({{"rk", i32}, {"rv", i64}});
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});

    void* initFn = nullptr;   auto r1 = CompileGenericJoin(keyDesc, "jt_init", initFn);
    void* destroyFn = nullptr; auto r2 = CompileGenericJoin(keyDesc, "jt_destroy", destroyFn);
    void* pbDestroyFn = nullptr; auto r3 = CompileGenericJoin(keyDesc, "pb_destroy", pbDestroyFn);
    void* leftFn = nullptr;   auto r4 = CompileGenericJoin(keyDesc, "jt_process_left", leftFn);
    void* rightFn = nullptr;  auto r5 = CompileGenericJoin(keyDesc, "jt_process_right", rightFn);
    ASSERT_NE(leftFn, nullptr);
    ASSERT_NE(rightFn, nullptr);

    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initFn);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyFn);
    auto pbDestroy = reinterpret_cast<void(*)(void*)>(pbDestroyFn);
    auto procLeft = reinterpret_cast<bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*)>(leftFn);
    auto procRight = reinterpret_cast<bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*)>(rightFn);

    // int32 keys, i64 values.
    std::vector<int32_t> lk = {1, 2, 1}; std::vector<int64_t> lv = {10, 20, 30};
    std::vector<int32_t> rk = {1, 1, 3}; std::vector<int64_t> rv = {100, 200, 300};
    auto batch = [](int32_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
        cols = {TColumn{.Data = reinterpret_cast<char*>(keys)},
                TColumn{.Data = reinterpret_cast<char*>(vals)}};
        return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = batch(lk.data(), lv.data(), 3, lcols);
    TRowSet rbatch = batch(rk.data(), rv.data(), 3, rcols);

    // The Key descriptor's struct may be wider than 4 bytes; jt_init uses its Size.
    const int64_t keySize = static_cast<int64_t>(keyDesc.Size);
    THashTable left{}, right{};
    ASSERT_TRUE(jtInit(&left, 8, keySize));
    ASSERT_TRUE(jtInit(&right, 8, keySize));
    TPairBuffer pairs{};
    ASSERT_TRUE(procLeft(&left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch));
    ASSERT_TRUE(procRight(&right, &left, &rbatch, 0, &pairs, &lbatch, &rbatch));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    pbDestroy(&pairs);
    jtDestroy(&left);
    jtDestroy(&right);
}

TEST(JoinKernelGeneric, Int32KeyTriggersRehash) {
    using namespace NQumir::NAst;
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    TStructType leftType({{"lk", i32}});
    TStructType rightType({{"rk", i32}});
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, {{"lk", "rk"}});

    void* initFn = nullptr;      auto r1 = CompileGenericJoin(keyDesc, "jt_init", initFn);
    void* destroyFn = nullptr;   auto r2 = CompileGenericJoin(keyDesc, "jt_destroy", destroyFn);
    void* pbDestroyFn = nullptr; auto r3 = CompileGenericJoin(keyDesc, "pb_destroy", pbDestroyFn);
    void* leftFn = nullptr;      auto r4 = CompileGenericJoin(keyDesc, "jt_process_left", leftFn);
    void* rightFn = nullptr;     auto r5 = CompileGenericJoin(keyDesc, "jt_process_right", rightFn);
    ASSERT_NE(leftFn, nullptr);
    ASSERT_NE(rightFn, nullptr);

    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initFn);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyFn);
    auto pbDestroy = reinterpret_cast<void(*)(void*)>(pbDestroyFn);
    auto procLeft = reinterpret_cast<bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*)>(leftFn);
    auto procRight = reinterpret_cast<bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*)>(rightFn);

    // ~25 distinct int32 keys over 60 rows/side forces rehashes from capacity 4.
    int seed = 12345;
    auto rnd = [&]() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
    std::vector<int32_t> lk, rk;
    for (int i = 0; i < 60; ++i) lk.push_back(rnd() % 25);
    for (int i = 0; i < 60; ++i) rk.push_back(rnd() % 25);
    auto batch = [](std::vector<int32_t>& keys, std::vector<TColumn>& cols) {
        cols = {TColumn{.Data = reinterpret_cast<char*>(keys.data())}};
        return TRowSet{.Columns = cols.data(), .ColumnCount = 1,
            .RowCount = static_cast<int64_t>(keys.size()), .RefCount = 1};
    };
    std::vector<TColumn> lcols, rcols;
    TRowSet lbatch = batch(lk, lcols);
    TRowSet rbatch = batch(rk, rcols);

    const int64_t keySize = static_cast<int64_t>(keyDesc.Size);
    THashTable left{}, right{};
    ASSERT_TRUE(jtInit(&left, 4, keySize)); // tiny capacity -> forces rehash
    ASSERT_TRUE(jtInit(&right, 4, keySize));
    TPairBuffer pairs{};
    ASSERT_TRUE(procLeft(&left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch));
    ASSERT_TRUE(procRight(&right, &left, &rbatch, 0, &pairs, &lbatch, &rbatch));

    int64_t matched = 0;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        int64_t leftRow = pairs.Data[2 * i] & 0xffffffff;
        int64_t rightRow = pairs.Data[2 * i + 1] & 0xffffffff;
        ASSERT_EQ(lk[leftRow], rk[rightRow]); // every emitted pair has matching keys
        ++matched;
    }
    int64_t expected = 0;
    for (int32_t a : lk) for (int32_t b : rk) if (a == b) ++expected;
    EXPECT_EQ(matched, expected);
    EXPECT_GT(left.Capacity, 4); // rehash happened (bucket pointers carried)

    pbDestroy(&pairs);
    jtDestroy(&left);
    jtDestroy(&right);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

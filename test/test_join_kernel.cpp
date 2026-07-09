#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/types/nullable.h>

#include "qumirdb_source_module.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;

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

constexpr int64_t JoinOpCode(EJoinKernelOp op) {
    return static_cast<int64_t>(op);
}

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
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*lib));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

} // namespace

TEST(JoinKernel, PairBufferLayout) {
    std::shared_ptr<NQumir::NAst::TStructType> pairBuffer;
    bool hasJoinTable = false;
    for (const auto& type : NQumir::NRegistry::QumirDbExternalTypes()) {
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
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

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
    ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr,
        8, JoinOpCode(EJoinKernelOp::Init)));
    // left_store / right_store are single-batch arrays (batch_idx 0). The default
    // (always-true) jt_residual_filter ignores them.
    ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch,
        0, JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, 0, &pairs, &lbatch, &rbatch,
        0, JoinOpCode(EJoinKernelOp::UpdateRight)));

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr,
        0, JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoinHash, ProducesMatchingSideHashes) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rv", i64()}, {"rk", i64()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoinHash(spec);

    std::vector<int64_t> lkeys = {1, 2, 1, 4};
    std::vector<int64_t> lvalues = {10, 20, 30, 40};
    std::vector<int64_t> rvalues = {100, 200, 300, 400};
    std::vector<int64_t> rkeys = {1, 3, 2, 1};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lkeys.data())},
        TColumn{.Data = reinterpret_cast<char*>(lvalues.data())},
    };
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rvalues.data())},
        TColumn{.Data = reinterpret_cast<char*>(rkeys.data())},
    };
    TRowSet lbatch{
        .Columns = lcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(lkeys.size()),
        .RefCount = 1,
    };
    TRowSet rbatch{
        .Columns = rcols.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(rkeys.size()),
        .RefCount = 1,
    };

    std::vector<uint64_t> leftHashes(lkeys.size(), 0);
    std::vector<uint64_t> rightHashes(rkeys.size(), 0);
    ASSERT_TRUE(kernels.Left(&lbatch, leftHashes.data()));
    ASSERT_TRUE(kernels.Right(&rbatch, rightHashes.data()));

    EXPECT_EQ(leftHashes[0], rightHashes[0]);
    EXPECT_EQ(leftHashes[1], rightHashes[2]);
    EXPECT_EQ(leftHashes[2], rightHashes[3]);
    EXPECT_EQ(leftHashes[0], leftHashes[2]);
    EXPECT_NE(leftHashes[0], leftHashes[1]);
}

TEST(CompileJoin, ProbeOnlyStreamsWithoutInserting) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TStructType leftType({{"lk", i64()}, {"lv", i64()}});
    TStructType rightType({{"rk", i64()}, {"rv", i64()}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

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
    ASSERT_TRUE(kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr,
        8, JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(&left, &right, &lbatch, 0, &pairs, &lbatch, &rbatch,
        0, JoinOpCode(EJoinKernelOp::UpdateLeft)));
    ASSERT_EQ(pairs.Count, 0);
    ASSERT_TRUE(kernels.Dispatch(&left, &right, &rbatch, 7, &pairs, &lbatch, &rbatch,
        0, JoinOpCode(EJoinKernelOp::StreamRight)));
    EXPECT_EQ(right.Size, 0);

    std::vector<std::tuple<int64_t, int64_t>> got;
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    pairs.Count = 0;
    kernels.Dispatch(&left, &right, nullptr, 0, &pairs, nullptr, nullptr,
        0, JoinOpCode(EJoinKernelOp::Destroy));
    THashTable left2{}, right2{};
    ASSERT_TRUE(kernels.Dispatch(&left2, &right2, nullptr, 0, &pairs, nullptr, nullptr,
        8, JoinOpCode(EJoinKernelOp::Init)));
    ASSERT_TRUE(kernels.Dispatch(&left2, &right2, &rbatch, 0, &pairs, &lbatch, &rbatch,
        0, JoinOpCode(EJoinKernelOp::UpdateRight)));
    ASSERT_EQ(pairs.Count, 0);
    ASSERT_TRUE(kernels.Dispatch(&left2, &right2, &lbatch, 9, &pairs, &lbatch, &rbatch,
        0, JoinOpCode(EJoinKernelOp::StreamLeft)));
    EXPECT_EQ(left2.Size, 0);

    got.clear();
    for (int64_t i = 0; i < pairs.Count; ++i) {
        got.emplace_back(pairs.Data[2 * i] & 0xffffffff, pairs.Data[2 * i + 1] & 0xffffffff);
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);

    kernels.Dispatch(&left2, &right2, nullptr, 0, &pairs, nullptr, nullptr,
        0, JoinOpCode(EJoinKernelOp::Destroy));
}

TEST(CompileJoin, RejectsStringKeyNonInnerAndIncompatible) {
    using namespace NQumir::NAst;
    auto i64 = [] { return std::make_shared<TIntegerType>(TIntegerType::I64); };
    TKernelCompiler compiler;

    // String keys are not supported yet (fixed-key scope).
    TStructType strLeft({{"lk", std::make_shared<TStringType>()}});
    TStructType strRight({{"rk", std::make_shared<TStringType>()}});
    auto stringSpec = NKernel::BuildJoinKernelSpec(
        strLeft, strRight, {{"lk", "rk"}}, EJoinType::Inner);
    EXPECT_THROW(compiler.CompileJoin(stringSpec), NQumir::TError);

    TStructType leftType({{"lk", i64()}});
    TStructType rightType({{"rk", i64()}});
    // Unsupported join type (Full not implemented yet).
    auto fullSpec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::Full);
    EXPECT_THROW(compiler.CompileJoin(fullSpec), NQumir::TError);
    // Missing column.
    EXPECT_THROW(NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"missing", "rk"}}, EJoinType::Inner),
        std::runtime_error);
    // Incompatible key types (i64 vs string).
    auto incompatibleSpec = NKernel::BuildJoinKernelSpec(
        leftType, strRight, {{"lk", "rk"}}, EJoinType::Inner);
    EXPECT_THROW(compiler.CompileJoin(incompatibleSpec), NQumir::TError);
}

namespace {

// Compiles one entry of the GENERIC join program: key-ops overloads for the
// join key + the shared library + generated jt_process_left/right.
std::unique_ptr<NQumir::TLLVMRunner> CompileGenericJoin(
    const NKernel::TJoinKeyDescriptor& keyDesc, const std::string& entry, void*& fn)
{
    using namespace NQumir::NAst;
    TTypePtr columnType, rowSetType, hashTableType, pairBufferType;
    for (const auto& et : NQumir::NRegistry::QumirDbExternalTypes()) {
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
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto prog = std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(program));
    NQdb::NTest::AddQumirDbUse(prog);
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

namespace {

// Packs (batch_idx << 32) | row the way the join kernels emit row ids.
constexpr int64_t PackRowId(int64_t batchIdx, int64_t row) {
    return (batchIdx << 32) | (row & 0xffffffff);
}

// Frees the buffers a jt_materialize call allocated (mirrors the host's
// kernel-owned-rowset Destroy callback).
void FreeMaterializedRowSet(TRowSet& rowSet) {
    auto* owners = static_cast<int64_t*>(rowSet.Private);
    ASSERT_NE(owners, nullptr);
    for (int64_t i = 0; i < owners[0]; ++i) {
        qdb_free(reinterpret_cast<void*>(owners[i + 1]));
    }
    qdb_free(owners);
    rowSet.Private = nullptr;
}

bool MaskBit(const TColumn& col, int64_t row) {
    return ((col.Mask[row / 8] >> (row % 8)) & 1) != 0;
}

} // namespace

TEST(JoinMaterialize, GathersFixedAndStringColumnsWithNullPadding) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    TStructType leftType({{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rk", i64}, {"rv", i32}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::Left);
    auto kernels = compiler.CompileJoin(spec);

    // Left store: one batch of 3 rows with a string payload column.
    std::vector<int64_t> lk = {1, 2, 3};
    std::string lnameBytes = "abb"; // "a", "bb", ""
    std::vector<int64_t> lnameOffsets = {0, 1, 3, 3};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = lnameBytes.data(),
            .Offsets = lnameOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2,
        .RowCount = 3, .RefCount = 1};

    // Right store: one batch of 2 rows with an i32 payload column.
    std::vector<int64_t> rk = {1, 2};
    std::vector<int32_t> rv = {10, 20};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rk.data())},
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2,
        .RowCount = 2, .RefCount = 1};

    // Hand-built pairs: two matches plus one outer-padded left row (right -1).
    std::vector<int64_t> pairData = {
        PackRowId(0, 0), PackRowId(0, 0),
        PackRowId(0, 1), PackRowId(0, 1),
        PackRowId(0, 2), -1,
    };
    TPairBuffer pairs{.Count = 3, .Capacity = 3, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(
        &pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 3);
    ASSERT_EQ(out.RowCount, 3);
    ASSERT_EQ(out.ColumnCount, 4);
    EXPECT_EQ(out.Selection, nullptr);
    EXPECT_EQ(out.RefCount, 1);

    // lk: all rows valid.
    const auto* outLk = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    EXPECT_EQ(outLk[0], 1);
    EXPECT_EQ(outLk[1], 2);
    EXPECT_EQ(outLk[2], 3);
    for (int64_t j = 0; j < 3; ++j) {
        EXPECT_TRUE(MaskBit(out.Columns[0], j));
    }

    // lname: string payloads follow pair order.
    const auto& lname = out.Columns[1];
    ASSERT_EQ(lname.OffsetWidth, 8);
    const auto* offsets = static_cast<const int64_t*>(lname.Offsets);
    EXPECT_EQ(std::string(lname.Data + offsets[0], lname.Data + offsets[1]), "a");
    EXPECT_EQ(std::string(lname.Data + offsets[1], lname.Data + offsets[2]), "bb");
    EXPECT_EQ(offsets[3], offsets[2]); // ""

    // rv (i32): matched rows carry values, the padded row is NULL.
    const auto* outRv = reinterpret_cast<const int32_t*>(out.Columns[3].Data);
    EXPECT_EQ(outRv[0], 10);
    EXPECT_EQ(outRv[1], 20);
    EXPECT_EQ(outRv[2], 0); // zeroed padding
    EXPECT_TRUE(MaskBit(out.Columns[3], 0));
    EXPECT_TRUE(MaskBit(out.Columns[3], 1));
    EXPECT_FALSE(MaskBit(out.Columns[3], 2));
    EXPECT_FALSE(MaskBit(out.Columns[2], 2)); // rk padded NULL too

    FreeMaterializedRowSet(out);

    // start/limit cursoring: one row starting at pair 1.
    TRowSet slice{};
    ASSERT_EQ(kernels.Materialize(
        &pairs, &lbatch, &rbatch, &lbatch, &rbatch, 1, 1, &slice), 1);
    ASSERT_EQ(slice.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<const int64_t*>(slice.Columns[0].Data)[0], 2);
    EXPECT_EQ(reinterpret_cast<const int32_t*>(slice.Columns[3].Data)[0], 20);
    FreeMaterializedRowSet(slice);

    // Exhausted / empty cursor positions leave the output untouched.
    TRowSet empty{};
    EXPECT_EQ(kernels.Materialize(
        &pairs, &lbatch, &rbatch, &lbatch, &rbatch, 3, 100, &empty), 0);
    EXPECT_EQ(empty.Columns, nullptr);
    TPairBuffer noPairs{};
    EXPECT_EQ(kernels.Materialize(
        &noPairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &empty), 0);
}

TEST(JoinMaterialize, PropagatesSourceNullsAndReadsStreamBatches) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64},
        {"lv", std::make_shared<TNullable>(i64)}});
    TStructType rightType({{"rk", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::Inner);
    auto kernels = compiler.CompileJoin(spec);

    // Left store: 2 rows; lv row 1 is NULL at the source.
    std::vector<int64_t> lk = {1, 2};
    std::vector<int64_t> lv = {100, 200};
    std::vector<uint8_t> lvMask = {0b01}; // row 0 valid, row 1 null
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data()), .Mask = lvMask.data()},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2,
        .RowCount = 2, .RefCount = 1};

    // Right store batch and a DIFFERENT right stream batch: pairs with a
    // stream-encoded right id (batch -1) must read the stream batch.
    std::vector<int64_t> rkStore = {7};
    std::vector<TColumn> rcolsStore = {
        TColumn{.Data = reinterpret_cast<char*>(rkStore.data())}};
    TRowSet rstore{.Columns = rcolsStore.data(), .ColumnCount = 1,
        .RowCount = 1, .RefCount = 1};
    std::vector<int64_t> rkStream = {42, 43};
    std::vector<TColumn> rcolsStream = {
        TColumn{.Data = reinterpret_cast<char*>(rkStream.data())}};
    TRowSet rstream{.Columns = rcolsStream.data(), .ColumnCount = 1,
        .RowCount = 2, .RefCount = 1};

    std::vector<int64_t> pairData = {
        PackRowId(0, 0), PackRowId(0, 0),  // store right row -> 7
        PackRowId(0, 1), PackRowId(-1, 1), // stream right row -> 43
    };
    TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(
        &pairs, &lbatch, &rstore, &lbatch, &rstream, 0, 100, &out), 2);
    ASSERT_EQ(out.RowCount, 2);
    ASSERT_EQ(out.ColumnCount, 3);

    // lv: source null propagated.
    EXPECT_EQ(reinterpret_cast<const int64_t*>(out.Columns[1].Data)[0], 100);
    EXPECT_TRUE(MaskBit(out.Columns[1], 0));
    EXPECT_FALSE(MaskBit(out.Columns[1], 1));

    // rk: row 0 from the store, row 1 from the stream batch.
    const auto* outRk = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
    EXPECT_EQ(outRk[0], 7);
    EXPECT_EQ(outRk[1], 43);

    FreeMaterializedRowSet(out);
}

TEST(JoinMaterialize, SemiAntiOutputsLeftColumnsOnly) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lv", i64}});
    TStructType rightType({{"rk", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildJoinKernelSpec(
        leftType, rightType, {{"lk", "rk"}}, EJoinType::LeftSemi);
    auto kernels = compiler.CompileJoin(spec);

    std::vector<int64_t> lk = {1, 2};
    std::vector<int64_t> lv = {10, 20};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = reinterpret_cast<char*>(lv.data())},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2,
        .RowCount = 2, .RefCount = 1};

    // Semi/anti pairs carry right = -1; the right side must not be read.
    std::vector<int64_t> pairData = {
        PackRowId(0, 1), -1,
        PackRowId(0, 0), -1,
    };
    TPairBuffer pairs{.Count = 2, .Capacity = 2, .Data = pairData.data()};

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(
        &pairs, &lbatch, nullptr, &lbatch, nullptr, 0, 100, &out), 2);
    ASSERT_EQ(out.RowCount, 2);
    ASSERT_EQ(out.ColumnCount, 2); // left columns only
    const auto* outLk = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    const auto* outLv = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(outLk[0], 2);
    EXPECT_EQ(outLv[0], 20);
    EXPECT_EQ(outLk[1], 1);
    EXPECT_EQ(outLv[1], 10);

    FreeMaterializedRowSet(out);
}

TEST(CompileCrossJoin, EmitsSelectedPairsAndMaterializesStringPayload) {
    using namespace NQumir::NAst;
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType leftType({{"lk", i64}, {"lname", std::make_shared<TStringType>()}});
    TStructType rightType({{"rv", i64}});

    TKernelCompiler compiler;
    auto spec = NKernel::BuildCrossJoinKernelSpec(leftType, rightType);
    auto kernels = compiler.CompileCrossJoin(spec);

    std::vector<int64_t> lk = {1, 2, 3};
    std::string lnameBytes = "abbccc";
    std::vector<int64_t> lnameOffsets = {0, 1, 3, 6};
    std::vector<uint8_t> lsel = {1, 0, 1};
    std::vector<TColumn> lcols = {
        TColumn{.Data = reinterpret_cast<char*>(lk.data())},
        TColumn{.Data = lnameBytes.data(),
            .Offsets = lnameOffsets.data(), .OffsetWidth = 8},
    };
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2,
        .RowCount = 3, .Selection = lsel.data(), .RefCount = 1};

    std::vector<int64_t> rv = {10, 20};
    std::vector<uint8_t> rsel = {0, 1};
    std::vector<TColumn> rcols = {
        TColumn{.Data = reinterpret_cast<char*>(rv.data())},
    };
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 1,
        .RowCount = 2, .Selection = rsel.data(), .RefCount = 1};

    TPairBuffer pairs{};
    ASSERT_TRUE(kernels.Dispatch(
        &lbatch, 0, &rbatch, 1, &pairs,
        static_cast<int64_t>(ECrossJoinKernelOp::Emit)));
    ASSERT_EQ(pairs.Count, 2);
    EXPECT_EQ(pairs.Data[0], PackRowId(0, 0));
    EXPECT_EQ(pairs.Data[1], PackRowId(0, 1));
    EXPECT_EQ(pairs.Data[2], PackRowId(0, 2));
    EXPECT_EQ(pairs.Data[3], PackRowId(0, 1));

    TRowSet out{};
    ASSERT_EQ(kernels.Materialize(
        &pairs, &lbatch, &rbatch, &lbatch, &rbatch, 0, 100, &out), 2);
    ASSERT_EQ(out.RowCount, 2);
    ASSERT_EQ(out.ColumnCount, 3);

    const auto* outLk = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
    EXPECT_EQ(outLk[0], 1);
    EXPECT_EQ(outLk[1], 3);
    const auto& outName = out.Columns[1];
    const auto* offsets = static_cast<const int64_t*>(outName.Offsets);
    EXPECT_EQ(std::string(outName.Data + offsets[0], outName.Data + offsets[1]), "a");
    EXPECT_EQ(std::string(outName.Data + offsets[1], outName.Data + offsets[2]), "ccc");
    const auto* outRv = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
    EXPECT_EQ(outRv[0], 20);
    EXPECT_EQ(outRv[1], 20);

    FreeMaterializedRowSet(out);
    kernels.Dispatch(
        nullptr, 0, nullptr, 0, &pairs,
        static_cast<int64_t>(ECrossJoinKernelOp::Destroy));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

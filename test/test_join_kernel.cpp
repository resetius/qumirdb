#include <gtest/gtest.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

using namespace NQqb;

namespace {

// Mirrors the JoinTable C layout from modules/qumirdb.cpp.
struct TJoinTable {
    uint8_t* Keys = nullptr;
    int64_t* Dist = nullptr;
    int64_t* SlotId = nullptr;
    uint8_t* GroupKeys = nullptr;
    int64_t* BucketCount = nullptr;
    int64_t* BucketCap = nullptr;
    int64_t** BucketData = nullptr;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t KeySize = 0;
};
static_assert(sizeof(TJoinTable) == TKernelCompiler::kJoinTableSize);

// Mirrors the PairBuffer C layout from modules/qumirdb.cpp.
struct TPairBuffer {
    int64_t Count = 0;
    int64_t Capacity = 0;
    int64_t* Data = nullptr;
};
static_assert(sizeof(TPairBuffer) == TKernelCompiler::kPairBufferSize);

// Compiles one entry point of join_table.oz with QumirDbModule registered.
std::unique_ptr<NQumir::TLLVMRunner> CompileJoinKernel(
    const std::string& entryName, void*& entry)
{
    auto library = NQqb::NKernel::ParseFunctionLibrary(
        NQqb::NKernel::ReadJoinKernel("join_table.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
        return {};
    }
    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    runner->RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*library));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

} // namespace

TEST(JoinKernel, JoinTableLayout) {
    NQumir::NRegistry::QumirDbModule module;
    std::shared_ptr<NQumir::NAst::TStructType> joinTable;
    std::shared_ptr<NQumir::NAst::TStructType> pairBuffer;
    for (const auto& type : module.ExternalTypes()) {
        if (type.Name == "JoinTable") {
            joinTable = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type.Type).Cast();
        } else if (type.Name == "PairBuffer") {
            pairBuffer = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(type.Type).Cast();
        }
    }
    ASSERT_NE(joinTable, nullptr);
    ASSERT_EQ(joinTable->Fields.size(), 10u);
    EXPECT_EQ(joinTable->Fields[4].first, "BucketCount");
    EXPECT_EQ(joinTable->Fields[6].first, "BucketData");
    EXPECT_EQ(joinTable->Fields[7].first, "Capacity");
    ASSERT_NE(pairBuffer, nullptr);
    ASSERT_EQ(pairBuffer->Fields.size(), 3u);
    EXPECT_EQ(pairBuffer->Fields[2].first, "Data");
}

TEST(JoinKernel, InitAllocatesAndDestroyClears) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinKernel("jt_init", initEntry);
    ASSERT_NE(initEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinKernel("jt_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);

    TJoinTable ht{};
    ASSERT_TRUE(jtInit(&ht, 8, 8));
    EXPECT_EQ(ht.Capacity, 8);
    EXPECT_EQ(ht.Size, 0);
    EXPECT_EQ(ht.KeySize, 8);
    ASSERT_NE(ht.Dist, nullptr);
    ASSERT_NE(ht.BucketData, nullptr);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(ht.Dist[i], -1);
        EXPECT_EQ(ht.SlotId[i], -1);
        EXPECT_EQ(ht.BucketCount[i], 0);
        EXPECT_EQ(ht.BucketCap[i], 0);
        EXPECT_EQ(ht.BucketData[i], nullptr);
    }

    jtDestroy(&ht);
    EXPECT_EQ(ht.Keys, nullptr);
    EXPECT_EQ(ht.Dist, nullptr);
    EXPECT_EQ(ht.BucketData, nullptr);
    EXPECT_EQ(ht.Capacity, 0);
}

TEST(JoinKernel, BucketAppendGrowsAndStoresRowIds) {
    void* initEntry = nullptr;
    auto initRunner = CompileJoinKernel("jt_init", initEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinKernel("jt_destroy", destroyEntry);
    void* appendEntry = nullptr;
    auto appendRunner = CompileJoinKernel("jb_append", appendEntry);
    ASSERT_NE(initEntry, nullptr);
    ASSERT_NE(appendEntry, nullptr);
    auto jtInit = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(initEntry);
    auto jtDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);
    auto jbAppend = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(appendEntry);

    TJoinTable ht{};
    ASSERT_TRUE(jtInit(&ht, 8, 8));

    const int64_t slot = 3;
    for (int64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(jbAppend(&ht, slot, 1000 + i));
    }
    EXPECT_EQ(ht.BucketCount[slot], 10);
    EXPECT_GE(ht.BucketCap[slot], 10);
    EXPECT_EQ(ht.BucketCap[slot], 16); // 4 -> 8 -> 16
    ASSERT_NE(ht.BucketData[slot], nullptr);
    for (int64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(ht.BucketData[slot][i], 1000 + i);
    }
    // Untouched slot stays empty.
    EXPECT_EQ(ht.BucketCount[0], 0);
    EXPECT_EQ(ht.BucketData[0], nullptr);

    jtDestroy(&ht);
}

TEST(JoinKernel, PairBufferPushGrowsAndStoresPairs) {
    void* pushEntry = nullptr;
    auto pushRunner = CompileJoinKernel("pb_push", pushEntry);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileJoinKernel("pb_destroy", destroyEntry);
    ASSERT_NE(pushEntry, nullptr);
    ASSERT_NE(destroyEntry, nullptr);
    auto pbPush = reinterpret_cast<bool(*)(void*, int64_t, int64_t)>(pushEntry);
    auto pbDestroy = reinterpret_cast<void(*)(void*)>(destroyEntry);

    TPairBuffer buf{};
    for (int64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(pbPush(&buf, 100 + i, 200 + i));
    }
    EXPECT_EQ(buf.Count, 10);
    EXPECT_GE(buf.Capacity, 10);
    ASSERT_NE(buf.Data, nullptr);
    for (int64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(buf.Data[2 * i], 100 + i);
        EXPECT_EQ(buf.Data[2 * i + 1], 200 + i);
    }

    pbDestroy(&buf);
    EXPECT_EQ(buf.Count, 0);
    EXPECT_EQ(buf.Capacity, 0);
    EXPECT_EQ(buf.Data, nullptr);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

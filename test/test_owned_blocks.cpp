#include <gtest/gtest.h>

#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
#include <memory>
#include <string>

namespace {

std::unique_ptr<NQumir::TLLVMRunner> CompileOwnedBlocks(
    const std::string& entryName,
    void*& entry)
{
    auto library = NQqb::NKernel::ParseFunctionLibrary(
        NQqb::NKernel::ReadAggregationKernel("owned_blocks.oz"));
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

TEST(OwnedBlocks, NullBlockDoesNotEnterRegistry) {
    void* entry = nullptr;
    auto runner = CompileOwnedBlocks("owned_blocks_register", entry);
    ASSERT_NE(entry, nullptr);
    auto registerBlock = reinterpret_cast<bool(*)(
        uint8_t***, int64_t*, int64_t*, uint8_t*)>(entry);

    uint8_t** blocks = nullptr;
    int64_t count = 0;
    int64_t capacity = 0;
    EXPECT_TRUE(registerBlock(&blocks, &count, &capacity, nullptr));
    EXPECT_EQ(blocks, nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(capacity, 0);
}

TEST(OwnedBlocks, GrowsAndDestroysRegisteredBlocks) {
    void* registerEntry = nullptr;
    auto registerRunner = CompileOwnedBlocks(
        "owned_blocks_register", registerEntry);
    ASSERT_NE(registerEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileOwnedBlocks(
        "owned_blocks_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto registerBlock = reinterpret_cast<bool(*)(
        uint8_t***, int64_t*, int64_t*, uint8_t*)>(registerEntry);
    auto destroy = reinterpret_cast<void(*)(
        uint8_t***, int64_t*, int64_t*)>(destroyEntry);

    uint8_t** blocks = nullptr;
    int64_t count = 0;
    int64_t capacity = 0;
    for (int64_t i = 0; i < 10; ++i) {
        auto* block = static_cast<uint8_t*>(qdb_alloc(i + 1));
        ASSERT_NE(block, nullptr);
        block[0] = static_cast<uint8_t>(i);
        ASSERT_TRUE(registerBlock(&blocks, &count, &capacity, block));
        EXPECT_EQ(count, i + 1);
        EXPECT_GE(capacity, count);
        EXPECT_EQ(blocks[i], block);
    }
    EXPECT_EQ(capacity, 16);

    destroy(&blocks, &count, &capacity);
    EXPECT_EQ(blocks, nullptr);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(capacity, 0);
}

TEST(OwnedBlocks, ReserveThenCommitCannotReallocate) {
    void* reserveEntry = nullptr;
    auto reserveRunner = CompileOwnedBlocks("owned_blocks_reserve", reserveEntry);
    ASSERT_NE(reserveEntry, nullptr);
    void* commitEntry = nullptr;
    auto commitRunner = CompileOwnedBlocks("owned_blocks_commit", commitEntry);
    ASSERT_NE(commitEntry, nullptr);
    void* destroyEntry = nullptr;
    auto destroyRunner = CompileOwnedBlocks(
        "owned_blocks_destroy", destroyEntry);
    ASSERT_NE(destroyEntry, nullptr);
    auto reserve = reinterpret_cast<bool(*)(
        uint8_t***, int64_t*, int64_t*, int64_t)>(reserveEntry);
    auto commit = reinterpret_cast<bool(*)(
        uint8_t***, int64_t*, int64_t*, uint8_t*)>(commitEntry);
    auto destroy = reinterpret_cast<void(*)(
        uint8_t***, int64_t*, int64_t*)>(destroyEntry);

    uint8_t** blocks = nullptr;
    int64_t count = 0;
    int64_t capacity = 0;
    ASSERT_TRUE(reserve(&blocks, &count, &capacity, 3));
    ASSERT_EQ(capacity, 4);
    auto** reservedStorage = blocks;
    for (int64_t i = 0; i < 3; ++i) {
        auto* block = static_cast<uint8_t*>(qdb_alloc(1));
        ASSERT_NE(block, nullptr);
        ASSERT_TRUE(commit(&blocks, &count, &capacity, block));
        EXPECT_EQ(blocks, reservedStorage);
    }
    EXPECT_EQ(count, 3);
    auto* uncommitted = static_cast<uint8_t*>(qdb_alloc(1));
    ASSERT_NE(uncommitted, nullptr);
    ASSERT_TRUE(commit(&blocks, &count, &capacity, uncommitted));
    auto* rejected = static_cast<uint8_t*>(qdb_alloc(1));
    ASSERT_NE(rejected, nullptr);
    EXPECT_FALSE(commit(&blocks, &count, &capacity, rejected));
    qdb_free(rejected);

    destroy(&blocks, &count, &capacity);
}

TEST(OwnedBlocks, HashTableUsesFormerScratchSlots) {
    NQumir::NRegistry::QumirDbModule module;
    std::shared_ptr<NQumir::NAst::TStructType> hashTable;
    for (const auto& type : module.ExternalTypes()) {
        if (type.Name == "HashTable") {
            hashTable = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
                type.Type).Cast();
            break;
        }
    }
    ASSERT_NE(hashTable, nullptr);
    ASSERT_EQ(hashTable->Fields.size(), 13u);
    EXPECT_EQ(hashTable->Fields[5].first, "OwnedBlocks");
    EXPECT_EQ(hashTable->Fields[6].first, "OwnedBlockCount");
    EXPECT_EQ(hashTable->Fields[7].first, "OwnedBlockCapacity");
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TPointerType>(
        hashTable->Fields[5].second));
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(
        hashTable->Fields[6].second));
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TIntegerType>(
        hashTable->Fields[7].second));
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}

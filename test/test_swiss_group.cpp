#include <gtest/gtest.h>

#include <qdb/kernel/lib.h>

#include "qumirdb_source_module.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

namespace {

constexpr uint64_t kMsbs = 0x8080808080808080ULL;
constexpr uint8_t kEmpty = 0x80;

std::unique_ptr<NQumir::TLLVMRunner> CompileSwissGroup(
    const std::string& entryName,
    void*& entry)
{
    auto library = NQdb::NKernel::ParseFunctionLibrary(
        NQdb::NKernel::ReadAggregationKernel("swiss_group.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
        return {};
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    NQdb::NTest::ConfigureQumirDbSourceModule(options);
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*library));
    NQdb::NTest::AddQumirDbUse(program);
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

uint64_t PackBytes(const uint8_t (&bytes)[8]) {
    uint64_t word = 0;
    for (int i = 7; i >= 0; --i) {
        word = (word << 8) | bytes[i];
    }
    return word;
}

TEST(SwissGroup, LowestIndexRecoversSlotWithinGroup) {
    void* entry = nullptr;
    auto runner = CompileSwissGroup("swiss_lowest_index", entry);
    ASSERT_NE(entry, nullptr);
    auto lowestIndex = reinterpret_cast<int64_t(*)(uint64_t)>(entry);

    for (int k = 0; k < 8; ++k) {
        const uint64_t mask = uint64_t{0x80} << (8 * k);
        EXPECT_EQ(lowestIndex(mask), k) << "single bit at slot " << k;
    }

    // With several bits set the lowest one wins.
    for (int low = 0; low < 8; ++low) {
        for (int high = low + 1; high < 8; ++high) {
            const uint64_t mask =
                (uint64_t{0x80} << (8 * low)) | (uint64_t{0x80} << (8 * high));
            EXPECT_EQ(lowestIndex(mask), low)
                << "bits at " << low << " and " << high;
        }
    }

    EXPECT_EQ(lowestIndex(kMsbs), 0) << "all slots set";
}

TEST(SwissGroup, MatchEmptyFindsExactlyTheEmptySlots) {
    void* entry = nullptr;
    auto runner = CompileSwissGroup("swiss_match_empty", entry);
    ASSERT_NE(entry, nullptr);
    auto matchEmpty = reinterpret_cast<uint64_t(*)(uint64_t)>(entry);

    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<int> h2Dist(0, 0x7F);
    for (int iteration = 0; iteration < 512; ++iteration) {
        uint8_t bytes[8];
        uint64_t expected = 0;
        for (int k = 0; k < 8; ++k) {
            const bool empty = (rng() & 1) != 0;
            bytes[k] = empty ? kEmpty : static_cast<uint8_t>(h2Dist(rng));
            if (empty) {
                expected |= uint64_t{0x80} << (8 * k);
            }
        }
        EXPECT_EQ(matchEmpty(PackBytes(bytes)), expected) << "iteration " << iteration;
    }
}

// absl's portable Match may report a false positive, but never a false
// negative and never on an empty byte -- the caller settles it with a key
// comparison. Those are exactly the properties the table relies on.
TEST(SwissGroup, MatchNeverMissesAndNeverHitsEmptySlots) {
    void* entry = nullptr;
    auto runner = CompileSwissGroup("swiss_match", entry);
    ASSERT_NE(entry, nullptr);
    auto match = reinterpret_cast<uint64_t(*)(uint64_t, uint64_t)>(entry);

    std::mt19937_64 rng(4321);
    std::uniform_int_distribution<int> h2Dist(0, 0x7F);
    for (int iteration = 0; iteration < 2048; ++iteration) {
        uint8_t bytes[8];
        for (int k = 0; k < 8; ++k) {
            bytes[k] = (rng() & 3) == 0
                ? kEmpty
                : static_cast<uint8_t>(h2Dist(rng));
        }
        const uint64_t word = PackBytes(bytes);
        const auto h2 = static_cast<uint64_t>(h2Dist(rng));
        const uint64_t mask = match(word, h2);

        EXPECT_EQ(mask & ~kMsbs, 0u) << "mask must only carry bits at 8k+7";
        for (int k = 0; k < 8; ++k) {
            const bool reported = (mask & (uint64_t{0x80} << (8 * k))) != 0;
            if (bytes[k] == h2) {
                EXPECT_TRUE(reported)
                    << "false negative at slot " << k << ", h2=" << h2;
            }
            if (bytes[k] == kEmpty) {
                EXPECT_FALSE(reported)
                    << "empty slot " << k << " must never match";
            }
        }
    }
}

TEST(SwissGroup, MatchFindsEveryH2Value) {
    void* entry = nullptr;
    auto runner = CompileSwissGroup("swiss_match", entry);
    ASSERT_NE(entry, nullptr);
    auto match = reinterpret_cast<uint64_t(*)(uint64_t, uint64_t)>(entry);

    // One full slot per group position, every legal H2, rest empty.
    for (uint64_t h2 = 0; h2 <= 0x7F; ++h2) {
        for (int k = 0; k < 8; ++k) {
            uint8_t bytes[8];
            for (auto& b : bytes) b = kEmpty;
            bytes[k] = static_cast<uint8_t>(h2);
            const uint64_t mask = match(PackBytes(bytes), h2);
            EXPECT_NE(mask & (uint64_t{0x80} << (8 * k)), 0u)
                << "h2=" << h2 << " slot=" << k;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

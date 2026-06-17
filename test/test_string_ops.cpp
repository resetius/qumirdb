#include <gtest/gtest.h>

#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace {

std::unique_ptr<NQumir::TLLVMRunner> CompileStringOperation(
    const std::string& entryName,
    void*& entry)
{
    auto library = NQqb::NKernel::ParseFunctionLibrary(
        NQqb::NKernel::ReadAggregationKernel("string_ops.oz"));
    if (!library) {
        ADD_FAILURE() << library.error().ToString();
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
        NQumir::TLocation{}, std::move(*library));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

NQqb::TStringView View(const std::string& value) {
    return {
        .Data = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
        .Size = static_cast<int64_t>(value.size()),
    };
}

TEST(QumirDbStringOps, HashHasStableByteVectors) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_hash_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto hash = reinterpret_cast<int64_t(*)(const uint8_t*, int64_t)>(entry);

    const std::string empty;
    const std::string ascii = "foobar";
    const std::string prefix = "abc";
    const std::string longer = "abcd";
    const std::string utf8 = "\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87";
    const std::string embeddedZero("a\0b", 3);
    EXPECT_EQ(static_cast<uint64_t>(hash(
        reinterpret_cast<const uint8_t*>(empty.data()), empty.size())),
        UINT64_C(0xcbf29ce484222325));
    EXPECT_EQ(static_cast<uint64_t>(hash(
        reinterpret_cast<const uint8_t*>(ascii.data()), ascii.size())),
        UINT64_C(0x85944171f73967e8));
    EXPECT_EQ(static_cast<uint64_t>(hash(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size())),
        UINT64_C(0xe71fa2190541574b));
    EXPECT_EQ(static_cast<uint64_t>(hash(
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size())),
        UINT64_C(0xfc179f83ee0724dd));
    EXPECT_EQ(static_cast<uint64_t>(hash(
        reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size())),
        UINT64_C(0x296130de6f5b7a81));
    EXPECT_EQ(static_cast<uint64_t>(hash(
        reinterpret_cast<const uint8_t*>(embeddedZero.data()), embeddedZero.size())),
        UINT64_C(0xe5d29919042666b2));
}

TEST(QumirDbStringOps, EqualityUsesBytesAndLength) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_equal_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto equal = reinterpret_cast<bool(*)(
        const uint8_t*, int64_t, const uint8_t*, int64_t)>(entry);

    const std::string prefix = "abc";
    const std::string longer = "abcd";
    const std::string same = "abc";
    const std::string embeddedZero("a\0b", 3);
    const std::string embeddedZeroDifferent("a\0c", 3);
    EXPECT_TRUE(equal(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(same.data()), same.size()));
    EXPECT_FALSE(equal(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size()));
    EXPECT_FALSE(equal(
        reinterpret_cast<const uint8_t*>(embeddedZero.data()), embeddedZero.size(),
        reinterpret_cast<const uint8_t*>(embeddedZeroDifferent.data()),
        embeddedZeroDifferent.size()));
}

TEST(QumirDbStringOps, CompareIsUnsignedByteLexicographic) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_compare_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto compare = reinterpret_cast<int64_t(*)(
        const uint8_t*, int64_t, const uint8_t*, int64_t)>(entry);

    const std::string prefix = "abc";
    const std::string longer = "abcd";
    const std::string highByte("\xC3\xA9", 2);
    const std::string ascii = "z";
    EXPECT_EQ(compare(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()), 0);
    EXPECT_LT(compare(
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(),
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size()), 0);
    EXPECT_GT(compare(
        reinterpret_cast<const uint8_t*>(longer.data()), longer.size(),
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()), 0);
    EXPECT_GT(compare(
        reinterpret_cast<const uint8_t*>(highByte.data()), highByte.size(),
        reinterpret_cast<const uint8_t*>(ascii.data()), ascii.size()), 0);
}

TEST(QumirDbStringOps, CopyPreservesEmbeddedZero) {
    void* entry = nullptr;
    auto runner = CompileStringOperation("qdb_string_copy_bytes", entry);
    ASSERT_NE(entry, nullptr);
    auto copy = reinterpret_cast<int64_t(*)(uint8_t*, const uint8_t*, int64_t)>(entry);

    const std::array<uint8_t, 4> source = {'a', 0, 'b', 0xff};
    std::array<uint8_t, 4> destination{};
    EXPECT_EQ(copy(destination.data(), source.data(), source.size()), source.size());
    EXPECT_EQ(destination, source);
}

TEST(QumirDbStringOps, ViewAndOwnedOverloadsShareContract) {
    void* hashViewEntry = nullptr;
    auto hashViewRunner = CompileStringOperation(
        "aggregation_string_hash_view", hashViewEntry);
    ASSERT_NE(hashViewEntry, nullptr);

    const std::string value("same\0bytes", 10);
    auto view = View(value);
    NQqb::TOwnedString owned{.Data = view.Data, .Size = view.Size};
    auto hashView = reinterpret_cast<int64_t(*)(NQqb::TStringView)>(hashViewEntry);
    const int64_t viewHash = hashView(view);

    void* hashOwnedEntry = nullptr;
    auto hashOwnedRunner = CompileStringOperation(
        "aggregation_string_hash_owned", hashOwnedEntry);
    ASSERT_NE(hashOwnedEntry, nullptr);
    auto hashOwned = reinterpret_cast<int64_t(*)(NQqb::TOwnedString)>(hashOwnedEntry);
    EXPECT_EQ(hashOwned(owned), viewHash);

    void* equalEntry = nullptr;
    auto equalRunner = CompileStringOperation(
        "aggregation_string_equal_view_owned", equalEntry);
    ASSERT_NE(equalEntry, nullptr);
    auto equal = reinterpret_cast<bool(*)(NQqb::TStringView, NQqb::TOwnedString)>(
        equalEntry);
    EXPECT_TRUE(equal(view, owned));
}

TEST(QumirDbStringOps, SqlLikeMatchesSqlWildcards) {
    void* likeEntry = (void*)qdb_string_view_sql_like;
    ASSERT_NE(likeEntry, nullptr);

    auto like = reinterpret_cast<int64_t(*)(NQqb::TStringView, const char*)>(
        likeEntry);

    const std::string value("same\0bytes", 10);
    auto view = View(value);

    EXPECT_EQ(like(view, "same%"), 1);
    EXPECT_EQ(like(view, "same_bytes"), 1);
    EXPECT_EQ(like(view, "%bytes"), 1);
    EXPECT_EQ(like(view, "%bytez"), 0);
    EXPECT_EQ(like(view, "same%xxx"), 0);

    EXPECT_EQ(like(view, "same"), 0);
    EXPECT_EQ(like(view, "same_bytes_"), 0);
    EXPECT_EQ(like(view, "same%xxx"), 0);
}

TEST(QumirDbStringOps, SqlLikeHandlesEmptyAndPercentOnlyPatterns) {
    void* likeEntry = (void*)qdb_string_view_sql_like;
    ASSERT_NE(likeEntry, nullptr);

    auto like = reinterpret_cast<int64_t(*)(NQqb::TStringView, const char*)>(
        likeEntry);

    EXPECT_EQ(like(View(""), ""), 1);
    EXPECT_EQ(like(View("abc"), "%"), 1);
    EXPECT_EQ(like(View(""), "%"), 1);
    EXPECT_EQ(like(View("abc"), "%%"), 1);
    EXPECT_EQ(like(View("abc"), ""), 0);
}

TEST(QumirDbStringOps, SqlLikeBacktracksOverPercent) {
    void* likeEntry = (void*)qdb_string_view_sql_like;
    ASSERT_NE(likeEntry, nullptr);

    auto like = reinterpret_cast<int64_t(*)(NQqb::TStringView, const char*)>(
        likeEntry);

    EXPECT_EQ(like(View("abbbc"), "a%c"), 1);
    EXPECT_EQ(like(View("abbbc"), "a%b%c"), 1);
    EXPECT_EQ(like(View("abbbc"), "a%bc"), 1);
    EXPECT_EQ(like(View("abbbc"), "a%d"), 0);
    EXPECT_EQ(like(View("mississippi"), "m%iss%ppi"), 1);
}

TEST(DateYear, KnownDates) {
    // 1970-01-01 = day 0
    EXPECT_EQ(qdb_date_year(0), 1970);
    // 1995-01-01 = 9131 days (used in TPC-H Q7/Q8/Q9)
    EXPECT_EQ(qdb_date_year(9131), 1995);
    // 1996-12-31 = 9861 days
    EXPECT_EQ(qdb_date_year(9861), 1996);
    // 1998-12-01 (approximate Q1 upper bound area)
    EXPECT_EQ(qdb_date_year(10471), 1998);
    // 2000-01-01 (leap year boundary)
    EXPECT_EQ(qdb_date_year(10957), 2000);
    // Negative: 1969-12-31 = -1
    EXPECT_EQ(qdb_date_year(-1), 1969);
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

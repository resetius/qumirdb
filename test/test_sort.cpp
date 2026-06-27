#include <gtest/gtest.h>

#include <stdint.h>

#include <qdb/kernel/lib.h>

#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>
#include <type_traits>
#include <vector>

namespace {

/*
123
^
 ^
  ^
example: 123, 120, 341, 145

sort by 0
0 -> 1
1 -> 1 -> 2
3 -> 1 -> 3
5 -> 1 -> 4

*/

template<typename T>
std::make_unsigned_t<T> RadixKey(T x) {
    using U = std::make_unsigned_t<T>;
    U u = std::bit_cast<U>(x);

    if constexpr (std::is_signed_v<T>) {
        u ^= (U(1) << (sizeof(T) * 8 - 1));
    }

    return u;
}

uint64_t RadixKey(double x) {
    uint64_t u = std::bit_cast<uint64_t>(x);
    uint64_t sign = u >> 63;
    uint64_t mask = (-sign) | 0x8000000000000000ULL;
    return u ^ mask;
}

template<typename T>
void CountSortIndices(T* dest, uint32_t* indices, uint32_t* work, int n, int digit)
{
    // base-16, 8 bits
    uint32_t counts[256] = {0};
    // count digits
    for (int i = 0; i < n; i++) {
        counts[(RadixKey(dest[indices[i]]) >> digit) & 0xffU]++;
    }
    // counts -> places
    for (int i = 1; i < 256; i++) {
        counts[i] += counts[i - 1];
    }

    for (int i = n-1; i >= 0; --i) {
        auto index = (RadixKey(dest[indices[i]]) >> digit) & 0xffU;
        auto place = counts[index]-1;
        work[place] = indices[i];
        counts[index]--;
    }

    for (int i = 0; i < n; i++) {
        indices[i] = work[i];
    }
}

template<typename T>
void RadixSortIndices(T* dest, uint32_t* indices, uint32_t* work, int n) {
    for (int i = 0; i < sizeof(T) * 8; i += 8) {
        CountSortIndices(dest, indices, work, n, i);
    }
}

void CountSort(uint32_t* dest, uint32_t* work, int n, int digit)
{
    // base-16, 8 bits
    uint32_t counts[256] = {0};
    // count digits
    for (int i = 0; i < n; i++) {
        counts[(dest[i] >> digit) & 0xffU]++;
    }
    // counts -> places
    for (int i = 1; i < 256; i++) {
        counts[i] += counts[i - 1];
    }

    for (int i = n-1; i >= 0; --i) {
        auto index = (dest[i] >> digit) & 0xffU;
        auto place = counts[index]-1;
        work[place] = dest[i];
        counts[index]--;
    }

    for (int i = 0; i < n; i++) {
        dest[i] = work[i];
    }
}

void RadixSort(uint32_t* dest, uint32_t* work, int n) {
    for (int i = 0; i < sizeof(uint32_t) * 8; i += 8) {
        CountSort(dest, work, n, i);
    }
}

int RandomInt(int* seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

void GenerateColumn(uint32_t* column, int n, int* seed) {
    for (int i = 0; i < n; i++) {
        column[i] = RandomInt(seed) % 15;
    }
}

std::unique_ptr<NQumir::TLLVMRunner> CompileRadixOperation(
    const std::string& entryName,
    void*& entry)
{
    std::vector<NQumir::NAst::TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) -> bool {
        auto library = NQdb::NKernel::ParseFunctionLibrary(
            NQdb::NKernel::ReadSortKernel(name));
        if (!library) {
            ADD_FAILURE() << library.error().ToString();
            return false;
        }
        for (auto& stmt : *library) {
            if (skipUse && NQumir::NAst::TMaybeNode<NQumir::NAst::TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
        return true;
    };
    if (!addLibrary("radix.oz", false) ||
        !addLibrary("radix_wrappers.oz", true)) {
        return {};
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

template<typename T>
std::vector<uint32_t> StableSortedIndices(const std::vector<T>& values) {
    std::vector<uint32_t> expected(values.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return values[lhs] < values[rhs];
    });
    return expected;
}

} // namespace

TEST(SortTest, Basic) {
    std::vector<uint32_t> ar = {2345, 123498, 123, 1, 2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    RadixSort(ar.data(), work.data(), ar.size());
    EXPECT_TRUE(std::is_sorted(ar.begin(), ar.end()));
}

TEST(SortTest, BasicIndices) {
    std::vector<uint32_t> ar = {2345, 123498, 123, 1, 2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    EXPECT_EQ(indices, StableSortedIndices(ar));
}

TEST(SortTest, BasicIndicesSigned) {
    std::vector<int32_t> ar = {-2345, 123498, -123, 1, -2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    EXPECT_EQ(indices, StableSortedIndices(ar));
}

TEST(SortTest, BasicIndicesDouble) {
    std::vector<double> ar = {-2345.6, 123498.1, -123.5, 1.2, -2.5, 555.99, 10.12344};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    EXPECT_EQ(indices, StableSortedIndices(ar));
}

TEST(SortTest, RowSet) {
    int n = 16;
    int seed = 42;
    std::vector<uint32_t> col1(n);
    std::vector<uint32_t> col2(n);
    std::vector<uint32_t> col3(n);

    std::vector<uint32_t> work; work.resize(n);
    std::vector<uint32_t> indices; indices.resize(n);
    std::iota(indices.begin(), indices.end(), 0);

    GenerateColumn(col1.data(), n, &seed);
    GenerateColumn(col2.data(), n, &seed);
    GenerateColumn(col3.data(), n, &seed);

    RadixSortIndices(col3.data(), indices.data(), work.data(), n);
    RadixSortIndices(col2.data(), indices.data(), work.data(), n);
    RadixSortIndices(col1.data(), indices.data(), work.data(), n);

    std::vector<uint32_t> expected(n);
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return std::tuple(col1[lhs], col2[lhs], col3[lhs])
            < std::tuple(col1[rhs], col2[rhs], col3[rhs]);
    });
    EXPECT_EQ(indices, expected);
}

TEST(SortRadixOz, NumericKeysMatchPrototype) {
    void* i32Entry = nullptr;
    auto i32Runner = CompileRadixOperation("qdb_radix_key_i32_test", i32Entry);
    ASSERT_NE(i32Entry, nullptr);
    auto i32Key = reinterpret_cast<uint64_t(*)(int32_t)>(i32Entry);
    EXPECT_EQ(i32Key(std::numeric_limits<int32_t>::min()), RadixKey(std::numeric_limits<int32_t>::min()));
    EXPECT_EQ(i32Key(-1), RadixKey(-1));
    EXPECT_EQ(i32Key(0), RadixKey(0));
    EXPECT_EQ(i32Key(42), RadixKey(42));
    EXPECT_EQ(i32Key(std::numeric_limits<int32_t>::max()), RadixKey(std::numeric_limits<int32_t>::max()));

    void* f64Entry = nullptr;
    auto f64Runner = CompileRadixOperation("qdb_radix_key_f64_test", f64Entry);
    ASSERT_NE(f64Entry, nullptr);
    auto f64Key = reinterpret_cast<uint64_t(*)(double)>(f64Entry);
    for (double value : {-2345.6, -0.0, 0.0, 1.2, 555.99}) {
        EXPECT_EQ(f64Key(value), RadixKey(value));
    }
}

TEST(SortRadixOz, SortsU32IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_u32_asc", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint32_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<uint32_t> values = {7, 3, 7, 1, 9, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsU8IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_u8_asc", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint8_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<uint8_t> values = {7, 3, 7, 1, 255, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsI8IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_i8_asc_test", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(int8_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<int8_t> values = {-7, 3, -7, 1, 127, 3, 0, std::numeric_limits<int8_t>::min()};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsU16IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_u16_asc", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint16_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<uint16_t> values = {700, 3, 700, 1, 65535, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsI16IndicesAscending) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_i16_asc_test", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(int16_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<int16_t> values = {-700, 3, -700, 1, 32767, 3, 0, std::numeric_limits<int16_t>::min()};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());
    EXPECT_EQ(indices, StableSortedIndices(values));
}

TEST(SortRadixOz, SortsU32IndicesDescendingStably) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_u32_desc", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(uint32_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<uint32_t> values = {7, 3, 7, 1, 9, 3, 0, 7};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());

    std::vector<uint32_t> expected(values.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::stable_sort(expected.begin(), expected.end(), [&](uint32_t lhs, uint32_t rhs) {
        return values[lhs] > values[rhs];
    });
    EXPECT_EQ(indices, expected);
}

TEST(SortRadixOz, GenericSortUsesI32RadixKeyOverload) {
    void* entry = nullptr;
    auto runner = CompileRadixOperation("qdb_radix_sort_indices_i32_asc_test", entry);
    ASSERT_NE(entry, nullptr);
    auto sort = reinterpret_cast<void(*)(int32_t*, uint32_t*, uint32_t*, uint32_t*, int64_t)>(entry);

    std::vector<int32_t> values = {-7, 3, -7, 1, 9, 3, 0, std::numeric_limits<int32_t>::min()};
    std::vector<uint32_t> indices(values.size());
    std::vector<uint32_t> work(values.size());
    std::vector<uint32_t> counts(256);
    std::iota(indices.begin(), indices.end(), 0);

    sort(values.data(), indices.data(), work.data(), counts.data(), values.size());
    EXPECT_EQ(indices, StableSortedIndices(values));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

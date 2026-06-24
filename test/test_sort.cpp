#include <gtest/gtest.h>

#include <stdint.h>

#include <vector>
#include <iostream>
#include <numeric>

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

void CountSortIndices(uint32_t* dest, uint32_t* indices, uint32_t* work, int n, int digit)
{
    // base-16, 8 bits
    uint32_t counts[256] = {0};
    // count digits
    for (int i = 0; i < n; i++) {
        counts[(dest[indices[i]] >> digit) & 0xffU]++;
    }
    // counts -> places
    for (int i = 1; i < 256; i++) {
        counts[i] += counts[i - 1];
    }

    for (int i = n-1; i >= 0; --i) {
        auto index = (dest[indices[i]] >> digit) & 0xffU;
        auto place = counts[index]-1;
        work[place] = indices[i];
        counts[index]--;
    }

    for (int i = 0; i < n; i++) {
        indices[i] = work[i];
    }
}

void RadixSortIndices(uint32_t* dest, uint32_t* indices, uint32_t* work, int n) {
    for (int i = 0; i < sizeof(uint32_t) * 8; i += 8) {
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

} // namespace

TEST(SortTest, Basic) {
    std::vector<uint32_t> ar = {2345, 123498, 123, 1, 2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    RadixSort(ar.data(), work.data(), ar.size());
    for (int i = 0; i < ar.size(); i++) {
        std::cerr << ar[i] << ", ";
    }
    std::cerr << "\n";
}

TEST(SortTest, BasicIndices) {
    std::vector<uint32_t> ar = {2345, 123498, 123, 1, 2, 555, 10};
    std::vector<uint32_t> work; work.resize(ar.size());
    std::vector<uint32_t> indices; indices.resize(ar.size());
    std::iota(indices.begin(), indices.end(), 0);
    RadixSortIndices(ar.data(), indices.data(), work.data(), ar.size());
    for (int i = 0; i < ar.size(); i++) {
        std::cerr << ar[indices[i]] << ", ";
    }
    std::cerr << "\n";
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

    for (int i = 0; i < n; i++) {
        std::cerr << col1[indices[i]] << "," << col2[indices[i]] << "," << col3[indices[i]] << "\n";
    }
    std::cerr << "\n";

    RadixSortIndices(col3.data(), indices.data(), work.data(), n);
    RadixSortIndices(col2.data(), indices.data(), work.data(), n);
    RadixSortIndices(col1.data(), indices.data(), work.data(), n);

    for (int i = 0; i < n; i++) {
        std::cerr << col1[indices[i]] << "," << col2[indices[i]] << "," << col3[indices[i]] << "\n";
    }
    std::cerr << "\n";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


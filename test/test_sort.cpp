#include <gtest/gtest.h>

#include <stdint.h>

#include <vector>
#include <iostream>

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


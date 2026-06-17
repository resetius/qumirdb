#include <qdb/modules/qumirdb_runtime.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string_view>

extern "C" {

void* qdb_alloc(int64_t size) {
    if (size < 0) {
        return nullptr;
    }
    return std::malloc(static_cast<size_t>(size));
}

void* qdb_realloc(void* ptr, int64_t size) {
    if (size < 0) {
        return nullptr;
    }
    return std::realloc(ptr, static_cast<size_t>(size));
}

void qdb_free(void* ptr) {
    std::free(ptr);
}

double qdb_bits_f64(uint64_t bits) {
    return std::bit_cast<double>(bits);
}

uint64_t qdb_f64_bits(double value) {
    return std::bit_cast<uint64_t>(value);
}

int64_t qdb_filter_string_compare(
    const uint8_t* left, int64_t leftSize,
    const uint8_t* right, int64_t rightSize)
{
    if (leftSize < 0 || rightSize < 0) {
        return (leftSize > rightSize) - (leftSize < rightSize);
    }
    const auto commonSize = static_cast<size_t>(std::min(leftSize, rightSize));
    const int compared = commonSize == 0
        ? 0
        : std::memcmp(left, right, commonSize);
    if (compared < 0) {
        return -1;
    }
    if (compared > 0) {
        return 1;
    }
    return (leftSize > rightSize) - (leftSize < rightSize);
}


int64_t qdb_string_view_sql_like(qdb_string_view str, const char* pattern)
{
    if (!pattern) {
        return 0;
    }

    const char* p = pattern;
    size_t s = 0;

    const char* last_percent = nullptr;
    size_t s_after_percent = 0;

    while (s < str.Size) {
        if (*p == '%') {
            // collapse multiple %%
            while (*p == '%') {
                ++p;
            }

            if (*p == '\0') {
                return 1; // trailing % matches everything
            }

            last_percent = p;
            s_after_percent = s;
        } else if (*p == '_' || *p == str.Data[s]) {
            ++p;
            ++s;
        } else if (last_percent) {
            // backtrack: let previous % consume one more char
            p = last_percent;
            s = ++s_after_percent;
        } else {
            return 0;
        }
    }

    // remaining pattern must be only %
    while (*p == '%') {
        ++p;
    }

    return *p == '\0' ? 1 : 0;
}

void qdb_bitmap_set_valid(uint8_t* bitmap, int64_t index, bool valid) {
    const auto byteIndex = static_cast<size_t>(index >> 3);
    const auto bit = static_cast<uint8_t>(1u << (index & 7));
    if (valid) {
        bitmap[byteIndex] |= bit;
    } else {
        bitmap[byteIndex] &= static_cast<uint8_t>(~bit);
    }
}

int64_t qdb_sql_bool_and(int64_t left, int64_t right) {
    left = left < 0 ? 1 : left;
    right = right < 0 ? 1 : right;
    if (left == 0 || right == 0) {
        return 0;
    }
    return left == 1 && right == 1 ? 1 : 2;
}

int64_t qdb_sql_bool_or(int64_t left, int64_t right) {
    left = left < 0 ? 1 : left;
    right = right < 0 ? 1 : right;
    if (left == 1 || right == 1) {
        return 1;
    }
    return left == 0 && right == 0 ? 0 : 2;
}

int64_t qdb_sql_bool_not(int64_t value) {
    value = value < 0 ? 1 : value;
    return value == 2 ? 2 : 1 - value;
}

} // extern "C"

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

int64_t qdb_string_view_cmp_cstr(const uint8_t* data, int64_t size, const char* cstr)
{
    if (!cstr) return size > 0 ? 1 : 0;
    for (int64_t i = 0; i < size; ++i) {
        if (cstr[i] == '\0') return 1;
        int diff = static_cast<int>(static_cast<uint8_t>(data[i]))
                 - static_cast<int>(static_cast<uint8_t>(cstr[i]));
        if (diff != 0) return diff;
    }
    return cstr[size] == '\0' ? 0 : -1;
}

int64_t qdb_cstr_cmp_cstr(const char* a, const char* b)
{
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return static_cast<int64_t>(std::strcmp(a, b));
}

// SQL SUBSTRING(str FROM start FOR length), 1-based start index.
qdb_string_view qdb_substring(qdb_string_view str, int32_t start, int32_t length)
{
    int64_t offset = static_cast<int64_t>(start) - 1;
    if (offset < 0) offset = 0;
    if (offset >= str.Size) return {str.Data + str.Size, 0};
    int64_t len = static_cast<int64_t>(length);
    if (len < 0) len = 0;
    if (offset + len > str.Size) len = str.Size - offset;
    return {str.Data + offset, len};
}

// Howard Hinnant's date algorithm: https://howardhinnant.github.io/date_algorithms.html
int32_t qdb_date_year(int32_t days) {
    // Shift epoch from 1970-01-01 to 0000-03-01 so leap-day falls at year end.
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097;                        // day of era  [0, 146096]
    int32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; // year of era [0, 399]
    int32_t y   = yoe + era * 400;
    int32_t doy = doe - (365*yoe + yoe/4 - yoe/100);      // day of year [0, 365]
    int32_t mp  = (5*doy + 2) / 153;                      // month [0=Mar … 11=Feb]
    y += (mp >= 10);                                       // Jan/Feb belong to next year
    return y;
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

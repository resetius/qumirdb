#include <qdb/modules/qumirdb_runtime.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<qdb_bin_int>);
static_assert(std::is_standard_layout_v<qdb_bin_int>);
static_assert(sizeof(qdb_bin_int) == 16);

namespace {

__int128 ToI128(qdb_bin_int value) {
    const unsigned __int128 raw = static_cast<unsigned __int128>(value.Lo) |
        (static_cast<unsigned __int128>(value.Hi) << 64);
    return static_cast<__int128>(raw);
}

qdb_bin_int FromI128(__int128 value) {
    const auto raw = static_cast<unsigned __int128>(value);
    return {
        .Lo = static_cast<uint64_t>(raw),
        .Hi = static_cast<uint64_t>(raw >> 64),
    };
}

__int128 Pow10I128(int64_t scale) {
    if (scale < 0) {
        return 0;
    }
    __int128 value = 1;
    for (int64_t i = 0; i < scale; ++i) {
        value *= 10;
    }
    return value;
}

} // namespace

extern "C" {

void* qdb_alloc(int64_t size) {
    if (size < 0) {
        return nullptr;
    }
    return std::malloc(static_cast<size_t>(size));
}

// Note: qdb_realloc is intentionally NOT provided here. It is implemented in Oz
// (qumirdb.oz) on top of qdb_alloc/qdb_free, taking the old size so it copies
// only the live bytes.

void qdb_free(void* ptr) {
    std::free(ptr);
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


// Size-aware LIKE: the pattern is a StringView (Size-delimited, not null-terminated).
int64_t qdb_string_view_sql_like(qdb_string_view str, qdb_string_view pattern)
{
    const uint8_t* p = pattern.Data;
    const uint8_t* pend = pattern.Data + pattern.Size;
    int64_t s = 0;

    const uint8_t* last_percent = nullptr;
    int64_t s_after_percent = 0;

    while (s < str.Size) {
        if (p < pend && *p == '%') {
            // collapse multiple %%
            while (p < pend && *p == '%') {
                ++p;
            }

            if (p == pend) {
                return 1; // trailing % matches everything
            }

            last_percent = p;
            s_after_percent = s;
        } else if (p < pend && (*p == '_' || *p == str.Data[s])) {
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
    while (p < pend && *p == '%') {
        ++p;
    }

    return p == pend ? 1 : 0;
}

// TODO: needs qumir changes. A string literal passed by value into a generic
// Nullable[StringView] operator otherwise materializes via str_from_lit (a qumir
// string-runtime symbol not linked into JIT kernels). This cast wraps the literal's
// char* as a StringView directly, so nullable string comparisons work without it.
qdb_string_view qdb_lit_to_sv(const char* lit) {
    return {
        reinterpret_cast<const uint8_t*>(lit),
        static_cast<int64_t>(std::strlen(lit))
    };
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

static int32_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int32_t>(era * 146097 + doe - 719468);
}

// Size-aware: CAST(<string column> AS DATE) passes a non-null-terminated StringView.
int32_t qdb_sql_date(qdb_string_view date) {
    int parts[3] = {0, 0, 0};
    int idx = 0;
    for (int64_t i = 0; i < date.Size && idx < 3; ++i) {
        const char c = static_cast<char>(date.Data[i]);
        if (c == '-') {
            ++idx;
        } else if (c >= '0' && c <= '9') {
            parts[idx] = parts[idx] * 10 + (c - '0');
        }
    }
    return DaysFromCivil(parts[0], parts[1], parts[2]);
}

// year/month are approximate (365/30): the base date is not available here for
// calendar-exact arithmetic.
int32_t qdb_sql_interval(qdb_string_view amount, qdb_string_view unit) {
    int n = 0;
    bool neg = false;
    for (int64_t i = 0; i < amount.Size; ++i) {
        const char c = static_cast<char>(amount.Data[i]);
        if (c == '-') {
            neg = true;
        } else if (c >= '0' && c <= '9') {
            n = n * 10 + (c - '0');
        }
    }
    if (neg) {
        n = -n;
    }
    std::string_view u(reinterpret_cast<const char*>(unit.Data),
                       static_cast<size_t>(unit.Size));
    if (u == "year" || u == "years") {
        return n * 365;
    }
    if (u == "month" || u == "months") {
        return n * 30;
    }
    return n;
}

// SQL ROUND: round half away from zero to `digits` decimals.
double qdb_round(double value, int32_t digits) {
    double factor = std::pow(10.0, digits);
    return std::round(value * factor) / factor;
}

qdb_bin_int qdb_decimal_from_i64(int64_t value, int64_t scale) {
    return FromI128(static_cast<__int128>(value) * Pow10I128(scale));
}

qdb_bin_int qdb_decimal_from_f64(double value, int64_t scale) {
    const long double factor = std::powl(10.0L, static_cast<long double>(scale));
    const long double rounded = std::roundl(static_cast<long double>(value) * factor);
    return FromI128(static_cast<__int128>(rounded));
}

qdb_bin_int qdb_decimal_scale_up(qdb_bin_int value, int64_t delta) {
    return FromI128(ToI128(value) * Pow10I128(delta));
}

qdb_bin_int qdb_decimal_add(qdb_bin_int left, qdb_bin_int right) {
    return FromI128(ToI128(left) + ToI128(right));
}

qdb_bin_int qdb_decimal_sub(qdb_bin_int left, qdb_bin_int right) {
    return FromI128(ToI128(left) - ToI128(right));
}

qdb_bin_int qdb_decimal_neg(qdb_bin_int value) {
    return FromI128(-ToI128(value));
}

qdb_bin_int qdb_decimal_mul_i64(qdb_bin_int left, int64_t right) {
    return FromI128(ToI128(left) * static_cast<__int128>(right));
}

qdb_bin_int qdb_decimal_div_i64(qdb_bin_int left, int64_t right) {
    return FromI128(ToI128(left) / static_cast<__int128>(right));
}

bool qdb_decimal_eq(qdb_bin_int left, qdb_bin_int right) {
    return ToI128(left) == ToI128(right);
}

bool qdb_decimal_ne(qdb_bin_int left, qdb_bin_int right) {
    return ToI128(left) != ToI128(right);
}

bool qdb_decimal_lt(qdb_bin_int left, qdb_bin_int right) {
    return ToI128(left) < ToI128(right);
}

bool qdb_decimal_le(qdb_bin_int left, qdb_bin_int right) {
    return ToI128(left) <= ToI128(right);
}

bool qdb_decimal_gt(qdb_bin_int left, qdb_bin_int right) {
    return ToI128(left) > ToI128(right);
}

bool qdb_decimal_ge(qdb_bin_int left, qdb_bin_int right) {
    return ToI128(left) >= ToI128(right);
}

} // extern "C"

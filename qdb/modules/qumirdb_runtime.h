#pragma once

#include <cstdint>
#include <string_view>

extern "C" {

// Backing allocator for qdb-generated kernels (e.g. the Robin Hood hash
// table used by the Aggregation operator). Thin wrappers around malloc
// family so they can be registered as external functions in the QumirDb
// module and resolved by the LLVM JIT via the process symbol table.
void* qdb_alloc(int64_t size);
void qdb_free(void* ptr);

int64_t qdb_filter_string_compare(
    const uint8_t* left, int64_t leftSize,
    const uint8_t* right, int64_t rightSize);

struct qdb_string_view {
    const uint8_t* Data;
    int64_t Size;
};

int64_t qdb_string_view_sql_like(qdb_string_view str, qdb_string_view pattern);

// TODO: needs qumir changes — temporary cast so string literals reach nullable
// StringView operators without str_from_lit (not linked in JIT kernels).
qdb_string_view qdb_lit_to_sv(const char* lit);

qdb_string_view qdb_substring(qdb_string_view str, int32_t start, int32_t length);

// Returns the Gregorian year for a date given as days since 1970-01-01.
int32_t qdb_date_year(int32_t days);

// SQL DATE '<yyyy-mm-dd>' literal -> days since 1970-01-01.
int32_t qdb_sql_date(qdb_string_view date);

// SQL INTERVAL '<amount>' <unit> -> days (year/month approximate: 365/30).
int32_t qdb_sql_interval(qdb_string_view amount, qdb_string_view unit);

// SQL ROUND(x, digits) -> round half away from zero.
double qdb_round(double value, int32_t digits);

} // extern "C"

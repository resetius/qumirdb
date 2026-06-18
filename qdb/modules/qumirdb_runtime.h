#pragma once

#include <cstdint>
#include <string_view>

extern "C" {

// Backing allocator for qdb-generated kernels (e.g. the Robin Hood hash
// table used by the Aggregation operator). Thin wrappers around malloc
// family so they can be registered as external functions in the QumirDb
// module and resolved by the LLVM JIT via the process symbol table.
void* qdb_alloc(int64_t size);
void* qdb_realloc(void* ptr, int64_t size);
void qdb_free(void* ptr);

// Temporary core-language bitcast primitives. See
// docs/issues/qumir_missing_scalar_bitcast.md.
uint64_t qdb_f64_bits(double value);
double qdb_bits_f64(uint64_t bits);

int64_t qdb_filter_string_compare(
    const uint8_t* left, int64_t leftSize,
    const uint8_t* right, int64_t rightSize);

struct qdb_string_view {
    const uint8_t* Data;
    int64_t Size;
};

int64_t qdb_string_view_sql_like(qdb_string_view str, const char* pattern);
int64_t qdb_string_view_cmp_cstr(const uint8_t* data, int64_t size, const char* cstr);
int64_t qdb_cstr_cmp_cstr(const char* a, const char* b);

qdb_string_view qdb_substring(qdb_string_view str, int32_t start, int32_t length);

// Returns the Gregorian year for a date given as days since 1970-01-01.
int32_t qdb_date_year(int32_t days);

void qdb_bitmap_set_valid(uint8_t* bitmap, int64_t index, bool valid);

int64_t qdb_sql_bool_and(int64_t left, int64_t right);
int64_t qdb_sql_bool_or(int64_t left, int64_t right);
int64_t qdb_sql_bool_not(int64_t value);

} // extern "C"

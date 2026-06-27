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

bool qdb_sv_lit_eq(qdb_string_view left, const char* right);
bool qdb_sv_lit_ne(qdb_string_view left, const char* right);
bool qdb_sv_lit_lt(qdb_string_view left, const char* right);
bool qdb_sv_lit_le(qdb_string_view left, const char* right);
bool qdb_sv_lit_gt(qdb_string_view left, const char* right);
bool qdb_sv_lit_ge(qdb_string_view left, const char* right);

bool qdb_lit_sv_eq(const char* left, qdb_string_view right);
bool qdb_lit_sv_ne(const char* left, qdb_string_view right);
bool qdb_lit_sv_lt(const char* left, qdb_string_view right);
bool qdb_lit_sv_le(const char* left, qdb_string_view right);
bool qdb_lit_sv_gt(const char* left, qdb_string_view right);
bool qdb_lit_sv_ge(const char* left, qdb_string_view right);

bool qdb_lit_lit_eq(const char* left, const char* right);
bool qdb_lit_lit_ne(const char* left, const char* right);
bool qdb_lit_lit_lt(const char* left, const char* right);
bool qdb_lit_lit_le(const char* left, const char* right);
bool qdb_lit_lit_gt(const char* left, const char* right);
bool qdb_lit_lit_ge(const char* left, const char* right);

qdb_string_view qdb_substring(qdb_string_view str, int32_t start, int32_t length);

// Returns the Gregorian year for a date given as days since 1970-01-01.
int32_t qdb_date_year(int32_t days);

// SQL DATE '<yyyy-mm-dd>' literal -> days since 1970-01-01.
int32_t qdb_sql_date(const char* date);

// SQL INTERVAL '<amount>' <unit> -> days (year/month approximate: 365/30).
int32_t qdb_sql_interval(const char* amount, const char* unit);

void qdb_bitmap_set_valid(uint8_t* bitmap, int64_t index, bool valid);

int64_t qdb_sql_bool_and(int64_t left, int64_t right);
int64_t qdb_sql_bool_or(int64_t left, int64_t right);
int64_t qdb_sql_bool_not(int64_t value);

} // extern "C"

#pragma once

#include <cstdint>

extern "C" {

// Backing allocator for qdb-generated kernels (e.g. the Robin Hood hash
// table used by the Aggregation operator). Thin wrappers around malloc
// family so they can be registered as external functions in the QumirDb
// module and resolved by the LLVM JIT via the process symbol table.
void* qdb_alloc(int64_t size);
void* qdb_realloc(void* ptr, int64_t size);
void qdb_free(void* ptr);

// Temporary core-language bitcast primitive. See
// docs/issues/qumir_missing_scalar_bitcast.md.
uint64_t qdb_f64_bits(double value);

int64_t qdb_filter_string_compare(
    const uint8_t* left, int64_t leftSize,
    const uint8_t* right, int64_t rightSize);

void qdb_bitmap_set_valid(uint8_t* bitmap, int64_t index, bool valid);

int64_t qdb_sql_bool_and(int64_t left, int64_t right);
int64_t qdb_sql_bool_or(int64_t left, int64_t right);
int64_t qdb_sql_bool_not(int64_t value);

} // extern "C"

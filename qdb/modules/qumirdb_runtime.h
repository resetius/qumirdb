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

} // extern "C"

#include <qdb/modules/qumirdb_runtime.h>

#include <cstdlib>

extern "C" {

void* qdb_alloc(int64_t size) {
    return std::malloc(static_cast<size_t>(size));
}

void* qdb_realloc(void* ptr, int64_t size) {
    return std::realloc(ptr, static_cast<size_t>(size));
}

void qdb_free(void* ptr) {
    std::free(ptr);
}

} // extern "C"

#include <qdb/modules/qumirdb_runtime.h>

#include <bit>
#include <cstdlib>

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

uint64_t qdb_f64_bits(double value) {
    return std::bit_cast<uint64_t>(value);
}

} // extern "C"

#pragma once

#include <cstdint>
#include <unordered_set>
#include <string>

#include <qdb/io/schema.h>
#include <qdb/plan/ops/stats.h>

namespace NQdb {

struct TColumn {
    char* Data;
    int32_t DataBitOffset = 0; // bit offset for bitmap-backed columns (bool)
    const uint8_t* Mask; // Arrow null bitmap; nullptr if all valid
    int32_t MaskBitOffset = 0;
    void* Offsets; // raw offsets buffer for variable-length types; null for fixed-width
    uint8_t OffsetWidth; // 4 for STRING, 8 for LARGE_STRING, 0 otherwise
};

struct TRowSet {
    TColumn* Columns;
    int64_t ColumnCount;
    int64_t RowCount;
    uint8_t* Selection; // nullptr = all rows selected; Selection[i]==0 skips row i
    uint64_t* Hash; // nullptr = not precomputed; Hash[i] is a shuffle-consistent rh_hash of row i's key

    void (*Destroy)(TRowSet*);
    void* Private;
    int64_t RefCount;
};

inline void Retain(TRowSet* rs) {
    ++rs->RefCount;
}

inline void Release(TRowSet* rs) {
    if (--rs->RefCount == 0 && rs->Destroy) {
        rs->Destroy(rs);
    }
}

struct ISource {
    virtual ~ISource() = default;
    virtual const TSchema& Schema() const = 0;
    virtual const TStatsPtr Stats() const = 0;
    virtual bool Next(TRowSet& rowSet) = 0;
    virtual void RestrictColumns(const std::unordered_set<std::string>& names) {}
};

struct ISink {
    virtual ~ISink() = default;
    virtual void Write(const TRowSet& rowSet) = 0;
    virtual void Flush() {}
};

} // namespace NQdb

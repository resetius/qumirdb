#pragma once

#include <cstdint>
#include <unordered_set>
#include <string>

#include <qdb/io/schema.h>

namespace NQqb {

struct TColumn {
    char* Data;
    char* Mask; // nulls
    int64_t* Offsets; // for variable length types
};

struct TRowSet {
    TColumn* Columns;
    int32_t ColumnCount;
    int32_t RowCount;
    bool* Selection; // nullptr = all rows selected; Selection[i]==false skips row i

    void (*Destroy)(TRowSet*);
    void* Private;
    int32_t RefCount;
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
    virtual bool Next(TRowSet& rowSet) = 0;
    virtual void RestrictColumns(const std::unordered_set<std::string>& names) {}
};

struct ISink {
    virtual ~ISink() = default;
    virtual void Write(const TRowSet& rowSet) = 0;
    virtual void Flush() {}
};

} // namespace NQqb

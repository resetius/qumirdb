#pragma once

#include <qdb/io/io.h>

#include <cstdint>
#include <vector>

namespace NQdb {

class TLateMaterializeProcessor {
public:
    TLateMaterializeProcessor(
        std::shared_ptr<const IPhysicalRowReader> reader,
        int32_t locatorColumn);

    void Add(TRowSet& rowSet);
    bool Next(TRowSet& output);

private:
    std::shared_ptr<const IPhysicalRowReader> Reader_;
    int32_t LocatorColumn_ = 0;
    std::vector<TPhysicalRowId> RowIds_;
    bool Finished_ = false;
};

// Join adjacent column ranges produced by parallel lookup tasks without
// copying their buffers.
void MergeLateMaterializedColumns(
    std::vector<TRowSet>& partitions,
    TRowSet& output);

} // namespace NQdb

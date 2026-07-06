#pragma once

#include <qdb/scheduler/settings.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NQdb {
namespace NScheduler {

struct TScanRowGroup {
    size_t RowGroup = 0;
    int64_t RowCount = 0;
    int64_t ByteSize = 0;
};

struct TScanSplit {
    size_t FirstRowGroup = 0;
    size_t RowGroupCount = 0;
    int64_t RowCount = 0;
    int64_t ByteSize = 0;
    bool SerialRead = false;
};

class IScanMetadataSource {
public:
    virtual ~IScanMetadataSource() = default;
    virtual std::vector<TScanRowGroup> ScanRowGroups() const = 0;
};

std::vector<TScanSplit> BuildScanSplits(
    const std::vector<TScanRowGroup>& rowGroups,
    const TScanSplitSettings& settings);

} // namespace NScheduler
} // namespace NQdb

#include <qdb/scheduler/scan_split.h>

#include <algorithm>

namespace NQdb {
namespace NScheduler {
namespace {

TScanSplit MakeSingleSplit(
    const std::vector<TScanRowGroup>& rowGroups,
    bool serialRead)
{
    TScanSplit split;
    split.FirstRowGroup = rowGroups.front().RowGroup;
    split.RowGroupCount = rowGroups.size();
    split.SerialRead = serialRead;
    for (const auto& rowGroup : rowGroups) {
        split.RowCount += rowGroup.RowCount;
        split.ByteSize += rowGroup.ByteSize;
    }
    return split;
}

} // namespace

std::vector<TScanSplit> BuildScanSplits(
    const std::vector<TScanRowGroup>& rowGroups,
    const TScanSplitSettings& settings)
{
    if (rowGroups.empty()) {
        return {};
    }

    auto totalRows = int64_t{0};
    for (const auto& rowGroup : rowGroups) {
        totalRows += rowGroup.RowCount;
    }

    const auto maxTasks = std::max<size_t>(settings.MaxScanTasks, 1);
    if (
        settings.Strategy == EScanSplitStrategy::SerialRead ||
        maxTasks == 1 ||
        (
            settings.TinyInputRowsThreshold &&
            totalRows <= static_cast<int64_t>(settings.TinyInputRowsThreshold)))
    {
        return {MakeSingleSplit(rowGroups, true)};
    }

    // Split the row groups into up to maxTasks evenly-sized, contiguous chunks
    // of ceil(n / m) groups each. The last chunk may be slightly smaller, which
    // is fine — an even split keeps every worker busy. (The previous target/byte
    // heuristic degenerated to 15 one-group splits plus one giant split when no
    // target was configured, serialising the whole scan onto a single worker.)
    const size_t groupCount = rowGroups.size();
    const size_t taskCount = std::min(maxTasks, groupCount);
    const size_t groupsPerTask = (groupCount + taskCount - 1) / taskCount;

    std::vector<TScanSplit> splits;
    splits.reserve(taskCount);
    for (size_t begin = 0; begin < groupCount; begin += groupsPerTask) {
        const size_t end = std::min(begin + groupsPerTask, groupCount);
        TScanSplit split;
        split.FirstRowGroup = rowGroups[begin].RowGroup;
        split.RowGroupCount = end - begin;
        for (size_t i = begin; i < end; ++i) {
            split.RowCount += rowGroups[i].RowCount;
            split.ByteSize += rowGroups[i].ByteSize;
        }
        splits.push_back(split);
    }
    return splits;
}

} // namespace NScheduler
} // namespace NQdb

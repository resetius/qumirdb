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

bool ReachesTarget(
    const TScanSplit& split,
    const TScanSplitSettings& settings)
{
    if (
        settings.TargetRowsPerTask &&
        split.RowCount >= static_cast<int64_t>(settings.TargetRowsPerTask))
    {
        return true;
    }
    if (
        settings.TargetBytesPerTask &&
        split.ByteSize >= static_cast<int64_t>(settings.TargetBytesPerTask))
    {
        return true;
    }
    return false;
}

bool HasTarget(const TScanSplitSettings& settings)
{
    return settings.TargetRowsPerTask || settings.TargetBytesPerTask;
}

bool ReachesMinimum(
    const TScanSplit& split,
    const TScanSplitSettings& settings)
{
    return !settings.MinRowsPerTask ||
        split.RowCount >= static_cast<int64_t>(settings.MinRowsPerTask);
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

    std::vector<TScanSplit> splits;
    splits.reserve(std::min(maxTasks, rowGroups.size()));

    const auto coalescingFactor = std::max<size_t>(
        settings.RowGroupCoalescingFactor,
        1);
    TScanSplit current;
    for (size_t index = 0; index < rowGroups.size(); ++index) {
        const auto& rowGroup = rowGroups[index];
        if (!current.RowGroupCount) {
            current.FirstRowGroup = rowGroup.RowGroup;
        }
        ++current.RowGroupCount;
        current.RowCount += rowGroup.RowCount;
        current.ByteSize += rowGroup.ByteSize;

        const auto remainingGroups = rowGroups.size() - index - 1;
        const auto remainingSplitSlots = maxTasks - splits.size() - 1;
        if (!remainingSplitSlots && remainingGroups) {
            continue;
        }

        const auto reachesCoalescing = current.RowGroupCount >= coalescingFactor;
        if (
            reachesCoalescing &&
            ReachesMinimum(current, settings) &&
            (
                ReachesTarget(current, settings) ||
                !HasTarget(settings) ||
                !remainingGroups))
        {
            splits.push_back(current);
            current = {};
        }
    }

    if (current.RowGroupCount) {
        splits.push_back(current);
    }
    return splits;
}

} // namespace NScheduler
} // namespace NQdb

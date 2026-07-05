#pragma once

#include <cstddef>

namespace NQdb {
namespace NScheduler {

enum class EExecutionMode {
    SingleThreadedScheduler = 0,
    ThreadedScheduler = 1,
};

enum class EScanSplitStrategy {
    Auto = 0,
    RowGroupRange = 1,
    SerialRead = 2,
};

struct TSchedulerSettings {
    EExecutionMode Mode = EExecutionMode::SingleThreadedScheduler;
    size_t WorkerCount = 1;
    size_t ReadyQueueCapacity = 1024;
};

struct TQueueSettings {
    // Rowset slots per connection lane. Depth 1 ping-pongs producer/consumer on
    // every batch; a small buffer (measured sweet spot ~4) lets a producer run
    // ahead, cutting handoff/scheduling overhead (~10-17% on TPC-H at 8-16
    // workers) with negligible memory cost. Override with --queue-depth.
    size_t RowsetCapacityPerLane = 4;
    bool EnableDebugCounters = false;
};

struct TScanSplitSettings {
    EScanSplitStrategy Strategy = EScanSplitStrategy::Auto;
    size_t TargetRowsPerTask = 0;
    size_t TargetBytesPerTask = 0;
    size_t MinRowsPerTask = 0;
    size_t MaxScanTasks = 1;
    size_t RowGroupCoalescingFactor = 1;
    size_t TinyInputRowsThreshold = 0;
};

struct THashShuffleSettings {
    size_t PartitionCount = 0;
    size_t MaxPartitionCount = 0;
    size_t MaxQueuedRowsetsPerLane = 1;
    size_t TargetOutputBatchRows = 16 * 1024;
    size_t MaxOutputBatchRows = 64 * 1024;
    size_t TargetOutputBatchBytes = 1024 * 1024;
};

struct TAggregateSettings {
    // Parallelize ungrouped/global aggregates with a partial->gather->combine
    // cascade instead of gathering every lane into one aggregate. Off by
    // default: the single-aggregate path is faster for cheap aggregates; the
    // cascade only pays off when the serial aggregate is the bottleneck (very
    // large inputs).
    bool CascadeGlobal = false;
};

struct TSettings {
    TSchedulerSettings Scheduler;
    TQueueSettings Queue;
    TScanSplitSettings ScanSplit;
    THashShuffleSettings HashShuffle;
    TAggregateSettings Aggregate;
};

} // namespace NScheduler
} // namespace NQdb

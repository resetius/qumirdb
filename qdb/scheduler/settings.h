#pragma once

#include <cstddef>

namespace NQdb {
namespace NScheduler {

enum class EExecutionMode {
    Serial = 0,
    SingleThreadedScheduler = 1,
    ThreadedScheduler = 2,
};

enum class EScanSplitStrategy {
    Auto = 0,
    RowGroupRange = 1,
    SerialRead = 2,
};

struct TSchedulerSettings {
    EExecutionMode Mode = EExecutionMode::Serial;
    size_t WorkerCount = 1;
    size_t ReadyQueueCapacity = 1024;
};

struct TQueueSettings {
    size_t RowsetCapacityPerLane = 1;
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
